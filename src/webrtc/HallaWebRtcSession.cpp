#include "HallaWebRtcSession.h"
#include "net/NetSession.h"
#include "core/AppLog.h"
#include "core/Settings.h"
#include "MediaFoundationH264.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonValue>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QMutexLocker>
#include <QPixmap>
#include <QScreen>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#if defined(HALLA_WEBRTC_NATIVE) && __has_include(<audioclientactivationparams.h>)
#define HALLA_PROCESS_LOOPBACK_EXCLUSION 1
#include <audioclientactivationparams.h>
#include <wrl/implements.h>
#endif
#endif

#ifdef HALLA_WEBRTC_NATIVE
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using nullptr_t = std::nullptr_t;
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/audio/audio_device_defines.h"
#include "modules/audio_device/include/audio_device_default.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "modules/video_coding/codecs/vp8/include/vp8.h"
#include "libyuv/convert.h"
#include "libyuv/convert_from.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#endif

#ifdef HALLA_WEBRTC_NATIVE
namespace {
#ifdef Q_OS_WIN
class DxgiScreenCapturer {
public:
    QImage grab(int screenIndex) {
        if (!ensureInitialized(screenIndex)) return {};
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        Microsoft::WRL::ComPtr<IDXGIResource> resource;
        const HRESULT hr = m_duplication->AcquireNextFrame(0, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return m_lastFrame;
        if (isDeviceLost(hr)) { reset(); return {}; }
        if (FAILED(hr)) return {};

        QImage out;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.As(&texture)) && texture) out = textureToImage(texture.Get());
        m_duplication->ReleaseFrame();
        if (!out.isNull()) m_lastFrame = out;
        return out;
    }

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> grabGpu(
            int screenIndex, int maxWidth, int maxHeight, int fps,
            QImage* preview, bool makePreview) {
        if (preview) *preview = {};
        if (!ensureInitialized(screenIndex)) return nullptr;

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        Microsoft::WRL::ComPtr<IDXGIResource> resource;
        const HRESULT acquireHr = m_duplication->AcquireNextFrame(0, &frameInfo, &resource);
        if (acquireHr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (preview && makePreview) *preview = m_lastFrame;
            return m_lastNative;
        }
        if (isDeviceLost(acquireHr)) { reset(); return nullptr; }
        if (FAILED(acquireHr)) return nullptr;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
        HRESULT hr = resource.As(&source);
        D3D11_TEXTURE2D_DESC sourceDesc{};
        if (SUCCEEDED(hr) && source) source->GetDesc(&sourceDesc);
        if (SUCCEEDED(hr) && source && makePreview) {
            QImage image = textureToImage(source.Get());
            if (!image.isNull()) {
                m_lastFrame = image;
                if (preview) *preview = image;
            }
        }

        int outputWidth = 0;
        int outputHeight = 0;
        fitWithinEven(int(sourceDesc.Width), int(sourceDesc.Height),
                      maxWidth, maxHeight, outputWidth, outputHeight);
        if (SUCCEEDED(hr))
            hr = ensureVideoProcessor(sourceDesc, outputWidth, outputHeight, fps);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> output;
        std::shared_ptr<TexturePool> pool;
        size_t slot = 0;
        if (SUCCEEDED(hr)) hr = acquireOutputTexture(outputWidth, outputHeight, output, pool, slot);

        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView;
        if (SUCCEEDED(hr)) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
            inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            inputDesc.Texture2D.MipSlice = 0;
            inputDesc.Texture2D.ArraySlice = 0;
            hr = m_videoDevice->CreateVideoProcessorInputView(
                source.Get(), m_processorEnumerator.Get(), &inputDesc, &inputView);
        }
        if (SUCCEEDED(hr)) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
            outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            outputDesc.Texture2D.MipSlice = 0;
            hr = m_videoDevice->CreateVideoProcessorOutputView(
                output.Get(), m_processorEnumerator.Get(), &outputDesc, &outputView);
        }
        if (SUCCEEDED(hr)) {
            const RECT sourceRect{0, 0, LONG(sourceDesc.Width), LONG(sourceDesc.Height)};
            const RECT outputRect{0, 0, LONG(outputWidth), LONG(outputHeight)};
            m_videoContext->VideoProcessorSetOutputTargetRect(
                m_videoProcessor.Get(), TRUE, &outputRect);
            m_videoContext->VideoProcessorSetStreamFrameFormat(
                m_videoProcessor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            m_videoContext->VideoProcessorSetStreamSourceRect(
                m_videoProcessor.Get(), 0, TRUE, &sourceRect);
            m_videoContext->VideoProcessorSetStreamDestRect(
                m_videoProcessor.Get(), 0, TRUE, &outputRect);
            m_videoContext->VideoProcessorSetStreamAutoProcessingMode(
                m_videoProcessor.Get(), 0, FALSE);
            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE;
            stream.pInputSurface = inputView.Get();
            hr = m_videoContext->VideoProcessorBlt(
                m_videoProcessor.Get(), outputView.Get(), m_frameNumber++, 1, &stream);
        }
        m_duplication->ReleaseFrame();

        if (FAILED(hr) || !output || !pool) {
            releasePoolSlot(pool, slot);
            if (!m_gpuFailureLogged) {
                m_gpuFailureLogged = true;
                AppLog::warn(QStringLiteral(
                    "WebRTC DXGI: conversão GPU BGRA->NV12 indisponível (0x%1); usando caminho pela CPU")
                    .arg(quint32(hr), 8, 16, QLatin1Char('0')));
            }
            return nullptr;
        }

        auto native = HallaMfH264::createD3D11FrameBuffer(
            output.Get(), outputWidth, outputHeight, [pool, slot] {
                releasePoolSlot(pool, slot);
            });
        if (!native) {
            releasePoolSlot(pool, slot);
            return nullptr;
        }
        m_lastNative = native;
        if (!m_gpuPathLogged) {
            m_gpuPathLogged = true;
            AppLog::info(QStringLiteral(
                "WebRTC DXGI: captura, escala e conversão BGRA->NV12 na GPU ativas (%1x%2 @ %3 FPS)")
                .arg(outputWidth).arg(outputHeight).arg(fps));
        }
        return native;
    }

