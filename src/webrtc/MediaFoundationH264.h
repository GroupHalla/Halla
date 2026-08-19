#pragma once

#include <memory>
#include <vector>

#ifdef HALLA_WEBRTC_NATIVE
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"

namespace HallaMfH264 {

bool encoderAvailable();
std::vector<webrtc::SdpVideoFormat> formats();
std::unique_ptr<webrtc::VideoEncoder> createEncoder();
std::unique_ptr<webrtc::VideoDecoder> createDecoder();

} // namespace HallaMfH264
#endif
