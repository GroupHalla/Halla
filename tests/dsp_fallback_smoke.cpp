/* Gate de CI do DSP embutido (builds sem o SDK do libwebrtc): valida que o
 * fallback RNNoise + AEC do speex realmente PROCESSA — eco sintético é
 * cancelado (ERLE), ruído de ventoinha é suprimido e, com tudo desligado,
 * o sinal passa byte a byte. Roda no workflow de release (MinGW) e pode ser
 * reproduzido localmente com g++/gcc: ver scripts de build do workflow.
 *
 * Este arquivo NÃO deve ver HALLA_WEBRTC_NATIVE: o fallback precisa ser
 * testado exatamente como o binário distribuído o compila. */
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>

#include <QtGlobal>
#include "audio/HallaAudioProcessing.h"

static unsigned rng = 4242;
static float frand(void){rng=rng*1664525u+1013904223u;return (rng/4294967296.0f)*2-1;}
static double rms16(const int16_t* x,int n){double s=0;for(int i=0;i<n;i++)s+=(double)x[i]*x[i];return sqrt(s/n);}
static float voice(float t){float f=210+25*sinf(2*3.14159f*4.7f*t);float env=0.55f*(1+sinf(2*3.14159f*2.3f*t));
  return env*(0.62f*sinf(2*3.14159f*f*t)+0.27f*sinf(2*3.14159f*2*f*t)+0.11f*sinf(2*3.14159f*3.1f*f*t));}

int failures = 0;
static void check(const char* name, double v, double minOk){
    const int ok = v >= minOk; if (!ok) ++failures;
    printf("  %-38s %8.2f (min %.2f) %s\n", name, v, minOk, ok?"OK":"FALHOU");
}

int main(){
    const int NFR = 1000;               /* 10 s */
    static int16_t far[NFR][480], mic[NFR][480], clean[NFR][480];
    /* eco: far atrasado 150 ms por IIR; mic = eco + voz local + ruido */
    float eco = 0.0f;
    for (int fr=0; fr<NFR; ++fr) {
        const float t = fr*0.01f;
        for (int i=0;i<480;++i) {
            const float tt = t+i/48000.0f;
            far[fr][i] = int16_t(12000.0f*0.5f*voice(tt));
            const float fs = fr>=15 ? float(far[fr-15][i]) : 0.0f;
            eco = 0.70f*eco + 0.30f*fs;
            mic[fr][i] = int16_t(0.5f*eco + 60.0f*frand());   /* ruido ~ -50 dBFS */
            clean[fr][i] = int16_t(0.5f*eco);
        }
    }
    /* 1. AEC so */
    {
        HallaAudioProcessing apm;
        apm.configure(false, 0, true);
        if (!apm.echoActive()) { printf("  echoActive()=false FALHOU\n"); ++failures; }
        double eb=0, ea=0; int n=0;
        for (int fr=0; fr<NFR; ++fr) {
            apm.processReverseFrame(far[fr]);
            int16_t out[480];
            memcpy(out, mic[fr], sizeof(out));
            apm.processCaptureFrame(out);
            if (fr>=600 && fr<800) { eb+=rms16(clean[fr],480)*rms16(clean[fr],480); ea+=rms16(out,480)*rms16(out,480); ++n; }
        }
        check("AEC: ERLE do eco (dB)", 20*log10(sqrt(eb/n)/sqrt(ea/n)), 12.0);
    }
    /* 2. NS so: mic com ruido branco forte */
    {
        HallaAudioProcessing apm;
        apm.configure(true, 100, false);
        if (!apm.denoiseActive()) { printf("  denoiseActive()=false FALHOU\n"); ++failures; }
        double ri=0, ro=0; int n=0;
        for (int fr=0; fr<NFR; ++fr) {
            int16_t in[480], out[480];
            for (int i=0;i<480;++i) in[i] = int16_t(14000.0f*frand());
            memcpy(out, in, sizeof(out));
            apm.processCaptureFrame(out);
            if (fr>=300) { ri+=rms16(in,480)*rms16(in,480); ro+=rms16(out,480)*rms16(out,480); ++n; }
        }
        check("NS: reducao do ruido (dB)", 20*log10(sqrt(ri/n)/sqrt(ro/n)), 10.0);
    }
    /* 3. nada habilitado: passthrough exato */
    {
        HallaAudioProcessing apm;
        apm.configure(false, 0, false);
        bool exact = true;
        for (int fr=0; fr<50; ++fr) {
            int16_t in[480], out[480];
            for (int i=0;i<480;++i) in[i] = int16_t(9000.0f*frand());
            memcpy(out, in, sizeof(out));
            apm.processCaptureFrame(out);
            if (memcmp(in, out, sizeof(in))) { exact=false; break; }
        }
        check("passthrough desligado (0=falha)", exact?0.0:-1.0, 0.0);
    }
    printf(failures?"\nRESULTADO: %d FALHAS\n":"\nRESULTADO: OK\n", failures);
    return failures?1:0;
}