private:
    struct TexturePool {
        struct Slot {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            bool busy = false;
        };
        std::mutex mutex;
        std::vector<Slot> slots;
        int width = 0;
        int height = 0;
    };

    static void releasePoolSlot(const std::shared_ptr<TexturePool>& pool, size_t slot) {
        if (!pool) return;
        std::lock_guard<std::mutex> lock(pool->mutex);
        if (slot < pool->slots.size()) pool->slots[slot].busy = false;
    }

    static bool isDeviceLost(HRESULT hr) {
        return hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED
            || hr == DXGI_ERROR_DEVICE_RESET;
    }

    static void fitWithinEven(int sourceWidth, int sourceHeight,
                              int maxWidth, int maxHeight,
                              int& outputWidth, int& outputHeight) {
        sourceWidth = std::max(2, sourceWidth);
        sourceHeight = std::max(2, sourceHeight);
        maxWidth = std::max(2, maxWidth);
        maxHeight = std::max(2, maxHeight);
        const double scale = std::min({1.0, double(maxWidth) / sourceWidth,
                                      double(maxHeight) / sourceHeight});
        outputWidth = std::max(2, int(sourceWidth * scale)) & ~1;
        outputHeight = std::max(2, int(sourceHeight * scale)) & ~1;
    }

    bool ensureInitialized(int screenIndex) {
        if (screenIndex < 0) screenIndex = 0;
        if (m_duplication && m_screenIndex == screenIndex) return true;
        reset();
        return init(screenIndex);
    }

    bool init(int screenIndex) {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                     reinterpret_cast<void**>(factory.GetAddressOf()))))
            return false;

        Microsoft::WRL::ComPtr<IDXGIAdapter1> selectedAdapter;
        Microsoft::WRL::ComPtr<IDXGIOutput> selectedOutput;
        int current = 0;
        for (UINT adapterIndex = 0; ; ++adapterIndex) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT oi = 0; ; ++oi) {
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;
                if (current == screenIndex || !selectedOutput) {
                    selectedAdapter = adapter;
                    selectedOutput = output;
                    if (current == screenIndex) break;
                }
                ++current;
            }
            if (selectedOutput && current == screenIndex) break;
        }
        if (!selectedAdapter || !selectedOutput) return false;

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT hr = D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       flags, levels, UINT(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                                       &m_device, &level, &m_context);
        if (FAILED(hr)) {
            flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            hr = D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   flags, levels, UINT(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                                   &m_device, &level, &m_context);
        }
        if (FAILED(hr)) return false;

        Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
        if (m_context && SUCCEEDED(m_context.As(&multithread)) && multithread)
            multithread->SetMultithreadProtected(TRUE);
        m_device.As(&m_videoDevice);
        m_context.As(&m_videoContext);

        Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
        if (FAILED(selectedOutput.As(&output1))) return false;
        if (FAILED(output1->DuplicateOutput(m_device.Get(), &m_duplication))) {
            reset();
            return false;
        }
        DXGI_ADAPTER_DESC1 adapterDesc{};
        selectedAdapter->GetDesc1(&adapterDesc);
        m_screenIndex = screenIndex;
        AppLog::info(QStringLiteral(
            "WebRTC DXGI Desktop Duplication ativo na tela #%1 — GPU: %2 (VEN_%3)")
            .arg(screenIndex)
            .arg(QString::fromWCharArray(adapterDesc.Description))
            .arg(adapterDesc.VendorId, 4, 16, QLatin1Char('0')));
        return true;
    }

    HRESULT ensureVideoProcessor(const D3D11_TEXTURE2D_DESC& sourceDesc,
                                 int outputWidth, int outputHeight, int fps) {
        if (!m_videoDevice || !m_videoContext || outputWidth < 2 || outputHeight < 2)
            return E_NOINTERFACE;
        if (m_processorEnumerator && m_videoProcessor
                && m_processorSourceWidth == sourceDesc.Width
                && m_processorSourceHeight == sourceDesc.Height
                && m_processorSourceFormat == sourceDesc.Format
                && m_processorOutputWidth == UINT(outputWidth)
                && m_processorOutputHeight == UINT(outputHeight)
                && m_processorFps == fps) return S_OK;

        m_videoProcessor.Reset();
        m_processorEnumerator.Reset();
        m_lastNative = nullptr;
        m_texturePool.reset();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {UINT(std::max(1, fps)), 1};
        content.InputWidth = sourceDesc.Width;
        content.InputHeight = sourceDesc.Height;
        content.OutputFrameRate = {UINT(std::max(1, fps)), 1};
        content.OutputWidth = UINT(outputWidth);
        content.OutputHeight = UINT(outputHeight);
        content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(
            &content, &m_processorEnumerator);
        UINT sourceSupport = 0;
        UINT outputSupport = 0;
        if (SUCCEEDED(hr))
            hr = m_processorEnumerator->CheckVideoProcessorFormat(sourceDesc.Format, &sourceSupport);
        if (SUCCEEDED(hr) && !(sourceSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
            hr = DXGI_ERROR_UNSUPPORTED;
        if (SUCCEEDED(hr))
            hr = m_processorEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outputSupport);
        if (SUCCEEDED(hr) && !(outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
            hr = DXGI_ERROR_UNSUPPORTED;
        if (SUCCEEDED(hr))
            hr = m_videoDevice->CreateVideoProcessor(
                m_processorEnumerator.Get(), 0, &m_videoProcessor);
        if (FAILED(hr)) {
            m_videoProcessor.Reset();
            m_processorEnumerator.Reset();
            return hr;
        }
        m_processorSourceWidth = sourceDesc.Width;
        m_processorSourceHeight = sourceDesc.Height;
        m_processorSourceFormat = sourceDesc.Format;
        m_processorOutputWidth = UINT(outputWidth);
        m_processorOutputHeight = UINT(outputHeight);
        m_processorFps = fps;
        return S_OK;
    }

    HRESULT acquireOutputTexture(int width, int height,
                                 Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                                 std::shared_ptr<TexturePool>& selectedPool,
                                 size_t& selectedSlot) {
        if (!m_texturePool || m_texturePool->width != width || m_texturePool->height != height) {
            m_texturePool = std::make_shared<TexturePool>();
            m_texturePool->width = width;
            m_texturePool->height = height;
        }
        selectedPool = m_texturePool;
        std::lock_guard<std::mutex> lock(selectedPool->mutex);
        for (size_t i = 0; i < selectedPool->slots.size(); ++i) {
            if (!selectedPool->slots[i].busy) {
                selectedPool->slots[i].busy = true;
                selectedSlot = i;
                texture = selectedPool->slots[i].texture;
                return S_OK;
            }
        }
        constexpr size_t kMaxGpuFramesInFlight = 8;
        if (selectedPool->slots.size() >= kMaxGpuFramesInFlight)
            return HRESULT_FROM_WIN32(ERROR_BUSY);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(width);
        desc.Height = UINT(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        TexturePool::Slot slot;
        const HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &slot.texture);
        if (FAILED(hr)) return hr;
        slot.busy = true;
        selectedPool->slots.push_back(slot);
        selectedSlot = selectedPool->slots.size() - 1;
        texture = slot.texture;
        return S_OK;
    }

    QImage textureToImage(ID3D11Texture2D* texture) {
        if (!texture) return {};
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
                && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            return {};
        ensureStaging(desc);
        if (!m_staging) return {};
        m_context->CopyResource(m_staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return {};
        QImage out(int(desc.Width), int(desc.Height), QImage::Format_ARGB32);
        const int rowBytes = int(desc.Width) * 4;
        for (int y = 0; y < out.height(); ++y) {
            memcpy(out.scanLine(y),
                   static_cast<const char*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                   size_t(rowBytes));
        }
        m_context->Unmap(m_staging.Get(), 0);
        return out;
    }

    void ensureStaging(const D3D11_TEXTURE2D_DESC& sourceDesc) {
        if (m_staging && m_stagingWidth == sourceDesc.Width
                && m_stagingHeight == sourceDesc.Height
                && m_stagingFormat == sourceDesc.Format) return;
        m_staging.Reset();
        D3D11_TEXTURE2D_DESC desc = sourceDesc;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        if (SUCCEEDED(m_device->CreateTexture2D(&desc, nullptr, &m_staging))) {
            m_stagingWidth = sourceDesc.Width;
            m_stagingHeight = sourceDesc.Height;
            m_stagingFormat = sourceDesc.Format;
        }
    }

    void reset() {
        m_lastNative = nullptr;
        m_texturePool.reset();
        m_videoProcessor.Reset();
        m_processorEnumerator.Reset();
        m_videoContext.Reset();
        m_videoDevice.Reset();
        m_duplication.Reset();
        m_staging.Reset();
        m_context.Reset();
        m_device.Reset();
        m_lastFrame = {};
        m_stagingWidth = 0;
        m_stagingHeight = 0;
        m_stagingFormat = DXGI_FORMAT_UNKNOWN;
        m_processorSourceWidth = 0;
        m_processorSourceHeight = 0;
        m_processorSourceFormat = DXGI_FORMAT_UNKNOWN;
        m_processorOutputWidth = 0;
        m_processorOutputHeight = 0;
        m_processorFps = 0;
        m_frameNumber = 0;
        m_screenIndex = -1;
        m_gpuPathLogged = false;
        m_gpuFailureLogged = false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_processorEnumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_videoProcessor;
    std::shared_ptr<TexturePool> m_texturePool;
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> m_lastNative;
    QImage m_lastFrame;
    UINT m_stagingWidth = 0;
    UINT m_stagingHeight = 0;
    DXGI_FORMAT m_stagingFormat = DXGI_FORMAT_UNKNOWN;
    UINT m_processorSourceWidth = 0;
    UINT m_processorSourceHeight = 0;
    DXGI_FORMAT m_processorSourceFormat = DXGI_FORMAT_UNKNOWN;
    UINT m_processorOutputWidth = 0;
    UINT m_processorOutputHeight = 0;
    int m_processorFps = 0;
    UINT m_frameNumber = 0;
    int m_screenIndex = -1;
    bool m_gpuPathLogged = false;
    bool m_gpuFailureLogged = false;
};

#ifdef HALLA_PROCESS_LOOPBACK_EXCLUSION
class ProcessLoopbackActivationHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::FtmBase,
          IActivateAudioInterfaceCompletionHandler> {
public:
    explicit ProcessLoopbackActivationHandler(HANDLE completed) : m_completed(completed) {}

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activationResult = E_UNEXPECTED;
        Microsoft::WRL::ComPtr<IUnknown> activated;
        HRESULT hr = operation->GetActivateResult(&activationResult, activated.GetAddressOf());
        m_result = FAILED(hr) ? hr : activationResult;
        if (SUCCEEDED(m_result)) m_result = activated.As(&m_audioClient);
        SetEvent(m_completed);
        return S_OK;
    }

    HRESULT result() const { return m_result; }
    Microsoft::WRL::ComPtr<IAudioClient> audioClient() const { return m_audioClient; }

private:
    HANDLE m_completed = nullptr;
    HRESULT m_result = E_PENDING;
    Microsoft::WRL::ComPtr<IAudioClient> m_audioClient;
};

static HRESULT activateSystemLoopbackExcludingHalla(
        Microsoft::WRL::ComPtr<IAudioClient>& audioClient) {
    HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completed) return HRESULT_FROM_WIN32(GetLastError());

    auto handler = Microsoft::WRL::Make<ProcessLoopbackActivationHandler>(completed);
    if (!handler) {
        CloseHandle(completed);
        return E_OUTOFMEMORY;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activation = {};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activation,
        handler.Get(),
        operation.GetAddressOf());
    if (SUCCEEDED(hr)) {
        const DWORD waitResult = WaitForSingleObject(completed, INFINITE);
        hr = waitResult == WAIT_OBJECT_0 ? handler->result()
                                         : HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr)) audioClient = handler->audioClient();
    }
    CloseHandle(completed);
    return hr;
}
#endif

