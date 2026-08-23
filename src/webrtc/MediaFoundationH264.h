#pragma once

#include <functional>
#include <memory>
#include <vector>

#ifdef HALLA_WEBRTC_NATIVE
#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"

#if defined(_WIN32)
struct ID3D11Device;
struct ID3D11Texture2D;
#endif

namespace HallaMfH264 {

bool encoderAvailable();
std::vector<webrtc::SdpVideoFormat> formats();
std::unique_ptr<webrtc::VideoEncoder> createEncoder();
std::unique_ptr<webrtc::VideoDecoder> createDecoder();

#if defined(_WIN32)
// Buffer nativo usado pelo caminho DXGI -> D3D11 Video Processor -> NVENC/MFT.
// A textura já está em NV12 na GPU; o encoder não precisa reconverter I420 na CPU.
class D3D11FrameBuffer : public webrtc::VideoFrameBuffer {
public:
    D3D11FrameBuffer(ID3D11Texture2D* texture, int width, int height,
                     std::function<void()> onRelease = {});

    Type type() const override;
    int width() const override;
    int height() const override;
    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
    std::string storage_representation() const override;

    ID3D11Texture2D* texture() const;
    ID3D11Device* device() const;

protected:
    ~D3D11FrameBuffer() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> createD3D11FrameBuffer(
    ID3D11Texture2D* texture, int width, int height,
    std::function<void()> onRelease = {});
#endif

} // namespace HallaMfH264
#endif
