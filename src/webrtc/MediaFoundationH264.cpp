#include "MediaFoundationH264.h"

#if defined(HALLA_WEBRTC_NATIVE) && defined(_WIN32)

#include "core/AppLog.h"

#include <windows.h>
#include <codecapi.h>
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "api/make_ref_counted.h"
#include "api/video/encoded_image.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "libyuv/convert.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"

namespace HallaMfH264 {

namespace {
std::mutex& preferredDeviceMutex() {
    static std::mutex mutex;
    return mutex;
}
Microsoft::WRL::ComPtr<ID3D11Device>& preferredDeviceStorage() {
    static Microsoft::WRL::ComPtr<ID3D11Device> device;
    return device;
}
void rememberPreferredDevice(ID3D11Device* device) {
    if (!device) return;
    std::lock_guard<std::mutex> lock(preferredDeviceMutex());
    preferredDeviceStorage() = device;
}
Microsoft::WRL::ComPtr<ID3D11Device> preferredDevice() {
    std::lock_guard<std::mutex> lock(preferredDeviceMutex());
    return preferredDeviceStorage();
}
} // namespace

struct D3D11FrameBuffer::Impl {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    int width = 0;
    int height = 0;
    std::function<void()> onRelease;
};

D3D11FrameBuffer::D3D11FrameBuffer(ID3D11Texture2D* texture, int width, int height,
                                   std::function<void()> onRelease)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->texture = texture;
    if (texture) texture->GetDevice(&m_impl->device);
    rememberPreferredDevice(m_impl->device.Get());
    m_impl->width = width;
    m_impl->height = height;
    m_impl->onRelease = std::move(onRelease);
}

D3D11FrameBuffer::~D3D11FrameBuffer() {
    if (m_impl && m_impl->onRelease) m_impl->onRelease();
}

webrtc::VideoFrameBuffer::Type D3D11FrameBuffer::type() const {
    return Type::kNative;
}

int D3D11FrameBuffer::width() const { return m_impl ? m_impl->width : 0; }
int D3D11FrameBuffer::height() const { return m_impl ? m_impl->height : 0; }
ID3D11Texture2D* D3D11FrameBuffer::texture() const {
    return m_impl ? m_impl->texture.Get() : nullptr;
}
ID3D11Device* D3D11FrameBuffer::device() const {
    return m_impl ? m_impl->device.Get() : nullptr;
}

std::string D3D11FrameBuffer::storage_representation() const {
    return "D3D11 NV12 GPU texture";
}