class SystemLoopbackAudioDeviceModule
    : public webrtc::webrtc_impl::AudioDeviceModuleDefault<webrtc::AudioDeviceModuleForTest> {
public:
    ~SystemLoopbackAudioDeviceModule() override {
        StopPlayout();
        StopRecording();
    }

    int32_t RegisterAudioCallback(webrtc::AudioTransport* audioCallback) override {
        m_audioCallback.store(audioCallback, std::memory_order_release);
        return 0;
    }
    int32_t ActiveAudioLayer(webrtc::AudioDeviceModule::AudioLayer* audioLayer) const override {
        if (audioLayer) *audioLayer = webrtc::AudioDeviceModule::kWindowsCoreAudio;
        return 0;
    }
    int32_t RecordingIsAvailable(bool* available) override { if (available) *available = true; return 0; }
    bool RecordingIsInitialized() const override { return true; }
    int32_t InitRecording() override { return 0; }
    int32_t PlayoutIsAvailable(bool* available) override { if (available) *available = true; return 0; }
    bool PlayoutIsInitialized() const override { return true; }
    int32_t InitPlayout() override { return 0; }
    int32_t StartPlayout() override {
        if (m_playoutRunning.exchange(true)) return 0;
        if (m_playoutThread.joinable()) m_playoutThread.join();
        m_playoutThread = std::thread([this] { playoutLoop(); });
        return 0;
    }
    int32_t StopPlayout() override {
        m_playoutRunning.store(false);
        if (m_playoutThread.joinable()) m_playoutThread.join();
        return 0;
    }
    bool Playing() const override { return m_playoutRunning.load(); }
    int32_t StartRecording() override {
        if (m_running.exchange(true)) return 0;
        // Uma tentativa anterior pode ter falhado e encerrado a thread depois
        // de limpar m_running; faça join antes de reutilizar std::thread.
        if (m_thread.joinable()) m_thread.join();
        m_thread = std::thread([this] { captureLoop(); });
        return 0;
    }
    int32_t StopRecording() override {
        m_running.store(false);
        if (m_thread.joinable()) m_thread.join();
        return 0;
    }
    bool Recording() const override { return m_running.load(); }
    int RestartPlayoutInternally() override { StopPlayout(); return StartPlayout(); }
    int RestartRecordingInternally() override { StopRecording(); return StartRecording(); }
    int SetPlayoutSampleRate(uint32_t) override { return 0; }
    int SetRecordingSampleRate(uint32_t sample_rate) override { m_forcedSampleRate = sample_rate; return 0; }

private:
    void pushSamples(const int16_t* samples, size_t count, uint32_t sampleRate) {
        if (!samples || count == 0) return;
        m_pcm.insert(m_pcm.end(), samples, samples + count);
        const size_t frameSamples = std::max<size_t>(1, sampleRate / 100); // 10 ms mono
        webrtc::AudioTransport* cb = m_audioCallback.load(std::memory_order_acquire);
        while (cb && m_pcm.size() >= frameSamples) {
            uint32_t newMicLevel = 0;
            cb->RecordedDataIsAvailable(m_pcm.data(), frameSamples, 2, 1, sampleRate,
                                        0, 0, 0, false, newMicLevel);
            m_pcm.erase(m_pcm.begin(), m_pcm.begin() + frameSamples);
            cb = m_audioCallback.load(std::memory_order_acquire);
        }
        if (m_pcm.size() > sampleRate) m_pcm.clear();
    }

    void playoutLoop() {
        constexpr uint32_t kSampleRate = 48000;
        constexpr size_t kChannels = 1;
        constexpr size_t kFrames = kSampleRate / 100; // 10 ms
        std::vector<int16_t> discarded(kFrames * kChannels);
        auto deadline = std::chrono::steady_clock::now();
        while (m_playoutRunning.load()) {
            if (webrtc::AudioTransport* cb =
                    m_audioCallback.load(std::memory_order_acquire)) {
                size_t framesOut = 0;
                int64_t elapsedTimeMs = 0;
                int64_t ntpTimeMs = 0;
                cb->NeedMorePlayData(kFrames, sizeof(int16_t), kChannels,
                                     kSampleRate, discarded.data(), framesOut,
                                     &elapsedTimeMs, &ntpTimeMs);
            }
            deadline += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(deadline);
            const auto now = std::chrono::steady_clock::now();
            if (deadline < now - std::chrono::milliseconds(100)) deadline = now;
        }
    }

    void captureLoop() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninitialize = SUCCEEDED(comResult);
        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
            AppLog::warn(QStringLiteral("WASAPI process loopback: falha ao inicializar COM"));
            m_running = false;
            return;
        }

#ifndef HALLA_PROCESS_LOOPBACK_EXCLUSION
        // Nunca volte silenciosamente ao loopback global: ele capturaria as
        // vozes e notificações do próprio Halla e causaria retorno/eco.
        AppLog::warn(QStringLiteral(
            "Áudio da transmissão indisponível: SDK do Windows sem process loopback exclusion"));
        if (shouldUninitialize) CoUninitialize();
        m_running = false;
        return;
#else
        Microsoft::WRL::ComPtr<IAudioClient> client;
        HRESULT hr = activateSystemLoopbackExcludingHalla(client);
        if (FAILED(hr) || !client) {
            AppLog::warn(QStringLiteral(
                "Áudio da transmissão indisponível: Windows sem suporte à exclusão do Halla (0x%1)")
                .arg(quint32(hr), 8, 16, QLatin1Char('0')));
            if (shouldUninitialize) CoUninitialize();
            m_running = false;
            return;
        }

        const uint32_t sampleRate = m_forcedSampleRate ? m_forcedSampleRate : 48000;
        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = sampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        const DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK
                                | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 0, 0, &format, nullptr);
        Microsoft::WRL::ComPtr<IAudioCaptureClient> capture;
        if (FAILED(hr)
                || FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                             reinterpret_cast<void**>(capture.GetAddressOf())))
                || FAILED(client->Start())) {
            AppLog::warn(QStringLiteral("WASAPI process loopback: falha ao iniciar captura (0x%1)")
                             .arg(quint32(hr), 8, 16, QLatin1Char('0')));
            if (shouldUninitialize) CoUninitialize();
            m_running = false;
            return;
        }

        AppLog::info(QStringLiteral(
            "WASAPI process loopback ativo: todos os sons do PC, Halla.exe e filhos excluídos; %1 Hz")
            .arg(sampleRate));

        while (m_running.load()) {
            UINT32 packetFrames = 0;
            if (FAILED(capture->GetNextPacketSize(&packetFrames))) break;
            if (packetFrames == 0) { Sleep(5); continue; }
            while (packetFrames > 0 && m_running.load()) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                std::vector<int16_t> mono(frames);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(mono.begin(), mono.end(), 0);
                } else {
                    const int16_t* input = reinterpret_cast<const int16_t*>(data);
                    for (UINT32 i = 0; i < frames; ++i) {
                        const int mixed = int(input[i * 2]) + int(input[i * 2 + 1]);
                        mono[i] = int16_t(mixed / 2);
                    }
                }
                if (!mono.empty()) pushSamples(mono.data(), mono.size(), sampleRate);
                capture->ReleaseBuffer(frames);
                if (FAILED(capture->GetNextPacketSize(&packetFrames))) break;
            }
        }
        client->Stop();
        if (shouldUninitialize) CoUninitialize();
        m_running = false;
#endif
    }

    std::atomic<webrtc::AudioTransport*> m_audioCallback{nullptr};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_playoutRunning{false};
    std::thread m_thread;
    std::thread m_playoutThread;
    std::vector<int16_t> m_pcm;
    uint32_t m_forcedSampleRate = 48000;
};

