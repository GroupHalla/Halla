#include "HallaWebRtcSession.h"
#include "net/NetSession.h"
#include "core/AppLog.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QImage>
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
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

#ifdef HALLA_WEBRTC_NATIVE
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
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
        if (screenIndex < 0) screenIndex = 0;
        if (!m_duplication || m_screenIndex != screenIndex) {
            reset();
            if (!init(screenIndex)) return QImage();
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        Microsoft::WRL::ComPtr<IDXGIResource> resource;
        HRESULT hr = m_duplication->AcquireNextFrame(0, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return m_lastFrame;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            reset();
            return QImage();
        }
        if (FAILED(hr)) return QImage();

        QImage out;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.As(&texture)) && texture) {
            D3D11_TEXTURE2D_DESC desc = {};
            texture->GetDesc(&desc);
            ensureStaging(desc);
            if (m_staging) {
                m_context->CopyResource(m_staging.Get(), texture.Get());
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (SUCCEEDED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    out = QImage(int(desc.Width), int(desc.Height), QImage::Format_ARGB32);
                    const int rowBytes = int(desc.Width) * 4;
                    for (int y = 0; y < out.height(); ++y) {
                        memcpy(out.scanLine(y),
                               static_cast<const char*>(mapped.pData) + y * mapped.RowPitch,
                               rowBytes);
                    }
                    m_context->Unmap(m_staging.Get(), 0);
                }
            }
        }
        m_duplication->ReleaseFrame();
        if (!out.isNull()) m_lastFrame = out;
        return out;
    }

private:
    bool init(int screenIndex) {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()))))
            return false;

        Microsoft::WRL::ComPtr<IDXGIAdapter1> selectedAdapter;
        Microsoft::WRL::ComPtr<IDXGIOutput> selectedOutput;
        int current = 0;
        for (UINT ai = 0; ; ++ai) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;
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
        HRESULT hr = D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       levels, UINT(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                                       &m_device, &level, &m_context);
        if (FAILED(hr)) return false;

        Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
        if (FAILED(selectedOutput.As(&output1))) return false;
        if (FAILED(output1->DuplicateOutput(m_device.Get(), &m_duplication))) {
            reset();
            return false;
        }
        m_screenIndex = screenIndex;
        AppLog::info(QStringLiteral("WebRTC DXGI Desktop Duplication ativo na tela #%1").arg(screenIndex));
        return true;
    }

    void ensureStaging(const D3D11_TEXTURE2D_DESC& srcDesc) {
        if (m_staging && m_width == srcDesc.Width && m_height == srcDesc.Height) return;
        m_staging.Reset();
        D3D11_TEXTURE2D_DESC desc = srcDesc;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        if (SUCCEEDED(m_device->CreateTexture2D(&desc, nullptr, &m_staging))) {
            m_width = srcDesc.Width;
            m_height = srcDesc.Height;
        }
    }

    void reset() {
        m_duplication.Reset();
        m_staging.Reset();
        m_context.Reset();
        m_device.Reset();
        m_lastFrame = QImage();
        m_width = 0;
        m_height = 0;
        m_screenIndex = -1;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    QImage m_lastFrame;
    UINT m_width = 0;
    UINT m_height = 0;
    int m_screenIndex = -1;
};


