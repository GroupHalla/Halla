/* Halla — unidade de tradução única que embute o AEC do speexdsp.
 *
 * Compila mdf.c + fftwrap.c + kiss_fft.c + kiss_fftr.c do vendor
 * (src/audio/speex_aec/, importado de https://gitlab.xiph.org/xiph/speexdsp
 * — BSD-like, ver COPYING no mesmo diretório) num único objeto:
 *
 *  * FLOATING_POINT/USE_KISS_FFT: mesma configuração que o speexdsp usa em
 *    desktops (ponto flutuante, FFT via kiss), sem dependência de FFTW.
 *  * EXPORT vazio: símbolos ficam internos ao objeto; a única API pública é
 *    a do speex_aec.h (prefixo halla_).
 *  * Símbolos kiss_* RENOMEADOS para spx_kiss_*: o RNNoise embutido
 *    (src/audio/rnnoise/) também carrega uma cópia do kiss_fft e os dois
 *    vendedores precisam coexistir no mesmo binário sem colisão de linker.
 *    Macros de preprocessador renomeiam os identificadores de forma
 *    consistente em declarações e definições (typedefs como kiss_fft_cfg são
 *    tokens distintos e não são afetados).
 *  * Sem HAVE_CONFIG_H: os .c ficam no estado "sem config.h" e recebem as
 *    definições deste wrapper. Sem VAR_ARRAYS o fftwrap usa buffers fixos
 *    MAX_FFT_SIZE — funciona até em MSVC (sem VLA).
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