static QPixmap grabWindowsAppForWebRtc(quintptr sourceId) {
    HWND hwnd = reinterpret_cast<HWND>(sourceId);
    if (!hwnd) return QPixmap();

    RECT rect;
    GetWindowRect(hwnd, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return QPixmap();

    HDC hdcWindow = GetWindowDC(hwnd);
    if (!hdcWindow) return QPixmap();
    HDC hdcMem = CreateCompatibleDC(hdcWindow);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdcWindow, width, height);
    HGDIOBJ hOld = SelectObject(hdcMem, hbmMem);

    BOOL ok = PrintWindow(hwnd, hdcMem, 2);
    if (!ok) ok = PrintWindow(hwnd, hdcMem, 0);

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWindow);

    QPixmap pix;
    if (ok) pix = QPixmap::fromImage(QImage::fromHBITMAP(hbmMem));
    DeleteObject(hbmMem);
    return pix;
}
#endif

class NoopSetObserver : public webrtc::SetSessionDescriptionObserver {
public:
    void OnSuccess() override {}
    void OnFailure(webrtc::RTCError error) override {
        AppLog::warn(QStringLiteral("WebRTC SetDescription falhou: %1")
                         .arg(QString::fromStdString(error.message())));
    }
};

class PeerObserver : public webrtc::PeerConnectionObserver {
public:
    PeerObserver(HallaWebRtcSession* owner, int peerId) : m_owner(owner), m_peerId(peerId) {}
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState state) override {
        AppLog::info(QStringLiteral("WebRTC signaling peer #%1: %2").arg(m_peerId).arg(int(state)));
    }
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) override {
        AppLog::info(QStringLiteral("WebRTC ICE peer #%1: %2").arg(m_peerId).arg(int(state)));
    }
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override {
        AppLog::info(QStringLiteral("WebRTC conexão peer #%1: %2").arg(m_peerId).arg(int(state)));
    }
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override {
        AppLog::info(QStringLiteral("WebRTC ICE gathering peer #%1: %2").arg(m_peerId).arg(int(state)));
    }
    void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override;
    void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) override;
private:
    HallaWebRtcSession* m_owner = nullptr;
    int m_peerId = 0;
};

class OfferObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    OfferObserver(HallaWebRtcSession* owner, int peerId) : m_owner(owner), m_peerId(peerId) {}
    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
    void OnFailure(webrtc::RTCError error) override {
        AppLog::warn(QStringLiteral("WebRTC CreateOffer falhou: %1")
                         .arg(QString::fromStdString(error.message())));
    }
private:
    HallaWebRtcSession* m_owner = nullptr;
    int m_peerId = 0;
};

class HallaVideoEncoderFactory : public webrtc::VideoEncoderFactory {
public:
    explicit HallaVideoEncoderFactory(bool requestHardware)
        : m_hardware(requestHardware && HallaMfH264::encoderAvailable()) {
        if (requestHardware && !m_hardware)
            AppLog::warn(QStringLiteral(
                "WebRTC: encoder de hardware solicitado, mas nenhum H264 MFT foi encontrado; usando VP8 por software"));
        else if (m_hardware)
            AppLog::info(QStringLiteral("WebRTC: H264 por hardware disponível e preferido"));
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats;
        if (m_hardware) {
            const auto hardware = HallaMfH264::formats();
            formats.insert(formats.end(), hardware.begin(), hardware.end());
        }
        formats.emplace_back("VP8");
        return formats;
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                   std::optional<std::string> scalabilityMode,
                                   std::optional<webrtc::Resolution> resolution) const override {
        Q_UNUSED(scalabilityMode);
        Q_UNUSED(resolution);
        if ((format.name == "H264" || format.name == "h264") && m_hardware)
            return {true, true};
        return {format.name == "VP8" || format.name == "vp8", false};
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override {
        if ((format.name == "H264" || format.name == "h264") && m_hardware)
            return HallaMfH264::createEncoder();
        if (format.name != "VP8" && format.name != "vp8") return nullptr;
        return webrtc::CreateVp8Encoder(env);
    }

private:
    bool m_hardware = false;
};

class HallaVideoDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats = HallaMfH264::formats();
        formats.emplace_back("VP8");
        return formats;
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                   bool referenceScaling,
                                   std::optional<webrtc::Resolution> resolution) const override {
        Q_UNUSED(referenceScaling);
        Q_UNUSED(resolution);
        if (format.name == "H264" || format.name == "h264") return {true, false};
        return {format.name == "VP8" || format.name == "vp8", false};
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override {
        if (format.name == "H264" || format.name == "h264")
            return HallaMfH264::createDecoder();
        if (format.name != "VP8" && format.name != "vp8") return nullptr;
        return webrtc::CreateVp8Decoder(env);
    }
};

class AnswerObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    AnswerObserver(HallaWebRtcSession* owner, int peerId) : m_owner(owner), m_peerId(peerId) {}
    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
    void OnFailure(webrtc::RTCError error) override {
        AppLog::warn(QStringLiteral("WebRTC CreateAnswer falhou: %1")
                         .arg(QString::fromStdString(error.message())));
    }
private:
    HallaWebRtcSession* m_owner = nullptr;
    int m_peerId = 0;
};

class RemoteOfferSetObserver : public webrtc::SetSessionDescriptionObserver {
public:
    RemoteOfferSetObserver(HallaWebRtcSession* owner, int peerId) : m_owner(owner), m_peerId(peerId) {}
    void OnSuccess() override;
    void OnFailure(webrtc::RTCError error) override {
        AppLog::warn(QStringLiteral("WebRTC SetRemoteOffer falhou: %1")
                         .arg(QString::fromStdString(error.message())));
    }
private:
    HallaWebRtcSession* m_owner = nullptr;
    int m_peerId = 0;
};

class RemoteAnswerSetObserver : public webrtc::SetSessionDescriptionObserver {
public:
    RemoteAnswerSetObserver(HallaWebRtcSession* owner, int peerId) : m_owner(owner), m_peerId(peerId) {}
    void OnSuccess() override;
    void OnFailure(webrtc::RTCError error) override {
        AppLog::warn(QStringLiteral("WebRTC SetRemoteAnswer falhou: %1")
                         .arg(QString::fromStdString(error.message())));
    }
private:
    HallaWebRtcSession* m_owner = nullptr;
    int m_peerId = 0;
};

class RemoteVideoSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    RemoteVideoSink(HallaWebRtcSession* owner, int peerId) : m_owner(owner), m_peerId(peerId) {}
    void OnFrame(const webrtc::VideoFrame& frame) override {
        if (!m_owner) return;
        auto i420 = frame.video_frame_buffer() ? frame.video_frame_buffer()->ToI420() : nullptr;
        if (!i420) return;
        QImage image(i420->width(), i420->height(), QImage::Format_ARGB32);
        const int ret = libyuv::I420ToARGB(
            i420->DataY(), i420->StrideY(),
            i420->DataU(), i420->StrideU(),
            i420->DataV(), i420->StrideV(),
            image.bits(), image.bytesPerLine(),
            i420->width(), i420->height());
        if (ret == 0) m_owner->deliverRemoteFrame(m_peerId, image.copy());
    }
private:
    HallaWebRtcSession* m_owner = nullptr;
    int m_peerId = 0;
};

class RemoteAudioSink : public webrtc::AudioTrackSinkInterface {
public:
    RemoteAudioSink(HallaWebRtcSession* owner, int peerId)
        : m_owner(owner), m_peerId(peerId) {}

    void OnData(const void* audioData, int bitsPerSample, int sampleRate,
                size_t channels, size_t frames) override {
        if (!m_owner || !audioData || bitsPerSample != 16 || sampleRate != 48000
                || (channels != 1 && channels != 2) || frames == 0) return;
        if (!m_loggedFirstFrame) {
            m_loggedFirstFrame = true;
            AppLog::info(QStringLiteral(
                "WebRTC Desktop viewer: primeiro PCM recebido do peer #%1").arg(m_peerId));
        }
        const int channelCount = int(channels);
        if (m_channels != channelCount || m_sampleRate != sampleRate) {
            m_pending.clear();
            m_channels = channelCount;
            m_sampleRate = sampleRate;
        }
        const int16_t* samples = static_cast<const int16_t*>(audioData);
        m_pending.insert(m_pending.end(), samples, samples + frames * channels);
        const size_t packetSamples = size_t(960 * channelCount); // mixer Qt usa 20 ms
        while (m_pending.size() >= packetSamples) {
            QByteArray pcm(int(packetSamples * sizeof(int16_t)), Qt::Uninitialized);
            std::memcpy(pcm.data(), m_pending.data(), size_t(pcm.size()));
            m_pending.erase(m_pending.begin(), m_pending.begin() + ptrdiff_t(packetSamples));
            m_owner->deliverRemoteAudio(m_peerId, pcm, sampleRate, channelCount, 960);
        }
    }

