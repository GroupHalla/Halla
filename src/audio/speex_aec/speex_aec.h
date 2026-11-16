/* Halla -- embedded acoustic echo canceller (speexdsp AUMDF).
 *
 * Builds without the native libwebrtc SDK still need an AEC that works in
 * EVERY distributed binary: this module embeds the classic speexdsp echo
 * canceller (mdf.c, Jean-Marc Valin / Xiph.Org) -- the same algorithm free
 * softphones have run in production for two decades -- compiled into the
 * application itself, with no external dependency.
 *
 * Small halla_-prefixed API so speex symbols do not leak into the rest of the
 * binary. Mono 480-sample frames at 48 kHz (10 ms), the same contract as
 * HallaAudioProcessing.
 */

#ifndef HALLA_SPEEX_AEC_H
#define HALLA_SPEEX_AEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the canceller. frame_size in samples (480 at 48 kHz);
 * filter_length is the echo tail to cancel in samples -- 250 ms (12000)
 * covers the speaker buffer (~160 ms) plus the room acoustic path.
 * Returns NULL when allocation fails. */
void* halla_speex_aec_init(int frame_size, int filter_length);

void halla_speex_aec_destroy(void* st);

/* Clears the adaptive filter (after a device change, for example). */
void halla_speex_aec_reset(void* st);

/* Feeds the far-end reference: the mix that is going to the speaker.
 * Must be called BEFORE the capture of the same logical instant. */
void halla_speex_aec_playback(void* st, const int16_t* play);

/* Processes the microphone and writes the echo-reduced signal to out[].
 * rec and out may point to the same buffer. */
void halla_speex_aec_capture(void* st, const int16_t* rec, int16_t* out);

#ifdef __cplusplus
}
#endif

#endif /* HALLA_SPEEX_AEC_H */