webrtc::scoped_refptr<webrtc::I420BufferInterface> D3D11FrameBuffer::ToI420() {
    if (!m_impl || !m_impl->texture || !m_impl->device
            || m_impl->width < 2 || m_impl->height < 2) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    m_impl->texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12) return nullptr;

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(m_impl->device->CreateTexture2D(&stagingDesc, nullptr, &staging)))
        return nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    m_impl->device->GetImmediateContext(&context);
    if (!context) return nullptr;
    context->CopyResource(staging.Get(), m_impl->texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return nullptr;

    auto i420 = webrtc::I420Buffer::Create(m_impl->width, m_impl->height);
    const uint8_t* y = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t* uv = y + size_t(mapped.RowPitch) * m_impl->height;
    const int converted = libyuv::NV12ToI420(
        y, int(mapped.RowPitch), uv, int(mapped.RowPitch),
        i420->MutableDataY(), i420->StrideY(),
        i420->MutableDataU(), i420->StrideU(),
        i420->MutableDataV(), i420->StrideV(),
        m_impl->width, m_impl->height);
    context->Unmap(staging.Get(), 0);
    return converted == 0 ? i420 : nullptr;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> createD3D11FrameBuffer(
        ID3D11Texture2D* texture, int width, int height,
        std::function<void()> onRelease) {
    if (!texture || width < 2 || height < 2) return nullptr;
    return webrtc::make_ref_counted<D3D11FrameBuffer>(
        texture, width, height, std::move(onRelease));
}

namespace {
using Microsoft::WRL::ComPtr;

bool ensureMediaFoundation() {
    static std::once_flag once;
    static bool available = false;
    std::call_once(once, [] {
        available = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    });
    return available;
}

std::vector<webrtc::SdpVideoFormat> h264Formats() {
    return {
        webrtc::SdpVideoFormat("H264", {
            {"profile-level-id", "42e01f"},
            {"level-asymmetry-allowed", "1"},
            {"packetization-mode", "1"},
        }),
    };
}

struct HardwareEncoderActivation {
    ComPtr<IMFActivate> activation;
    QString name;
    QString vendor;
};

QString activationString(IMFActivate* activation, const GUID& key) {
    if (!activation) return {};
    WCHAR* value = nullptr;
    UINT32 length = 0;
    if (FAILED(activation->GetAllocatedString(key, &value, &length)) || !value)
        return {};
    const QString result = QString::fromWCharArray(value, int(length));
    CoTaskMemFree(value);
    return result;
}

QString d3dVendorId(ID3D11Device* device) {
    if (!device) return {};
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || !dxgiDevice
            || FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter
            || FAILED(adapter->GetDesc(&desc))) return {};
    return QStringLiteral("VEN_%1").arg(desc.VendorId, 4, 16, QLatin1Char('0')).toUpper();
}

bool enumerateHardwareEncoder(HardwareEncoderActivation& selected,
                              const QString& preferredVendor = {}) {
    if (!ensureMediaFoundation()) return false;
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags,
                                 &input, &output, &activations, &count);
    if (FAILED(hr) || count == 0 || !activations) return false;

    std::vector<HardwareEncoderActivation> encoders;
    encoders.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        HardwareEncoderActivation candidate;
        candidate.activation = activations[i];
        candidate.name = activationString(activations[i], MFT_FRIENDLY_NAME_Attribute);
        candidate.vendor = activationString(
            activations[i], MFT_ENUM_HARDWARE_VENDOR_ID_Attribute).toUpper();
        encoders.push_back(std::move(candidate));
        activations[i]->Release();
    }
    CoTaskMemFree(activations);
    if (encoders.empty()) return false;

    auto chooseVendor = [&](const QString& vendor) -> bool {
        if (vendor.isEmpty()) return false;
        const auto it = std::find_if(encoders.begin(), encoders.end(),
            [&](const HardwareEncoderActivation& item) {
                return item.vendor.compare(vendor, Qt::CaseInsensitive) == 0;
            });
        if (it == encoders.end()) return false;
        selected = *it;
        return true;
    };
    // A textura DXGI e o MFT devem usar o mesmo adaptador. Sem uma textura
    // nativa, prefira GPU dedicada (NVIDIA/AMD) antes do encoder da iGPU.
    if (chooseVendor(preferredVendor)
            || chooseVendor(QStringLiteral("VEN_10DE"))
            || chooseVendor(QStringLiteral("VEN_1002"))) return true;
    selected = encoders.front();
    return selected.activation != nullptr;
}

void setCodecApiU32(IMFTransform* transform, const GUID& key, UINT32 value) {
    if (!transform) return;
    ComPtr<ICodecAPI> api;
    if (FAILED(transform->QueryInterface(IID_PPV_ARGS(&api))) || !api) return;
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_UI4;
    variant.ulVal = value;
    api->SetValue(&key, &variant);
    VariantClear(&variant);
}

bool containsIdr(const std::vector<uint8_t>& data) {
    for (size_t i = 0; i + 4 < data.size(); ++i) {
        size_t nal = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) nal = i + 3;
        else if (i + 4 < data.size() && data[i] == 0 && data[i + 1] == 0
                 && data[i + 2] == 0 && data[i + 3] == 1) nal = i + 4;
        if (nal && nal < data.size() && (data[nal] & 0x1f) == 5) return true;
    }
    return false;
}