    int NumPreferredChannels() const override { return 1; }

private:
    HallaWebRtcSession* m_owner = nullptr;
    int m_peerId = 0;
    int m_channels = 0;
    int m_sampleRate = 0;
    bool m_loggedFirstFrame = false;
    std::vector<int16_t> m_pending;
};

class QtScreenVideoSource : public webrtc::VideoTrackSourceInterface {
public:
    bool is_screencast() const override { return true; }
    std::optional<bool> needs_denoising() const override { return false; }
    SourceState state() const override { return kLive; }
    bool remote() const override { return false; }

    void RegisterObserver(webrtc::ObserverInterface* observer) override {
        if (!observer) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (std::find(m_observers.begin(), m_observers.end(), observer) == m_observers.end())
            m_observers.push_back(observer);
    }

    void UnregisterObserver(webrtc::ObserverInterface* observer) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_observers.erase(std::remove(m_observers.begin(), m_observers.end(), observer), m_observers.end());
    }

    void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                         const webrtc::VideoSinkWants& wants) override {
        if (!sink) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& item : m_sinks) {
            if (item.sink == sink) {
                item.wants = wants;
                return;
            }
        }
        m_sinks.push_back({sink, wants});
    }

    void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sinks.erase(std::remove_if(m_sinks.begin(), m_sinks.end(),
                                     [sink](const SinkEntry& item) { return item.sink == sink; }),
                      m_sinks.end());
    }

    bool GetStats(Stats* stats) override {
        if (!stats) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_lastWidth <= 0 || m_lastHeight <= 0) return false;
        stats->input_width = m_lastWidth;
        stats->input_height = m_lastHeight;
        return true;
    }

    bool SupportsEncodedOutput() const override { return false; }
    void GenerateKeyFrame() override {}
    void AddEncodedSink(webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>*) override {}
    void RemoveEncodedSink(webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>*) override {}

    void PushImage(const QImage& image) {
        if (image.isNull()) return;
        QImage img = image.convertToFormat(QImage::Format_RGB32);
        const int w = img.width() & ~1;
        const int h = img.height() & ~1;
        if (w <= 1 || h <= 1) return;
        auto buffer = webrtc::I420Buffer::Create(w, h);
        // QImage::Format_RGB32 is compatible with libyuv ARGB on little-endian
        // Windows. libyuv is much faster than the previous per-pixel C++ loop
        // and avoids starving audio while screen sharing.
        const int converted = libyuv::ARGBToI420(
            img.constBits(), img.bytesPerLine(),
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            w, h);
        if (converted != 0) return;
        PushBuffer(buffer);
    }

    void PushBuffer(const webrtc::scoped_refptr<webrtc::VideoFrameBuffer>& buffer) {
        if (!buffer || buffer->width() < 2 || buffer->height() < 2) return;
        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(buffer)
            .set_timestamp_ms(QDateTime::currentMSecsSinceEpoch())
            .set_rotation(webrtc::kVideoRotation_0)
            .build();

        std::vector<webrtc::VideoSinkInterface<webrtc::VideoFrame>*> sinks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastWidth = buffer->width();
            m_lastHeight = buffer->height();
            sinks.reserve(m_sinks.size());
            for (const auto& item : m_sinks) sinks.push_back(item.sink);
        }
        for (auto* sink : sinks) if (sink) sink->OnFrame(frame);
    }

protected:
    ~QtScreenVideoSource() override = default;

private:
    struct SinkEntry {
        webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink = nullptr;
        webrtc::VideoSinkWants wants;
    };

    mutable std::mutex m_mutex;
    std::vector<SinkEntry> m_sinks;
    std::vector<webrtc::ObserverInterface*> m_observers;
    int m_lastWidth = 0;
    int m_lastHeight = 0;
};
}
#endif

#ifdef HALLA_WEBRTC_NATIVE
struct HallaWebRtcSession::PeerContext {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    std::unique_ptr<PeerObserver> observer;
    std::vector<webrtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver>> pendingOffers;
    struct RemoteBinding {
        webrtc::scoped_refptr<webrtc::VideoTrackInterface> track;
        std::unique_ptr<RemoteVideoSink> sink;
    };
    std::vector<RemoteBinding> remoteVideo;
    struct RemoteAudioBinding {
        webrtc::scoped_refptr<webrtc::AudioTrackInterface> track;
        std::unique_ptr<RemoteAudioSink> sink;
    };
    std::vector<RemoteAudioBinding> remoteAudio;
    std::vector<QJsonObject> pendingRemoteIce;
    bool remoteDescriptionSet = false;
    bool trackAdded = false;
    bool audioTrackAdded = false;
};
#endif

struct HallaWebRtcSession::NativeState {
#ifdef HALLA_WEBRTC_NATIVE
    std::unique_ptr<webrtc::Thread> networkThread;
    std::unique_ptr<webrtc::Thread> workerThread;
    std::unique_ptr<webrtc::Thread> signalingThread;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> loopbackAdm;
    webrtc::scoped_refptr<webrtc::AudioSourceInterface> systemAudioSource;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> systemAudioTrack;
    webrtc::scoped_refptr<QtScreenVideoSource> videoSource;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> videoTrack;
#ifdef Q_OS_WIN
    std::unique_ptr<DxgiScreenCapturer> dxgiCapturer;
#endif
    std::map<int, std::unique_ptr<PeerContext>> peers;
    uint64_t captureFrameNumber = 0;
    bool gpuCaptureFrames = false;
    bool factoryHardwareConfigured = false;
    bool factoryConfigurationKnown = false;
    bool sslInitialized = false;

    ~NativeState() = default;
#endif
};

HallaWebRtcSession::HallaWebRtcSession(NetSession* net, QObject* parent)
    : QObject(parent), m_native(std::make_unique<NativeState>()), m_net(net) {}

HallaWebRtcSession::~HallaWebRtcSession() {
    stopBroadcast();
#ifdef HALLA_WEBRTC_NATIVE
    if (m_native) {
        while (!m_native->peers.empty()) closePeer(m_native->peers.begin()->first);
        if (m_native->loopbackAdm) {
            m_native->loopbackAdm->StopPlayout();
            m_native->loopbackAdm->StopRecording();
        }
        m_native->systemAudioTrack = nullptr;
        m_native->systemAudioSource = nullptr;
        m_native->videoTrack = nullptr;
        m_native->videoSource = nullptr;
        m_native->factory = nullptr;
        m_native->signalingThread.reset();
        m_native->workerThread.reset();
        m_native->networkThread.reset();
        if (m_native->sslInitialized) {
            webrtc::CleanupSSL();
            m_native->sslInitialized = false;
        }
    }
#endif
}

bool HallaWebRtcSession::isNativeAvailable() const {
#ifdef HALLA_WEBRTC_NATIVE
    return true;
#else
    return false;
#endif
}

void HallaWebRtcSession::setCaptureSource(int sourceType, quintptr sourceId) {
    m_captureSourceType = sourceType;
    m_captureSourceId = sourceId;
}

void HallaWebRtcSession::setCaptureQuality(int width, int height, int fps, int bitrateKbps) {
    m_captureWidth = qBound(640, width, 3840);
    m_captureHeight = qBound(360, height, 2160);
    m_captureFps = qBound(15, fps, 60);
    m_captureBitrateKbps = qBound(500, bitrateKbps, 50000);
    if (m_captureTimer && m_captureTimer->isActive()) {
        m_captureTimer->start(qMax(1, 1000 / m_captureFps));
    }
}

void HallaWebRtcSession::setCaptureSystemAudio(bool enabled) {
    m_captureSystemAudio = enabled;
    if (enabled) {
        AppLog::info(QStringLiteral("WebRTC: captura de áudio do PC solicitada com exclusão do processo Halla"));
    }
}

#ifdef HALLA_WEBRTC_NATIVE
void HallaWebRtcSession::resetNativeFactoryForEncoderSetting() {
    if (!m_native || !m_native->factory || !m_native->factoryConfigurationKnown)
        return;
    const bool requested = S::flag("screenshare/hardwareEncoder", false);
    if (requested == m_native->factoryHardwareConfigured) return;

    AppLog::info(QStringLiteral(
        "WebRTC: configuração do encoder mudou; recriando factory para a próxima transmissão"));
    while (!m_native->peers.empty()) closePeer(m_native->peers.begin()->first);
    if (m_native->loopbackAdm) {
        m_native->loopbackAdm->StopPlayout();
        m_native->loopbackAdm->StopRecording();
        m_native->loopbackAdm->RegisterAudioCallback(nullptr);
    }
    m_native->systemAudioTrack = nullptr;
    m_native->systemAudioSource = nullptr;
    m_native->videoTrack = nullptr;
    m_native->videoSource = nullptr;
    m_native->factory = nullptr;
    m_native->signalingThread.reset();
    m_native->workerThread.reset();
    m_native->networkThread.reset();
    m_native->gpuCaptureFrames = false;
    m_native->factoryConfigurationKnown = false;
}

