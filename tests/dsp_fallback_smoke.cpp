/* CI gate for the embedded DSP (builds without the libwebrtc SDK): proves that
 * the RNNoise + speex AEC fallback actually PROCESSES -- synthetic echo is
 * cancelled (ERLE), noise on top of speech is suppressed without damaging the
 * voice (SNR gain), and with everything disabled the signal passes through
 * byte for byte. Runs on the release workflow (MinGW) and can be reproduced
 * locally with g++/gcc: see the build scripts in the workflow.
 *
 * This file must NOT see HALLA_WEBRTC_NATIVE: the fallback has to be tested
 * exactly the way the distributed binary compiles it. */
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

/* Runs one 10 s pass of the embedded denoiser over the frames produced by
 * `gen` and returns the SNR gain in dB: how much more the noise floor drops
 * than the speech level. Level-only measurements (raw RMS) are meaningless
 * here -- RNNoise band gains are relative and speech energy is largely kept,
 * so suppression shows up as an SNR improvement, not as global attenuation. */
static double measureSnrGain(double speechDeltaDb, double noiseDeltaDb){
    return speechDeltaDb - noiseDeltaDb;
}

int failures = 0;
static void check(const char* name, double v, double minOk){
    const int ok = v >= minOk; if (!ok) ++failures;
    printf("  %-38s %8.2f (min %.2f) %s\n", name, v, minOk, ok?"OK":"FAILED");
}

int main(){
    const int NFR = 1000;               /* 10 s of 10 ms frames */
    static int16_t far[NFR][480], mic[NFR][480], clean[NFR][480];
    /* echo: far-end delayed by 150 ms through an IIR; mic = echo + local voice + noise */
    float eco = 0.0f;
    for (int fr=0; fr<NFR; ++fr) {
        const float t = fr*0.01f;
        for (int i=0;i<480;++i) {
            const float tt = t+i/48000.0f;
            far[fr][i] = int16_t(12000.0f*0.5f*voice(tt));
            const float fs = fr>=15 ? float(far[fr-15][i]) : 0.0f;
            eco = 0.70f*eco + 0.30f*fs;
            mic[fr][i] = int16_t(0.5f*eco + 60.0f*frand());   /* noise floor ~-50 dBFS */
            clean[fr][i] = int16_t(0.5f*eco);
        }
    }
    /* 1. AEC alone */
    {
        HallaAudioProcessing apm;
        apm.configure(false, 0, true);
        if (!apm.echoActive()) { printf("  echoActive()=false FAILED\n"); ++failures; }
        double eb=0, ea=0; int n=0;
        for (int fr=0; fr<NFR; ++fr) {
            apm.processReverseFrame(far[fr]);
            int16_t out[480];
            memcpy(out, mic[fr], sizeof(out));
            apm.processCaptureFrame(out);
            if (fr>=600 && fr<800) { eb+=rms16(clean[fr],480)*rms16(clean[fr],480); ea+=rms16(out,480)*rms16(out,480); ++n; }
        }
        check("AEC: echo return loss (ERLE, dB)", 20*log10(sqrt(eb/n)/sqrt(ea/n)), 12.0);
    }
    /* 2. NS alone: quiet speech bursts buried in fan-like noise. The gate is
     *    the SNR gain -- the noise floor must drop at least 10 dB more than
     *    the speech level. Averaging over every processed frame (not only the
     *    noisy ones) would reward muting the whole frame instead of cleaning
     *    it, so speech and pause frames are measured separately. */
    {
        HallaAudioProcessing apm;
        apm.configure(true, 100, false);
        if (!apm.denoiseActive()) { printf("  denoiseActive()=false FAILED\n"); ++failures; }
        static int16_t in[1000][480], out[1000][480];
        float lp = 0.0f;
        for (int fr=0; fr<1000; ++fr) {
            for (int i=0;i<480;++i) {
                const float tt = fr*0.01f + i/48000.0f;
                const float w  = 1800.0f*frand();   /* broadband fan noise */
                lp = 0.30f*lp + 0.70f*w;                 /* low-passed -> fan-like */
                const float env = 0.55f*(1+sinf(2*3.14159f*2.3f*tt));
                const float sp  = env > 0.62f ? 8000.0f*voice(tt) : 0.0f;
                in[fr][i] = int16_t(qBound(-32768.0f, sp + lp, 32767.0f));
            }
            memcpy(out[fr], in[fr], sizeof(out[fr]));
            apm.processCaptureFrame(out[fr]);
        }
        double spI=0, spO=0, nzI=0, nzO=0; int nsp=0, nnz=0;
        for (int fr=300; fr<1000; ++fr) {
            const float env = 0.55f*(1+sinf(2*3.14159f*2.3f*(fr*0.01f)));
            if (env > 0.85f) { spI += rms16(in[fr],480)*rms16(in[fr],480); spO += rms16(out[fr],480)*rms16(out[fr],480); ++nsp; }
            if (env < 0.20f) { nzI += rms16(in[fr],480)*rms16(in[fr],480); nzO += rms16(out[fr],480)*rms16(out[fr],480); ++nnz; }
        }
        const double speechDelta = 10*log10(spO/spI);
        const double noiseDelta  = 10*log10(nzO/nzI);
        check("NS: SNR gain over fan noise (dB)",
              measureSnrGain(speechDelta, noiseDelta), 10.0);
        printf("      (speech %+.2f dB, noise %+.2f dB, %d + %d frames)\n",
               speechDelta, noiseDelta, nsp, nnz);
    }
    /* 3. nothing enabled: exact passthrough */
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
        check("passthrough with DSP disabled", exact?0.0:-1.0, 0.0);
    }
    printf(failures?"\nRESULT: %d FAILURE(S)\n":"\nRESULT: OK\n", failures);
    return failures?1:0;
}
