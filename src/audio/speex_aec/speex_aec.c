/* Halla -- single translation unit that embeds the speexdsp AEC.
 *
 * Builds mdf.c + fftwrap.c + kiss_fft.c + kiss_fftr.c from the vendor tree
 * (src/audio/speex_aec/, imported from https://gitlab.xiph.org/xiph/speexdsp
 * -- BSD-like, see COPYING in the same directory) into a single object:
 *
 *  * FLOATING_POINT/USE_KISS_FFT: the same configuration speexdsp uses on
 *    desktops (floating point, kiss FFT), with no FFTW dependency.
 *  * Empty EXPORT: symbols stay internal to the object; the only public API is
 *    the one in speex_aec.h (halla_ prefix).
 *  * kiss_* symbols RENAMED to spx_kiss_*: the embedded RNNoise
 *    (src/audio/rnnoise/) also carries a copy of kiss_fft and both vendors
 *    must coexist in the same binary without a linker collision. Preprocessor
 *    macros rename the identifiers consistently in declarations and
 *    definitions (typedefs such as kiss_fft_cfg are distinct tokens and are
 *    not affected).
 *  * No HAVE_CONFIG_H: the .c files stay in the "no config.h" state and take
 *    their definitions from this wrapper. Without VAR_ARRAYS, fftwrap uses
 *    fixed MAX_FFT_SIZE buffers -- works even on MSVC (no VLA).
 */
#define FLOATING_POINT 1
#define USE_KISS_FFT 1
#define EXPORT

#define kiss_fft_alloc   spx_kiss_fft_alloc
#define kiss_fft_stride  spx_kiss_fft_stride
#define kiss_fft         spx_kiss_fft
#define kiss_fftr_alloc  spx_kiss_fftr_alloc
#define kiss_fftr        spx_kiss_fftr
#define kiss_fftri       spx_kiss_fftri
#define kiss_fftr2       spx_kiss_fftr2
#define kiss_fftri2      spx_kiss_fftri2

#include "kiss_fft.c"
#include "kiss_fftr.c"
#include "fftwrap.c"
#include "mdf.c"

#include "speex_aec.h"

void* halla_speex_aec_init(int frame_size, int filter_length) {
    SpeexEchoState* st = speex_echo_state_init(frame_size, filter_length);
    if (!st) return NULL;
    int rate = 48000;
    speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    return st;
}

void halla_speex_aec_destroy(void* st) {
    if (st) speex_echo_state_destroy((SpeexEchoState*)st);
}

void halla_speex_aec_reset(void* st) {
    if (st) speex_echo_state_reset((SpeexEchoState*)st);
}

void halla_speex_aec_playback(void* st, const int16_t* play) {
    if (!st || !play) return;
    speex_echo_playback((SpeexEchoState*)st, play);
}

void halla_speex_aec_capture(void* st, const int16_t* rec, int16_t* out) {
    if (!st || !rec || !out) return;
    speex_echo_capture((SpeexEchoState*)st, rec, out);
}