bool HallaWebRtcSession::ensureNativeFactory() {
    if (!m_native->sslInitialized) {
        if (!webrtc::InitializeSSL()) return false;
        m_native->sslInitialized = true;
    }
    if (m_native->factory) return true;
    m_native->networkThread = webrtc::Thread::CreateWithSocketServer();
    m_native->workerThread = webrtc::Thread::Create();
    m_native->signalingThread = webrtc::Thread::Create();
    if (!m_native->networkThread || !m_native->workerThread || !m_native->signalingThread)
        return false;
    if (!m_native->networkThread->Start() || !m_native->workerThread->Start() || !m_native->signalingThread->Start())
        return false;
#ifdef Q_OS_WIN
    if (!m_native->loopbackAdm) {
        m_native->loopbackAdm = webrtc::make_ref_counted<SystemLoopbackAudioDeviceModule>();
    }
#endif
    const bool hardwareEncoderRequested = S::flag("screenshare/hardwareEncoder", false);
    m_native->gpuCaptureFrames = hardwareEncoderRequested && HallaMfH264::encoderAvailable();
    m_native->factory = webrtc::CreatePeerConnectionFactory(
        m_native->networkThread.get(), m_native->workerThread.get(), m_native->signalingThread.get(),
#ifdef Q_OS_WIN
        m_native->loopbackAdm,
#else
        nullptr,
#endif
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        std::make_unique<HallaVideoEncoderFactory>(hardwareEncoderRequested),
        std::make_unique<HallaVideoDecoderFactory>(),
        nullptr, nullptr);
    if (m_native->factory) {
        m_native->factoryHardwareConfigured = hardwareEncoderRequested;
        m_native->factoryConfigurationKnown = true;
        m_native->videoSource = webrtc::make_ref_counted<QtScreenVideoSource>();
        m_native->videoTrack = m_native->factory->CreateVideoTrack(m_native->videoSource, "halla-screen");
        if (m_native->videoTrack) {
            m_native->videoTrack->set_enabled(true);
            m_native->videoTrack->set_content_hint(webrtc::VideoTrackInterface::ContentHint::kDetailed);
        }
        m_native->systemAudioSource = m_native->factory->CreateAudioSource(webrtc::AudioOptions());
        m_native->systemAudioTrack = m_native->factory->CreateAudioTrack("halla-system-audio", m_native->systemAudioSource.get());
        if (m_native->systemAudioTrack) m_native->systemAudioTrack->set_enabled(true);
        AppLog::info(tr("WebRTC nativo inicializado (factory, video e áudio de sistema prontos)"));
    }
    return m_native->factory != nullptr;
}

HallaWebRtcSession::PeerContext* HallaWebRtcSession::ensurePeer(int peerId) {
    if (peerId <= 0 || !ensureNativeFactory()) return nullptr;
    auto it = m_native->peers.find(peerId);
    if (it != m_native->peers.end()) return it->second.get();

    auto ctx = std::make_unique<PeerContext>();
    ctx->observer = std::make_unique<PeerObserver>(this, peerId);
    webrtc::PeerConnectionInterface::RTCConfiguration cfg;
    cfg.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    const QJsonArray configuredServers = m_net ? m_net->webRtcIceServers() : QJsonArray();
    for (const QJsonValue& value : configuredServers) {
        const QJsonObject object = value.toObject();
        webrtc::PeerConnectionInterface::IceServer server;
        const QJsonValue urls = object.value(QStringLiteral("urls"));
        if (urls.isArray()) {
            for (const QJsonValue& url : urls.toArray())
                if (!url.toString().isEmpty()) server.urls.push_back(url.toString().toStdString());
        } else if (!urls.toString().isEmpty()) {
            server.urls.push_back(urls.toString().toStdString());
        }
        server.username = object.value(QStringLiteral("username")).toString().toStdString();
        server.password = object.value(QStringLiteral("credential")).toString().toStdString();
        if (!server.urls.empty()) cfg.servers.push_back(std::move(server));
    }
    if (cfg.servers.empty()) {
        webrtc::PeerConnectionInterface::IceServer fallback;
        fallback.urls.push_back("stun:stun.l.google.com:19302");
        cfg.servers.push_back(std::move(fallback));
    }
    webrtc::PeerConnectionDependencies deps(ctx->observer.get());
    auto result = m_native->factory->CreatePeerConnectionOrError(cfg, std::move(deps));
    if (!result.ok()) {
        AppLog::warn(QStringLiteral("WebRTC PeerConnection falhou: %1")
                         .arg(QString::fromStdString(result.error().message())));
        return nullptr;
    }
    ctx->pc = result.MoveValue();
    PeerContext* raw = ctx.get();
    m_native->peers[peerId] = std::move(ctx);
    return raw;
}

void HallaWebRtcSession::sendNativeIce(int peerId, const std::string& candidate,
                                       const std::string& mid, int mline) {
    if (m_net) m_net->sendWebRtcIce(peerId, QString::fromStdString(candidate),
                                    QString::fromStdString(mid), mline);
}

void HallaWebRtcSession::sendNativeOffer(int peerId, const std::string& sdp) {
    if (m_net) m_net->sendWebRtcOffer(peerId, QString::fromStdString(sdp));
}

void HallaWebRtcSession::sendNativeAnswer(int peerId, const std::string& sdp) {
    if (m_net) m_net->sendWebRtcAnswer(peerId, QString::fromStdString(sdp));
}

void HallaWebRtcSession::startWatching(int userId) {
    if (userId <= 0) return;
    ensurePeer(userId);
    if (m_net) m_net->sendWebRtcWatchRequest(userId);
}

void HallaWebRtcSession::createOfferForPeer(int peerId) {
    PeerContext* ctx = ensurePeer(peerId);
    if (!ctx || !ctx->pc) return;
    if (!ctx->trackAdded && m_native->videoTrack) {
        auto result = ctx->pc->AddTrack(m_native->videoTrack, {"halla-screen"});
        if (!result.ok()) {
            AppLog::warn(QStringLiteral("WebRTC AddTrack falhou: %1")
                             .arg(QString::fromStdString(result.error().message())));
        } else {
            auto sender = result.MoveValue();
            auto params = sender->GetParameters();
            if (params.encodings.empty()) params.encodings.emplace_back();
            for (auto& encoding : params.encodings) {
                encoding.max_framerate = double(m_captureFps);
                // User-selected screen-share profile. The bitrate cap avoids
                // unbounded VP8/network spikes while keeping text readable.
                encoding.max_bitrate_bps = m_captureBitrateKbps * 1000;
            }
            const auto setParamsError = sender->SetParameters(params);
            if (!setParamsError.ok()) {
                AppLog::warn(QStringLiteral("WebRTC SetParameters falhou: %1")
                                 .arg(QString::fromStdString(setParamsError.message())));
            }
            ctx->trackAdded = true;
        }
    }
    if (m_captureSystemAudio && !ctx->audioTrackAdded && m_native->systemAudioTrack) {
        auto audioResult = ctx->pc->AddTrack(m_native->systemAudioTrack, {"halla-screen"});
        if (!audioResult.ok()) {
            AppLog::warn(QStringLiteral("WebRTC AddTrack áudio sistema falhou: %1")
                             .arg(QString::fromStdString(audioResult.error().message())));
        } else {
            ctx->audioTrackAdded = true;
            AppLog::info(QStringLiteral("WebRTC áudio de todo o PC adicionado ao peer #%1").arg(peerId));
        }
    }
    auto obs = webrtc::make_ref_counted<OfferObserver>(this, peerId);
    ctx->pendingOffers.push_back(obs);
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
    ctx->pc->CreateOffer(obs.get(), opts);
}

void HallaWebRtcSession::setRemoteAnswer(int peerId, const QString& sdp) {
    PeerContext* ctx = ensurePeer(peerId);
    if (!ctx || !ctx->pc || sdp.isEmpty()) return;
    auto desc = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp.toStdString());
    if (!desc) return;
    ctx->pc->SetRemoteDescription(
        new webrtc::RefCountedObject<RemoteAnswerSetObserver>(this, peerId), desc.release());
}