void normalizeAnnexB(std::vector<uint8_t>& data) {
    if (data.size() < 4) return;
    if ((data[0] == 0 && data[1] == 0 && data[2] == 1)
            || (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)) return;
    std::vector<uint8_t> annexb;
    size_t offset = 0;
    while (offset + 4 <= data.size()) {
        const uint32_t length = (uint32_t(data[offset]) << 24)
            | (uint32_t(data[offset + 1]) << 16)
            | (uint32_t(data[offset + 2]) << 8)
            | uint32_t(data[offset + 3]);
        offset += 4;
        if (length == 0 || offset + length > data.size()) return;
        annexb.insert(annexb.end(), {0, 0, 0, 1});
        annexb.insert(annexb.end(), data.begin() + ptrdiff_t(offset),
                      data.begin() + ptrdiff_t(offset + length));
        offset += length;
    }
    if (offset == data.size() && !annexb.empty()) data.swap(annexb);
}

class MfH264Encoder final : public webrtc::VideoEncoder {
public:
    ~MfH264Encoder() override { Release(); }

    int InitEncode(const webrtc::VideoCodec* codec,
                   const webrtc::VideoEncoder::Settings&) override {
        if (!codec || codec->width < 2 || codec->height < 2 || !ensureMediaFoundation())
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        Release();
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_comInitialized = SUCCEEDED(com);
        m_d3dDevice = preferredDevice();
        HardwareEncoderActivation selected;
        if (!enumerateHardwareEncoder(selected, d3dVendorId(m_d3dDevice.Get()))
                || !selected.activation
                || FAILED(selected.activation->ActivateObject(IID_PPV_ARGS(&m_transform)))) {
            AppLog::warn(QStringLiteral("WebRTC H264 hardware: nenhum encoder MFT disponível"));
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        m_encoderName = selected.name.isEmpty()
            ? QStringLiteral("H.264 hardware MFT") : selected.name;
        m_encoderVendor = selected.vendor;
        m_width = codec->width & ~1;
        m_height = codec->height & ~1;
        m_fps = std::clamp<int>(codec->maxFramerate, 1, 60);
        m_bitrate = std::max<uint32_t>(500000, codec->startBitrate * 1000u);

        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(m_transform->GetAttributes(&attrs)) && attrs) {
            UINT32 async = FALSE;
            attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async);
            m_async = async != FALSE;
            if (m_async) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }

        if (m_d3dDevice) {
            ComPtr<ID3D11DeviceContext> context;
            ComPtr<ID3D10Multithread> multithread;
            m_d3dDevice->GetImmediateContext(&context);
            if (context && SUCCEEDED(context.As(&multithread)) && multithread)
                multithread->SetMultithreadProtected(TRUE);
            UINT resetToken = 0;
            HRESULT managerHr = MFCreateDXGIDeviceManager(&resetToken, &m_deviceManager);
            if (SUCCEEDED(managerHr))
                managerHr = m_deviceManager->ResetDevice(m_d3dDevice.Get(), resetToken);
            if (SUCCEEDED(managerHr))
                managerHr = m_transform->ProcessMessage(
                    MFT_MESSAGE_SET_D3D_MANAGER,
                    reinterpret_cast<ULONG_PTR>(m_deviceManager.Get()));
            m_gpuSurfaceInput = SUCCEEDED(managerHr);
            if (!m_gpuSurfaceInput) {
                AppLog::warn(QStringLiteral(
                    "WebRTC H264 hardware: %1 recusou o gerenciador D3D11 (0x%2); "
                    "encode continua por GPU, mas com upload pela CPU")
                    .arg(m_encoderName)
                    .arg(quint32(managerHr), 8, 16, QLatin1Char('0')));
                m_deviceManager.Reset();
                m_d3dDevice.Reset();
            }
        }

        if (FAILED(m_transform.As(&m_events))) m_async = false;
        if (FAILED(m_transform->GetStreamIDs(1, &m_inputId, 1, &m_outputId))) {
            m_inputId = 0;
            m_outputId = 0;
        }

