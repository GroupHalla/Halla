#include "MediaFoundationH264.h"

#if defined(HALLA_WEBRTC_NATIVE) && defined(_WIN32)

#include "core/AppLog.h"

#include <windows.h>
#include <codecapi.h>
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
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "libyuv/convert.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"

namespace HallaMfH264 {
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

bool enumerateHardwareEncoder(ComPtr<IMFActivate>& selected) {
    if (!ensureMediaFoundation()) return false;
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags,
                                 &input, &output, &activations, &count);
    if (FAILED(hr) || count == 0 || !activations) return false;
    selected = activations[0];
    for (UINT32 i = 0; i < count; ++i) activations[i]->Release();
    CoTaskMemFree(activations);
    return selected != nullptr;
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
        ComPtr<IMFActivate> activation;
        if (!enumerateHardwareEncoder(activation)
                || FAILED(activation->ActivateObject(IID_PPV_ARGS(&m_transform)))) {
            AppLog::warn(QStringLiteral("WebRTC H264 hardware: nenhum encoder MFT disponível"));
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
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
        if (FAILED(m_transform->SetOutputType(m_outputId, output.Get(), 0))) {
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
        if (FAILED(m_transform->SetInputType(m_inputId, input.Get(), 0))) {
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
        AppLog::info(QStringLiteral("WebRTC: encoder H264 por hardware ativo (%1x%2 @ %3 FPS)")
                         .arg(m_width).arg(m_height).arg(m_fps));
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
        }
        m_events.Reset();
        m_transform.Reset();
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
        auto i420 = frame.video_frame_buffer() ? frame.video_frame_buffer()->ToI420() : nullptr;
        if (!i420 || i420->width() != m_width || i420->height() != m_height)
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        const bool forceKey = frameTypes && std::find(frameTypes->begin(), frameTypes->end(),
            webrtc::VideoFrameType::kVideoFrameKey) != frameTypes->end();
        if (forceKey) setCodecApiU32(m_transform.Get(), CODECAPI_AVEncVideoForceKeyFrame, TRUE);

        std::vector<uint8_t> nv12(size_t(m_width) * m_height * 3 / 2);
        if (libyuv::I420ToNV12(i420->DataY(), i420->StrideY(),
                              i420->DataU(), i420->StrideU(),
                              i420->DataV(), i420->StrideV(),
                              nv12.data(), m_width,
                              nv12.data() + size_t(m_width) * m_height, m_width,
                              m_width, m_height) != 0)
            return WEBRTC_VIDEO_CODEC_ERROR;

        if (m_async && !waitForEvent(METransformNeedInput, 150))
            return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        MFCreateSample(&sample);
        MFCreateMemoryBuffer(DWORD(nv12.size()), &buffer);
        BYTE* bytes = nullptr;
        DWORD maxLength = 0;
        if (!sample || !buffer || FAILED(buffer->Lock(&bytes, &maxLength, nullptr)))
            return WEBRTC_VIDEO_CODEC_MEMORY;
        std::memcpy(bytes, nv12.data(), std::min<size_t>(maxLength, nv12.size()));
        buffer->Unlock();
        buffer->SetCurrentLength(DWORD(nv12.size()));
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(frame.timestamp_us() * 10);
        sample->SetSampleDuration(10000000ll / m_fps);
        HRESULT hr = m_transform->ProcessInput(m_inputId, sample.Get(), 0);
        if (FAILED(hr)) return WEBRTC_VIDEO_CODEC_ERROR;
        if (m_async && !waitForEvent(METransformHaveOutput, 250))
            return WEBRTC_VIDEO_CODEC_NO_OUTPUT;

        std::vector<uint8_t> encoded;
        hr = takeOutput(encoded);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
        if (FAILED(hr) || encoded.empty()) return WEBRTC_VIDEO_CODEC_ERROR;
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
        info.supports_native_handle = false;
        info.has_trusted_rate_controller = false;
        info.requested_resolution_alignment = 2;
        info.scaling_settings = ScalingSettings::kOff;
        return info;
    }

private:
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
    webrtc::EncodedImageCallback* m_callback = nullptr;
    DWORD m_inputId = 0;
    DWORD m_outputId = 0;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 30;
    uint32_t m_bitrate = 4000000;
    bool m_async = false;
    bool m_initialized = false;
    bool m_comInitialized = false;
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
    ComPtr<IMFActivate> activation;
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