void HallaWebRtcSession::setRemoteOffer(int peerId, const QString& sdp) {
    PeerContext* ctx = ensurePeer(peerId);
    if (!ctx || !ctx->pc || sdp.isEmpty()) return;
    auto desc = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp.toStdString());
    if (!desc) return;
    ctx->pc->SetRemoteDescription(new webrtc::RefCountedObject<RemoteOfferSetObserver>(this, peerId), desc.release());
}

void HallaWebRtcSession::createAnswerForPeer(int peerId) {
    PeerContext* ctx = ensurePeer(peerId);
    if (!ctx || !ctx->pc) return;
    auto obs = webrtc::make_ref_counted<AnswerObserver>(this, peerId);
    ctx->pendingOffers.push_back(obs);
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
    ctx->pc->CreateAnswer(obs.get(), opts);
}

void HallaWebRtcSession::attachRemoteVideoTrack(int peerId, webrtc::scoped_refptr<webrtc::VideoTrackInterface> track) {
    if (!track || !m_native) return;
    auto it = m_native->peers.find(peerId);
    if (it == m_native->peers.end()) return;
    auto sink = std::make_unique<RemoteVideoSink>(this, peerId);
    track->AddOrUpdateSink(sink.get(), webrtc::VideoSinkWants());
    it->second->remoteVideo.push_back({track, std::move(sink)});
    AppLog::info(QStringLiteral("WebRTC Desktop viewer: vídeo remoto anexado do peer #%1").arg(peerId));
}

void HallaWebRtcSession::attachRemoteAudioTrack(
        int peerId, webrtc::scoped_refptr<webrtc::AudioTrackInterface> track) {
    if (!track || !m_native) return;
    auto it = m_native->peers.find(peerId);
    if (it == m_native->peers.end()) return;
    for (const auto& binding : it->second->remoteAudio)
        if (binding.track.get() == track.get()) return;
    auto sink = std::make_unique<RemoteAudioSink>(this, peerId);
    track->AddSink(sink.get());
    it->second->remoteAudio.push_back({track, std::move(sink)});
    // O ADM customizado captura o loopback do sistema, mas a saída final é o
    // mixer/QAudioSink do Halla. Ainda assim o libwebrtc precisa ser puxado a
    // cada 10 ms para decodificar a track e chamar RemoteAudioSink::OnData.
    if (m_native->loopbackAdm && !m_native->loopbackAdm->Playing()) {
        bool available = false;
        if (m_native->loopbackAdm->PlayoutIsAvailable(&available) == 0 && available
                && m_native->loopbackAdm->InitPlayout() == 0) {
            m_native->loopbackAdm->StartPlayout();
        }
    }
    AppLog::info(QStringLiteral("WebRTC Desktop viewer: áudio remoto anexado do peer #%1").arg(peerId));
}

void HallaWebRtcSession::deliverRemoteFrame(int peerId, const QImage& image) {
    if (peerId <= 0 || image.isNull()) return;
    bool postDispatch = false;
    {
        QMutexLocker lock(&m_remoteFrameMutex);
        m_pendingRemoteFrames[peerId] = image;
        if (!m_remoteFrameDispatchPosted.contains(peerId)) {
            m_remoteFrameDispatchPosted.insert(peerId);
            postDispatch = true;
        }
    }
    if (!postDispatch) return;
    QMetaObject::invokeMethod(this, [this, peerId] {
        QImage latest;
        {
            QMutexLocker lock(&m_remoteFrameMutex);
            latest = m_pendingRemoteFrames.take(peerId);
            m_remoteFrameDispatchPosted.remove(peerId);
        }
        if (!latest.isNull()) emit remoteFrameReceived(peerId, latest);
    }, Qt::QueuedConnection);
}

void HallaWebRtcSession::deliverRemoteAudio(int peerId, const QByteArray& pcm,
                                            int sampleRate, int channels, int frames) {
    emit remoteAudioReceived(peerId, pcm, sampleRate, channels, frames);
}

void HallaWebRtcSession::addRemoteIce(int peerId, const QJsonObject& signal) {
    PeerContext* ctx = ensurePeer(peerId);
    if (!ctx || !ctx->pc) return;
    const QString candidate = signal.value(QStringLiteral("candidate")).toString();
    if (candidate.isEmpty()) return;
    // Android costuma enviar ICE imediatamente após a oferta. libwebrtc rejeita
    // candidatos antes de SetRemoteDescription concluir; preserve-os e aplique
    // em ordem quando a descrição remota ficar pronta.
    if (!ctx->remoteDescriptionSet) {
        if (ctx->pendingRemoteIce.size() < 128) ctx->pendingRemoteIce.push_back(signal);
        return;
    }
    std::unique_ptr<webrtc::IceCandidate> ice(webrtc::CreateIceCandidate(
        signal.value(QStringLiteral("sdpMid")).toString(QStringLiteral("0")).toStdString(),
        signal.value(QStringLiteral("sdpMLineIndex")).toInt(0),
        candidate.toStdString(), nullptr));
    if (ice) ctx->pc->AddIceCandidate(std::move(ice), [](webrtc::RTCError error) {
        if (!error.ok()) AppLog::warn(QStringLiteral("WebRTC AddIceCandidate falhou: %1")
                                         .arg(QString::fromStdString(error.message())));
    });
}

void HallaWebRtcSession::remoteDescriptionReady(int peerId) {
    PeerContext* ctx = ensurePeer(peerId);
    if (!ctx) return;
    ctx->remoteDescriptionSet = true;
    const std::vector<QJsonObject> pending = std::move(ctx->pendingRemoteIce);
    ctx->pendingRemoteIce.clear();
    for (const QJsonObject& signal : pending) addRemoteIce(peerId, signal);
}

void HallaWebRtcSession::closePeer(int peerId) {
    auto it = m_native->peers.find(peerId);
    if (it != m_native->peers.end()) {
        for (auto& binding : it->second->remoteVideo) {
            if (binding.track && binding.sink) binding.track->RemoveSink(binding.sink.get());
        }
        for (auto& binding : it->second->remoteAudio) {
            if (binding.track && binding.sink) binding.track->RemoveSink(binding.sink.get());
        }
        if (it->second->pc) it->second->pc->Close();
        m_native->peers.erase(it);
    }
    {
        QMutexLocker lock(&m_remoteFrameMutex);
        m_pendingRemoteFrames.remove(peerId);
        // Um dispatch já postado pode executar e encontrar imagem vazia.
        m_remoteFrameDispatchPosted.remove(peerId);
    }
    if (m_native->peers.empty() && m_native->loopbackAdm
            && m_native->loopbackAdm->Playing()) {
        m_native->loopbackAdm->StopPlayout();
    }
}