class SystemLoopbackAudioDeviceModule
    : public webrtc::webrtc_impl::AudioDeviceModuleDefault<webrtc::AudioDeviceModuleForTest> {
public:
    ~SystemLoopbackAudioDeviceModule() override { StopRecording(); }

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
    int32_t StartRecording() override {
        if (m_running.exchange(true)) return 0;
        m_thread = std::thread([this] { captureLoop(); });
        return 0;
    }
    int32_t StopRecording() override {
        if (!m_running.exchange(false)) return 0;
        if (m_thread.joinable()) m_thread.join();
        return 0;
    }
    bool Recording() const override { return m_running.load(); }
    int RestartPlayoutInternally() override { return 0; }
    int RestartRecordingInternally() override { StopRecording(); return StartRecording(); }
    int SetPlayoutSampleRate(uint32_t) override { return 0; }
    int SetRecordingSampleRate(uint32_t sample_rate) override { m_forcedSampleRate = sample_rate; return 0; }

private:
    static bool isFloatFormat(const WAVEFORMATEX* fmt) {
        if (!fmt) return false;
        if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
            return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }
        return false;
    }
    static bool isPcmFormat(const WAVEFORMATEX* fmt) {
        if (!fmt) return false;
        if (fmt->wFormatTag == WAVE_FORMAT_PCM) return true;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
            return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
        }
        return false;
    }
    static int16_t clamp16(float v) {
        if (v > 32767.0f) return 32767;
        if (v < -32768.0f) return -32768;
        return static_cast<int16_t>(v);
    }

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

    void captureLoop() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comOk = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
        Microsoft::WRL::ComPtr<IMMDevice> device;
        Microsoft::WRL::ComPtr<IAudioClient> client;
        Microsoft::WRL::ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* mix = nullptr;

        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.GetAddressOf()))) ||
            FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))) ||
            FAILED(client->GetMixFormat(&mix))) {
            AppLog::warn(QStringLiteral("WASAPI loopback: falha ao abrir dispositivo padrão de reprodução"));
            if (mix) CoTaskMemFree(mix);
            if (comOk && hr != RPC_E_CHANGED_MODE) CoUninitialize();
            m_running = false;
            return;
        }

        const REFERENCE_TIME bufferDuration = 10000000; // 1s
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK,
                                bufferDuration, 0, mix, nullptr);
        if (FAILED(hr) || FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture.GetAddressOf()))) ||
            FAILED(client->Start())) {
            AppLog::warn(QStringLiteral("WASAPI loopback: falha ao iniciar captura"));
            CoTaskMemFree(mix);
            if (comOk && hr != RPC_E_CHANGED_MODE) CoUninitialize();
            m_running = false;
            return;
        }

        const uint32_t sampleRate = m_forcedSampleRate ? m_forcedSampleRate : mix->nSamplesPerSec;
        const int channels = std::max<int>(1, mix->nChannels);
        const bool isFloat = isFloatFormat(mix);
        const bool isPcm = isPcmFormat(mix);
        const int bits = mix->wBitsPerSample;
        AppLog::info(QStringLiteral("WASAPI loopback ativo: %1 Hz, %2 canais, %3 bits")
                         .arg(sampleRate).arg(channels).arg(bits));

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
                } else if (isFloat && bits == 32) {
                    const float* f = reinterpret_cast<const float*>(data);
                    for (UINT32 i = 0; i < frames; ++i) {
                        float acc = 0.0f;
                        for (int c = 0; c < channels; ++c) acc += f[i * channels + c];
                        mono[i] = clamp16((acc / float(channels)) * 32767.0f);
                    }
                } else if (isPcm && bits == 16) {
                    const int16_t* in = reinterpret_cast<const int16_t*>(data);
                    for (UINT32 i = 0; i < frames; ++i) {
                        int acc = 0;
                        for (int c = 0; c < channels; ++c) acc += in[i * channels + c];
                        mono[i] = int16_t(acc / channels);
                    }
                }
                if (!mono.empty()) pushSamples(mono.data(), mono.size(), sampleRate);
                capture->ReleaseBuffer(frames);
                if (FAILED(capture->GetNextPacketSize(&packetFrames))) break;
            }
        }
        client->Stop();
        CoTaskMemFree(mix);
        if (comOk && hr != RPC_E_CHANGED_MODE) CoUninitialize();
        m_running = false;
    }

    std::atomic<webrtc::AudioTransport*> m_audioCallback{nullptr};
    std::atomic<bool> m_running{false};
    std::thread m_thread;
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

class HallaVp8EncoderFactory : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return { webrtc::SdpVideoFormat("VP8") };
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                   std::optional<std::string> scalabilityMode,
                                   std::optional<webrtc::Resolution> resolution) const override {
        Q_UNUSED(scalabilityMode);
        Q_UNUSED(resolution);
        return { format.name == "VP8" || format.name == "vp8", false };
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override {
        if (format.name != "VP8" && format.name != "vp8") return nullptr;
        return webrtc::CreateVp8Encoder(env);
    }
};