        ComPtr<IMFMediaType> output;
        MFCreateMediaType(&output);
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        output->SetUINT32(MF_MT_AVG_BITRATE, m_bitrate);
        output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
        MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, m_fps, 1);
        MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        HRESULT typeHr = m_transform->SetOutputType(m_outputId, output.Get(), 0);
        if (FAILED(typeHr)) {
            AppLog::error(QStringLiteral(
                "WebRTC H264 hardware: %1 rejeitou saída %2x%3 @ %4 FPS (0x%5)")
                .arg(m_encoderName).arg(m_width).arg(m_height).arg(m_fps)
                .arg(quint32(typeHr), 8, 16, QLatin1Char('0')));
            Release();
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        ComPtr<IMFMediaType> input;
        MFCreateMediaType(&input);
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        input->SetUINT32(MF_MT_DEFAULT_STRIDE, m_width);
        MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
        MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, m_fps, 1);
        MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        typeHr = m_transform->SetInputType(m_inputId, input.Get(), 0);
        if (FAILED(typeHr)) {
            AppLog::error(QStringLiteral(
                "WebRTC H264 hardware: %1 rejeitou entrada NV12 %2x%3 (0x%4)")
                .arg(m_encoderName).arg(m_width).arg(m_height)
                .arg(quint32(typeHr), 8, 16, QLatin1Char('0')));
            Release();
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        setCodecApiU32(m_transform.Get(), CODECAPI_AVEncCommonRateControlMode,
                       eAVEncCommonRateControlMode_LowDelayVBR);
        setCodecApiU32(m_transform.Get(), CODECAPI_AVEncCommonMeanBitRate, m_bitrate);
        setCodecApiU32(m_transform.Get(), CODECAPI_AVEncCommonRealTime, TRUE);
        m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        m_initialized = true;
        AppLog::info(QStringLiteral(
            "WebRTC: encoder H264 por hardware ativo: %1 [%2] (%3x%4 @ %5 FPS; entrada %6)")
            .arg(m_encoderName, m_encoderVendor)
            .arg(m_width).arg(m_height).arg(m_fps)
            .arg(m_gpuSurfaceInput ? QStringLiteral("D3D11/NV12 na GPU")
                                   : QStringLiteral("NV12 pela RAM")));
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterEncodeCompleteCallback(
            webrtc::EncodedImageCallback* callback) override {
        m_callback = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override {
        if (m_transform) {
            m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            if (m_deviceManager)
                m_transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        }
        m_events.Reset();
        m_transform.Reset();
        m_deviceManager.Reset();
        m_d3dDevice.Reset();
        m_nv12.clear();
        m_encoderName.clear();
        m_encoderVendor.clear();
        m_gpuSurfaceInput = false;
        m_inputPathLogged = false;
        m_runtimeErrorLogged = false;
        m_perfStarted = {};
        m_perfFrames = 0;
        m_perfGpuFrames = 0;
        m_perfEncodeMs = 0.0;
        m_initialized = false;
        if (m_comInitialized) {
            CoUninitialize();
            m_comInitialized = false;
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Encode(const webrtc::VideoFrame& frame,
                   const std::vector<webrtc::VideoFrameType>* frameTypes) override {
        if (!m_initialized || !m_transform || !m_callback) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        const auto frameBuffer = frame.video_frame_buffer();
        if (!frameBuffer || frameBuffer->width() != m_width || frameBuffer->height() != m_height)
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        const bool forceKey = frameTypes && std::find(frameTypes->begin(), frameTypes->end(),
            webrtc::VideoFrameType::kVideoFrameKey) != frameTypes->end();
        if (forceKey) setCodecApiU32(m_transform.Get(), CODECAPI_AVEncVideoForceKeyFrame, TRUE);

        if (m_async && !waitForEvent(METransformNeedInput, 150)) {
            logRuntimeError(QStringLiteral("espera por METransformNeedInput"), HRESULT_FROM_WIN32(WAIT_TIMEOUT));
            return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
        }
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateSample(&sample);
        auto* native = frameBuffer->type() == webrtc::VideoFrameBuffer::Type::kNative
            ? dynamic_cast<D3D11FrameBuffer*>(frameBuffer.get()) : nullptr;
        const bool directGpu = SUCCEEDED(hr) && native && native->texture()
            && m_gpuSurfaceInput && native->device() == m_d3dDevice.Get();
        if (directGpu) {
            hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),
                                           native->texture(), 0, FALSE, &buffer);
            if (!m_inputPathLogged) {
                m_inputPathLogged = true;
                AppLog::info(QStringLiteral(
                    "WebRTC H264 hardware: caminho GPU zero-copy ativo "
                    "(DXGI -> NV12 D3D11 -> %1, %2)")
                    .arg(m_encoderName, m_encoderVendor));
            }
        } else {
            auto i420 = frameBuffer->ToI420();
            if (!i420 || i420->width() != m_width || i420->height() != m_height)
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            m_nv12.resize(size_t(m_width) * m_height * 3 / 2);
            if (libyuv::I420ToNV12(i420->DataY(), i420->StrideY(),
                                  i420->DataU(), i420->StrideU(),
                                  i420->DataV(), i420->StrideV(),
                                  m_nv12.data(), m_width,
                                  m_nv12.data() + size_t(m_width) * m_height, m_width,
                                  m_width, m_height) != 0)
                return WEBRTC_VIDEO_CODEC_ERROR;
            hr = MFCreateMemoryBuffer(DWORD(m_nv12.size()), &buffer);
            BYTE* bytes = nullptr;
            DWORD maxLength = 0;
            if (SUCCEEDED(hr)) hr = buffer->Lock(&bytes, &maxLength, nullptr);
            if (FAILED(hr) || !bytes || maxLength < m_nv12.size())
                return WEBRTC_VIDEO_CODEC_MEMORY;
            std::memcpy(bytes, m_nv12.data(), m_nv12.size());
            buffer->Unlock();
            buffer->SetCurrentLength(DWORD(m_nv12.size()));
            if (!m_inputPathLogged) {
                m_inputPathLogged = true;
                AppLog::warn(QStringLiteral(
                    "WebRTC H264 hardware: %1 codifica na GPU, mas recebeu frames pela RAM; "
                    "a captura/conversão ainda pode limitar 1440p/4K")
                    .arg(m_encoderName));
            }
        }
        if (FAILED(hr) || !sample || !buffer || FAILED(sample->AddBuffer(buffer.Get())))
            return WEBRTC_VIDEO_CODEC_MEMORY;
        sample->SetSampleTime(frame.timestamp_us() * 10);
        sample->SetSampleDuration(10000000ll / m_fps);
        const auto encodeStarted = std::chrono::steady_clock::now();
        hr = m_transform->ProcessInput(m_inputId, sample.Get(), 0);
        if (FAILED(hr)) {
            logRuntimeError(QStringLiteral("ProcessInput"), hr);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (m_async && !waitForEvent(METransformHaveOutput, 250)) {
            logRuntimeError(QStringLiteral("espera por METransformHaveOutput"),
                            HRESULT_FROM_WIN32(WAIT_TIMEOUT));
            return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
        }

        std::vector<uint8_t> encoded;
        hr = takeOutput(encoded);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
        if (FAILED(hr) || encoded.empty()) {
            logRuntimeError(QStringLiteral("ProcessOutput"), FAILED(hr) ? hr : E_FAIL);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        normalizeAnnexB(encoded);
        const bool key = containsIdr(encoded) || forceKey;

        webrtc::EncodedImage image;
        image.SetEncodedData(webrtc::EncodedImageBuffer::Create(encoded.data(), encoded.size()));
        image._encodedWidth = m_width;
        image._encodedHeight = m_height;
        image.SetRtpTimestamp(frame.rtp_timestamp());
        image.capture_time_ms_ = frame.timestamp_us() / 1000;
        image.ntp_time_ms_ = frame.ntp_time_ms();
        image.rotation_ = frame.rotation();
        image.SetFrameType(key ? webrtc::VideoFrameType::kVideoFrameKey
                               : webrtc::VideoFrameType::kVideoFrameDelta);
        webrtc::CodecSpecificInfo info;
        info.codecType = webrtc::kVideoCodecH264;
        info.codecSpecific.H264.packetization_mode =
            webrtc::H264PacketizationMode::NonInterleaved;
        info.codecSpecific.H264.temporal_idx = 0xff;
        info.codecSpecific.H264.base_layer_sync = false;
        info.codecSpecific.H264.idr_frame = key;

        const auto now = std::chrono::steady_clock::now();
        if (m_perfStarted.time_since_epoch().count() == 0) m_perfStarted = now;
        ++m_perfFrames;
        if (directGpu) ++m_perfGpuFrames;
        m_perfEncodeMs += std::chrono::duration<double, std::milli>(now - encodeStarted).count();
        const double elapsed = std::chrono::duration<double>(now - m_perfStarted).count();
        if (elapsed >= 5.0) {
            AppLog::info(QStringLiteral(
                "WebRTC H264 hardware: %1 — %2 FPS codificados, %3 ms/frame, "
                "%4% entrada GPU")
                .arg(m_encoderName)
                .arg(double(m_perfFrames) / elapsed, 0, 'f', 1)
                .arg(m_perfEncodeMs / std::max<uint64_t>(1, m_perfFrames), 0, 'f', 1)
                .arg((100.0 * double(m_perfGpuFrames))
                     / double(std::max<uint64_t>(1, m_perfFrames)), 0, 'f', 0));
            m_perfStarted = now;
            m_perfFrames = 0;
            m_perfGpuFrames = 0;
            m_perfEncodeMs = 0.0;
        }
        m_callback->OnEncodedImage(image, &info);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void SetRates(const RateControlParameters& parameters) override {
        const uint32_t bitrate = parameters.bitrate.get_sum_bps();
        if (bitrate > 0) {
            m_bitrate = bitrate;
            setCodecApiU32(m_transform.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
        }
    }

    EncoderInfo GetEncoderInfo() const override {
        EncoderInfo info;
        info.implementation_name = "Windows Media Foundation H264";
        info.is_hardware_accelerated = true;
        info.supports_native_handle = true;
        info.has_trusted_rate_controller = false;
        info.requested_resolution_alignment = 2;
        info.scaling_settings = ScalingSettings::kOff;
        return info;
    }

private:
    void logRuntimeError(const QString& stage, HRESULT hr) {
        if (m_runtimeErrorLogged) return;
        m_runtimeErrorLogged = true;
        AppLog::error(QStringLiteral(
            "WebRTC H264 hardware: falha em %1 no encoder %2 (0x%3)")
            .arg(stage, m_encoderName)
            .arg(quint32(hr), 8, 16, QLatin1Char('0')));
    }

    bool waitForEvent(MediaEventType wanted, int timeoutMs) {
        if (!m_events) return true;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            ComPtr<IMFMediaEvent> event;
            const HRESULT hr = m_events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (FAILED(hr) || !event) return false;
            MediaEventType type = MEUnknown;
            event->GetType(&type);
            HRESULT status = S_OK;
            event->GetStatus(&status);
            if (FAILED(status)) return false;
            if (type == wanted) return true;
        }
        return false;
    }

    HRESULT takeOutput(std::vector<uint8_t>& output) {
        MFT_OUTPUT_STREAM_INFO streamInfo{};
        HRESULT hr = m_transform->GetOutputStreamInfo(m_outputId, &streamInfo);
        if (FAILED(hr)) return hr;
        MFT_OUTPUT_DATA_BUFFER out{};
        out.dwStreamID = m_outputId;
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        if (!(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            MFCreateSample(&sample);
            const DWORD capacity = std::max<DWORD>(streamInfo.cbSize,
                DWORD(size_t(m_width) * m_height * 3 / 2));
            MFCreateMemoryBuffer(capacity, &buffer);
            if (!sample || !buffer) return E_OUTOFMEMORY;
            sample->AddBuffer(buffer.Get());
            out.pSample = sample.Get();
        }
        DWORD status = 0;
        hr = m_transform->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (FAILED(hr)) return hr;
        ComPtr<IMFSample> result;
        if (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)
            result.Attach(out.pSample);
        else
            result = sample;
        if (!result) return E_FAIL;
        ComPtr<IMFMediaBuffer> contiguous;
        hr = result->ConvertToContiguousBuffer(&contiguous);
        if (FAILED(hr)) return hr;
        BYTE* data = nullptr;
        DWORD length = 0;
        hr = contiguous->Lock(&data, nullptr, &length);
        if (SUCCEEDED(hr)) {
            output.assign(data, data + length);
            contiguous->Unlock();
        }
        return hr;
    }

    ComPtr<IMFTransform> m_transform;
    ComPtr<IMFMediaEventGenerator> m_events;
    ComPtr<IMFDXGIDeviceManager> m_deviceManager;
    ComPtr<ID3D11Device> m_d3dDevice;
    webrtc::EncodedImageCallback* m_callback = nullptr;
    std::vector<uint8_t> m_nv12;
    QString m_encoderName;
    QString m_encoderVendor;
    std::chrono::steady_clock::time_point m_perfStarted{};
    uint64_t m_perfFrames = 0;
    uint64_t m_perfGpuFrames = 0;
    double m_perfEncodeMs = 0.0;
    DWORD m_inputId = 0;
    DWORD m_outputId = 0;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 30;
    uint32_t m_bitrate = 4000000;
    bool m_async = false;
    bool m_initialized = false;
    bool m_comInitialized = false;
    bool m_gpuSurfaceInput = false;
    bool m_inputPathLogged = false;
    bool m_runtimeErrorLogged = false;
};

class MfH264Decoder final : public webrtc::VideoDecoder {
public:
    ~MfH264Decoder() override { Release(); }

    bool Configure(const Settings& settings) override {
        m_settings = settings;
        return ensureMediaFoundation();
    }

    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
        m_callback = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override {
        m_transform.Reset();
        m_width = m_height = 0;
        if (m_comInitialized) {
            CoUninitialize();
            m_comInitialized = false;
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Decode(const webrtc::EncodedImage& input, int64_t) override {
        if (!m_callback || !input.data() || input.size() == 0) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        const int width = int(input._encodedWidth);
        const int height = int(input._encodedHeight);
        if (width <= 0 || height <= 0) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        if (!m_transform || width != m_width || height != m_height) {
            if (!initialize(width, height)) return WEBRTC_VIDEO_CODEC_ERROR;
        }
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        MFCreateSample(&sample);
        MFCreateMemoryBuffer(DWORD(input.size()), &buffer);
        BYTE* bytes = nullptr;
        if (!sample || !buffer || FAILED(buffer->Lock(&bytes, nullptr, nullptr)))
            return WEBRTC_VIDEO_CODEC_MEMORY;
        std::memcpy(bytes, input.data(), input.size());
        buffer->Unlock();
        buffer->SetCurrentLength(DWORD(input.size()));
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(input.capture_time_ms_ * 10000);
        HRESULT hr = m_transform->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr)) return WEBRTC_VIDEO_CODEC_ERROR;

        MFT_OUTPUT_STREAM_INFO info{};
        m_transform->GetOutputStreamInfo(0, &info);
        ComPtr<IMFSample> outSample;
        ComPtr<IMFMediaBuffer> outBuffer;
        MFT_OUTPUT_DATA_BUFFER out{};
        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            MFCreateSample(&outSample);
            MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize,
                DWORD(size_t(m_width) * m_height * 3 / 2)), &outBuffer);
            if (!outSample || !outBuffer) return WEBRTC_VIDEO_CODEC_MEMORY;
            outSample->AddBuffer(outBuffer.Get());
            out.pSample = outSample.Get();
        }
        DWORD status = 0;
        hr = m_transform->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
        if (FAILED(hr)) return WEBRTC_VIDEO_CODEC_ERROR;
        ComPtr<IMFSample> result;
        if (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) result.Attach(out.pSample);
        else result = outSample;
        if (!result) return WEBRTC_VIDEO_CODEC_ERROR;
        ComPtr<IMFMediaBuffer> contiguous;
        if (FAILED(result->ConvertToContiguousBuffer(&contiguous))) return WEBRTC_VIDEO_CODEC_ERROR;
        BYTE* nv12 = nullptr;
        DWORD length = 0;
        if (FAILED(contiguous->Lock(&nv12, nullptr, &length))) return WEBRTC_VIDEO_CODEC_ERROR;
        auto i420 = webrtc::I420Buffer::Create(m_width, m_height);
        const int converted = length >= size_t(m_width) * m_height * 3 / 2
            ? libyuv::NV12ToI420(nv12, m_stride,
                nv12 + size_t(m_stride) * m_height, m_stride,
                i420->MutableDataY(), i420->StrideY(),
                i420->MutableDataU(), i420->StrideU(),
                i420->MutableDataV(), i420->StrideV(), m_width, m_height)
            : -1;
        contiguous->Unlock();
        if (converted != 0) return WEBRTC_VIDEO_CODEC_ERROR;
        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(i420)
            .set_rtp_timestamp(input.RtpTimestamp())
            .set_timestamp_ms(input.capture_time_ms_)
            .set_rotation(input.rotation_)
            .build();
        m_callback->Decoded(frame);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    DecoderInfo GetDecoderInfo() const override {
        DecoderInfo info;
        info.implementation_name = "Windows Media Foundation H264 decoder";
        info.is_hardware_accelerated = false;
        return info;
    }

private:
    bool initialize(int width, int height) {
        Release();
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_comInitialized = SUCCEEDED(com);
        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_transform)))) return false;
        m_width = width & ~1;
        m_height = height & ~1;
        m_stride = m_width;
        ComPtr<IMFMediaType> input;
        MFCreateMediaType(&input);
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
        if (FAILED(m_transform->SetInputType(0, input.Get(), 0))) return false;
        ComPtr<IMFMediaType> output;
        MFCreateMediaType(&output);
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        output->SetUINT32(MF_MT_DEFAULT_STRIDE, m_stride);
        MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
        if (FAILED(m_transform->SetOutputType(0, output.Get(), 0))) return false;
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    Settings m_settings;
    ComPtr<IMFTransform> m_transform;
    webrtc::DecodedImageCallback* m_callback = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_stride = 0;
    bool m_comInitialized = false;
};

} // namespace

bool encoderAvailable() {
    HardwareEncoderActivation activation;
    return enumerateHardwareEncoder(activation);
}

std::vector<webrtc::SdpVideoFormat> formats() { return h264Formats(); }
std::unique_ptr<webrtc::VideoEncoder> createEncoder() {
    return std::make_unique<MfH264Encoder>();
}
std::unique_ptr<webrtc::VideoDecoder> createDecoder() {
    return std::make_unique<MfH264Decoder>();
}

} // namespace HallaMfH264

#elif defined(HALLA_WEBRTC_NATIVE)
namespace HallaMfH264 {
bool encoderAvailable() { return false; }
std::vector<webrtc::SdpVideoFormat> formats() { return {}; }
std::unique_ptr<webrtc::VideoEncoder> createEncoder() { return nullptr; }
std::unique_ptr<webrtc::VideoDecoder> createDecoder() { return nullptr; }
}
#endif