void HallaWebRtcSession::captureFrame() {
    if (!m_broadcasting || !m_native || !m_native->videoSource) return;

    QImage frameImage;
    QPixmap pix;
    QScreen* primary = QGuiApplication::primaryScreen();
#ifdef Q_OS_WIN
    // Caminho de alto desempenho para monitor inteiro: o frame permanece em
    // D3D11 desde Desktop Duplication até o MFT/NVENC. A prévia local é copiada
    // para a CPU em apenas ~10 FPS e não limita a transmissão de 60 FPS.
    if (m_captureSourceType == 0 && m_native->gpuCaptureFrames) {
        const int screenIndex = int(m_captureSourceId);
        if (!m_native->dxgiCapturer)
            m_native->dxgiCapturer = std::make_unique<DxgiScreenCapturer>();
        const uint64_t previewInterval = uint64_t(std::max(1, m_captureFps / 10));
        const bool makePreview = (m_native->captureFrameNumber++ % previewInterval) == 0;
        QImage preview;
        auto native = m_native->dxgiCapturer->grabGpu(
            screenIndex, m_captureWidth, m_captureHeight, m_captureFps,
            &preview, makePreview);
        if (native) {
            if (!preview.isNull()) {
                if (preview.width() != native->width() || preview.height() != native->height())
                    preview = preview.scaled(native->width(), native->height(),
                                             Qt::IgnoreAspectRatio, Qt::FastTransformation);
                emit localPreviewFrame(preview);
            }
            m_native->videoSource->PushBuffer(native);
            return;
        }
    }
#endif
    if (m_captureSourceType == 0) {
        const int screenIndex = int(m_captureSourceId);
#ifdef Q_OS_WIN
        if (!m_native->dxgiCapturer) m_native->dxgiCapturer = std::make_unique<DxgiScreenCapturer>();
        frameImage = m_native->dxgiCapturer->grab(screenIndex);
#endif
        if (frameImage.isNull()) {
            const QList<QScreen*> screens = QGuiApplication::screens();
            if (screenIndex >= 0 && screenIndex < screens.size() && screens[screenIndex]) {
                pix = screens[screenIndex]->grabWindow(0);
            } else if (primary) {
                pix = primary->grabWindow(0);
            }
        }
    } else {
#ifdef Q_OS_WIN
        if (m_captureSourceId > 0) {
            HWND hwnd = reinterpret_cast<HWND>(m_captureSourceId);
            RECT wr = {};
            if (hwnd && !IsIconic(hwnd) && GetWindowRect(hwnd, &wr)) {
                const QPoint center((wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2);
                const QList<QScreen*> screens = QGuiApplication::screens();
                int screenIndex = 0;
                QScreen* targetScreen = primary;
                for (int i = 0; i < screens.size(); ++i) {
                    if (screens[i] && screens[i]->geometry().contains(center)) {
                        screenIndex = i;
                        targetScreen = screens[i];
                        break;
                    }
                }
                if (!m_native->dxgiCapturer) m_native->dxgiCapturer = std::make_unique<DxgiScreenCapturer>();
                QImage screenImage = m_native->dxgiCapturer->grab(screenIndex);
                if (!screenImage.isNull() && targetScreen) {
                    const QRect sg = targetScreen->geometry();
                    const qreal dprX = sg.width() > 0 ? qreal(screenImage.width()) / qreal(sg.width()) : 1.0;
                    const qreal dprY = sg.height() > 0 ? qreal(screenImage.height()) / qreal(sg.height()) : 1.0;
                    QRect crop(qRound((wr.left - sg.left()) * dprX),
                               qRound((wr.top - sg.top()) * dprY),
                               qRound((wr.right - wr.left) * dprX),
                               qRound((wr.bottom - wr.top) * dprY));
                    crop = crop.intersected(screenImage.rect());
                    if (crop.width() > 8 && crop.height() > 8) {
                        frameImage = screenImage.copy(crop);
                    }
                }
            }
            if (frameImage.isNull()) pix = grabWindowsAppForWebRtc(m_captureSourceId);
        }
#else
        if (primary && m_captureSourceId > 0) pix = primary->grabWindow(WId(m_captureSourceId));
#endif
        if (frameImage.isNull() && pix.isNull() && primary) pix = primary->grabWindow(0);
    }

    if (frameImage.isNull() && !pix.isNull()) frameImage = pix.toImage();
    if (frameImage.isNull()) return;
    QImage img;
    if (frameImage.width() == m_captureWidth && frameImage.height() == m_captureHeight) {
        img = frameImage;
    } else {
        img = frameImage.scaled(m_captureWidth, m_captureHeight, Qt::KeepAspectRatio, Qt::FastTransformation);
    }
    emit localPreviewFrame(img);
    m_native->videoSource->PushImage(img);
}
#endif

#ifndef HALLA_WEBRTC_NATIVE
void HallaWebRtcSession::startWatching(int userId) {
    Q_UNUSED(userId);
    emit unavailable(tr("Este build não contém o WebRTC nativo necessário para assistir transmissões."));
}
#endif

void HallaWebRtcSession::stopWatching(int userId) {
    if (userId <= 0) return;
    if (m_net) m_net->sendWebRtcWatchStop(userId);
#ifdef HALLA_WEBRTC_NATIVE
    closePeer(userId);
#endif
}

void HallaWebRtcSession::startBroadcast() {
#ifdef HALLA_WEBRTC_NATIVE
    if (m_broadcasting) return;
    resetNativeFactoryForEncoderSetting();
    if (!ensureNativeFactory()) {
        const QString reason = tr("Falha ao inicializar WebRTC nativo");
        AppLog::error(reason);
        emit unavailable(reason);
        return;
    }
    m_broadcasting = true;
    if (!m_captureTimer) {
        m_captureTimer = new QTimer(this);
        m_captureTimer->setTimerType(Qt::PreciseTimer);
        connect(m_captureTimer, &QTimer::timeout, this, [this] {
#ifdef HALLA_WEBRTC_NATIVE
            captureFrame();
#endif
        });
    }
    // Prepare a first frame (and the D3D11 adapter) before advertising the
    // live. This guarantees that the H.264 MFT is bound to the capture GPU.
    captureFrame();
    m_captureTimer->start(qMax(1, 1000 / m_captureFps));
    if (m_net) m_net->sendWebRtcStreamStart(
        m_captureWidth, m_captureHeight, m_captureFps, m_captureBitrateKbps);
    emit broadcastStarted();
#else
    const QString reason = tr("WebRTC nativo ainda não está disponível neste build. "
                              "O cliente continuará usando a transmissão clássica por enquanto.");
    AppLog::warn(reason);
    emit unavailable(reason);
#endif
}

void HallaWebRtcSession::stopBroadcast() {
    if (!m_broadcasting) return;
    m_broadcasting = false;
    if (m_captureTimer) m_captureTimer->stop();
#ifdef HALLA_WEBRTC_NATIVE
    while (!m_native->peers.empty()) closePeer(m_native->peers.begin()->first);
#endif
    if (m_net) m_net->sendWebRtcStreamStop();
    emit broadcastStopped();
}

void HallaWebRtcSession::handleSignal(const QJsonObject& signal) {
#ifdef HALLA_WEBRTC_NATIVE
    const QString type = signal.value(QStringLiteral("t")).toString();
    const int from = signal.value(QStringLiteral("from")).toInt();
    if (type == QLatin1String("webrtc_watch_request")) createOfferForPeer(from);
    else if (type == QLatin1String("webrtc_watch_stop")) closePeer(from);
    else if (type == QLatin1String("webrtc_offer")) setRemoteOffer(from, signal.value(QStringLiteral("sdp")).toString());
    else if (type == QLatin1String("webrtc_answer")) setRemoteAnswer(from, signal.value(QStringLiteral("sdp")).toString());
    else if (type == QLatin1String("webrtc_ice")) addRemoteIce(from, signal);
#else
    const QString type = signal.value(QStringLiteral("t")).toString();
    AppLog::info(QStringLiteral("WebRTC signaling recebido em build sem libwebrtc: %1").arg(type));
#endif
}

#ifdef HALLA_WEBRTC_NATIVE
void PeerObserver::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    if (!transceiver || !m_owner) return;
    auto receiver = transceiver->receiver();
    if (!receiver) return;
    auto track = receiver->track();
    if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        m_owner->attachRemoteVideoTrack(m_peerId,
            webrtc::scoped_refptr<webrtc::VideoTrackInterface>(static_cast<webrtc::VideoTrackInterface*>(track.get())));
    } else if (track && track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
        m_owner->attachRemoteAudioTrack(m_peerId,
            webrtc::scoped_refptr<webrtc::AudioTrackInterface>(static_cast<webrtc::AudioTrackInterface*>(track.get())));
    }
}

void PeerObserver::OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                              const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&) {
    if (!receiver || !m_owner) return;
    auto track = receiver->track();
    if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        m_owner->attachRemoteVideoTrack(m_peerId,
            webrtc::scoped_refptr<webrtc::VideoTrackInterface>(static_cast<webrtc::VideoTrackInterface*>(track.get())));
    } else if (track && track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
        m_owner->attachRemoteAudioTrack(m_peerId,
            webrtc::scoped_refptr<webrtc::AudioTrackInterface>(static_cast<webrtc::AudioTrackInterface*>(track.get())));
    }
}

void PeerObserver::OnIceCandidate(const webrtc::IceCandidate* candidate) {
    if (!candidate || !m_owner) return;
    m_owner->sendNativeIce(m_peerId, candidate->ToString(), candidate->sdp_mid(), candidate->sdp_mline_index());
}

void OfferObserver::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    if (!desc || !m_owner) return;
    std::string sdp = desc->ToString();
    if (auto* ctx = m_owner->ensurePeer(m_peerId)) {
        ctx->pc->SetLocalDescription(new webrtc::RefCountedObject<NoopSetObserver>(), desc);
    } else {
        delete desc;
    }
    m_owner->sendNativeOffer(m_peerId, sdp);
}

void AnswerObserver::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    if (!desc || !m_owner) return;
    std::string sdp = desc->ToString();
    if (auto* ctx = m_owner->ensurePeer(m_peerId)) {
        ctx->pc->SetLocalDescription(new webrtc::RefCountedObject<NoopSetObserver>(), desc);
    } else {
        delete desc;
    }
    m_owner->sendNativeAnswer(m_peerId, sdp);
}

void RemoteOfferSetObserver::OnSuccess() {
    if (!m_owner) return;
    m_owner->remoteDescriptionReady(m_peerId);
    m_owner->createAnswerForPeer(m_peerId);
}

void RemoteAnswerSetObserver::OnSuccess() {
    if (m_owner) m_owner->remoteDescriptionReady(m_peerId);
}
#endif