class HallaVp8DecoderFactory : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return { webrtc::SdpVideoFormat("VP8") };
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                   bool referenceScaling,
                                   std::optional<webrtc::Resolution> resolution) const override {
        Q_UNUSED(referenceScaling);
        Q_UNUSED(resolution);
        return { format.name == "VP8" || format.name == "vp8", false };
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override {
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
        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(buffer)
            .set_timestamp_ms(QDateTime::currentMSecsSinceEpoch())
            .set_rotation(webrtc::kVideoRotation_0)
            .build();

        std::vector<webrtc::VideoSinkInterface<webrtc::VideoFrame>*> sinks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastWidth = w;
            m_lastHeight = h;
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
    bool sslInitialized = false;
#endif
};

HallaWebRtcSession::HallaWebRtcSession(NetSession* net, QObject* parent)
    : QObject(parent), m_native(std::make_unique<NativeState>()), m_net(net) {}

HallaWebRtcSession::~HallaWebRtcSession() {
    stopBroadcast();
#ifdef HALLA_WEBRTC_NATIVE
    if (m_native) {
        m_native->peers.clear();
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
    m_captureBitrateKbps = qBound(800, bitrateKbps, 20000);
    if (m_captureTimer && m_captureTimer->isActive()) {
        m_captureTimer->start(qMax(1, 1000 / m_captureFps));
    }
}

void HallaWebRtcSession::setCaptureSystemAudio(bool enabled) {
    m_captureSystemAudio = enabled;
    if (enabled) {
        AppLog::info(QStringLiteral("WebRTC: captura de áudio de todo o PC solicitada (WASAPI loopback pendente)"));
    }
}

#ifdef HALLA_WEBRTC_NATIVE
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
    m_native->factory = webrtc::CreatePeerConnectionFactory(
        m_native->networkThread.get(), m_native->workerThread.get(), m_native->signalingThread.get(),
#ifdef Q_OS_WIN
        m_native->loopbackAdm,
#else
        nullptr,
#endif
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        std::make_unique<HallaVp8EncoderFactory>(),
        std::make_unique<HallaVp8DecoderFactory>(),
        nullptr, nullptr);
    if (m_native->factory) {
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
    webrtc::PeerConnectionInterface::IceServer stun;
    stun.urls.push_back("stun:stun.l.google.com:19302");
    cfg.servers.push_back(stun);
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
    ctx->pc->SetRemoteDescription(new webrtc::RefCountedObject<NoopSetObserver>(), desc.release());
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

void HallaWebRtcSession::deliverRemoteFrame(int peerId, const QImage& image) {
    emit remoteFrameReceived(peerId, image);
}

void HallaWebRtcSession::addRemoteIce(int peerId, const QJsonObject& signal) {
    PeerContext* ctx = ensurePeer(peerId);
    if (!ctx || !ctx->pc) return;
    const QString candidate = signal.value(QStringLiteral("candidate")).toString();
    if (candidate.isEmpty()) return;
    std::unique_ptr<webrtc::IceCandidate> ice(webrtc::CreateIceCandidate(
        signal.value(QStringLiteral("sdpMid")).toString(QStringLiteral("0")).toStdString(),
        signal.value(QStringLiteral("sdpMLineIndex")).toInt(0),
        candidate.toStdString(), nullptr));
    if (ice) ctx->pc->AddIceCandidate(std::move(ice), [](webrtc::RTCError) {});
}

void HallaWebRtcSession::closePeer(int peerId) {
    auto it = m_native->peers.find(peerId);
    if (it != m_native->peers.end()) {
        for (auto& binding : it->second->remoteVideo) {
            if (binding.track && binding.sink) binding.track->RemoveSink(binding.sink.get());
        }
        if (it->second->pc) it->second->pc->Close();
        m_native->peers.erase(it);
    }
}

void HallaWebRtcSession::captureFrame() {
    if (!m_broadcasting || !m_native || !m_native->videoSource) return;

    QImage frameImage;
    QPixmap pix;
    QScreen* primary = QGuiApplication::primaryScreen();
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
        if (m_captureSourceId > 0) pix = grabWindowsAppForWebRtc(m_captureSourceId);
#else
        if (primary && m_captureSourceId > 0) pix = primary->grabWindow(WId(m_captureSourceId));
#endif
        if (pix.isNull() && primary) pix = primary->grabWindow(0);
    }

    if (frameImage.isNull() && !pix.isNull()) frameImage = pix.toImage();
    if (frameImage.isNull()) return;
    QImage img = frameImage.scaled(m_captureWidth, m_captureHeight, Qt::KeepAspectRatio, Qt::FastTransformation);
    emit localPreviewFrame(img);
    m_native->videoSource->PushImage(img);
}
#endif

void HallaWebRtcSession::startBroadcast() {
#ifdef HALLA_WEBRTC_NATIVE
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
    m_captureTimer->start(qMax(1, 1000 / m_captureFps));
    if (m_net) m_net->sendWebRtcStreamStart();
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
    for (auto& kv : m_native->peers) if (kv.second->pc) kv.second->pc->Close();
    m_native->peers.clear();
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
    }
}

void PeerObserver::OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                              const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&) {
    if (!receiver || !m_owner) return;
    auto track = receiver->track();
    if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        m_owner->attachRemoteVideoTrack(m_peerId,
            webrtc::scoped_refptr<webrtc::VideoTrackInterface>(static_cast<webrtc::VideoTrackInterface*>(track.get())));
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
    if (m_owner) m_owner->createAnswerForPeer(m_peerId);
}
#endif
