/* Halla — cancelador de eco acústico embutido (AUMDF do speexdsp).
 *
 * O build sem o SDK nativo do libwebrtc precisa de um AEC que funcione em
 * QUALQUER binário distribuído: este módulo embute o echo canceller clássico
 * do speexdsp (mdf.c, Jean-Marc Valin / Xiph.Org) — o mesmo algoritmo usado
 * em produção por softphones livres há duas décadas — compilado junto com o
 * aplicativo, sem dependências externas.
 *
 * API enxuta com prefixo halla_ para não vazar símbolos do speex no resto do
 * binário. Quadros de 480 amostras mono @ 48 kHz (10 ms), o mesmo contrato do
 * HallaAudioProcessing.
 */

#ifndef HALLA_SPEEX_AEC_H
#define HALLA_SPEEX_AEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cria o cancelador. frame_size em amostras (use 480 @ 48 kHz);
 * filter_length em amostras de eco a cancelar — 250 ms (12000) cobre o
 * buffer do alto-falante (~160 ms) + o caminho acústico da sala.
 * Retorna NULL em caso de falha de alocação. */
void* halla_speex_aec_init(int frame_size, int filter_length);

void halla_speex_aec_destroy(void* st);

/* Zera o filtro adaptativo (após troca de dispositivo, por exemplo). */
void halla_speex_aec_reset(void* st);

/* Alimenta a referência far-end: o mix que está indo para o alto-falante.
 * Chamar ANTES do capture correspondente ao mesmo instante lógico. */
void halla_speex_aec_playback(void* st, const int16_t* play);

/* Processa o microfone e devolve em out[] o sinal sem o eco. rec e out
 * podem apontar para o mesmo buffer. */
void halla_speex_aec_capture(void* st, const int16_t* rec, int16_t* out);

#ifdef __cplusplus
}
#endif

#endif /* HALLA_SPEEX_AEC_H */
