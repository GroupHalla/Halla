#include "EchoGuard.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr int kFrame = 960;  // 20 ms @ 48 kHz (espelha o contrato do header)
constexpr int kSub = kFrame / 4;
} // namespace
// ------------------------------------------------------------------ push
void EchoGuard::pushRing(std::vector<int16_t>& ring, int64_t& total,
                          const int16_t* pcm, int samples) {
    if (!pcm || samples < kFrame) return;
    if (ring.empty()) ring.assign(size_t(kRingFrames) * size_t(kSub), 0);
    // Decimação por 4 com média móvel (anti-aliasing grosseiro): mantém a
    // correlação de voz (300-3400 Hz) intacta e corta o custo 4x.
    const size_t slot = size_t(total % kRingFrames) * size_t(kSub);
    for (int i = 0; i < kSub; ++i) {
        const int s = int(pcm[i * 4]) + int(pcm[i * 4 + 1])
                    + int(pcm[i * 4 + 2]) + int(pcm[i * 4 + 3]);
        ring[slot + size_t(i)] = int16_t(s / 4);
    }
    ++total;
}

void EchoGuard::notePlayout(const int16_t* pcm, int samples) {
    const int64_t before = m_playTotal;
    pushRing(m_playRing, m_playTotal, pcm, samples);
    if (m_playTotal == before) return;

    // Energia por quadro para o fast-path de silêncio (rms^2 normalizado).
    if (m_playEnergy.empty()) m_playEnergy.assign(size_t(kRingFrames), 0.0);
    const size_t slot = size_t((m_playTotal - 1) % kRingFrames) * size_t(kSub);
    double energy = 0.0;
    for (int i = 0; i < kSub; ++i)
        energy += double(m_playRing[slot + size_t(i)])
                * double(m_playRing[slot + size_t(i)]);
    m_playEnergy[size_t((m_playTotal - 1) % kRingFrames)] = energy / double(kSub);

    // O playout avançou: com validação em curso, o correspondente do
    // crosstalk pode ter acabado de chegar — tenta confirmar já.
    if (m_validating && m_capTotal - m_lastTestFrame >= kTestPeriodFrames)
        tryConfirmEcho(kConfirmEcho);
}

// ------------------------------------------------------------- correlação
// Copia [endSub - kWindowSub, endSub) do ring (linear com wrap) para "out".
bool EchoGuard::copyWindow(const std::vector<int16_t>& ring, int64_t endSub,
                           std::vector<int16_t>& out) {
    if (ring.empty()) return false;
    const int64_t ringLen = int64_t(kRingFrames) * kSub;
    const int64_t begin = endSub - int64_t(kWindowSub);
    if (begin < 0) return false;
    if (out.size() != size_t(kWindowSub)) out.assign(size_t(kWindowSub), 0);
    int64_t startIdx = begin % ringLen;
    const size_t first = size_t(
        std::min<int64_t>(ringLen - startIdx, int64_t(kWindowSub)));
    std::memcpy(out.data(), &ring[size_t(startIdx)], first * sizeof(int16_t));
    if (first < size_t(kWindowSub)) {
        std::memcpy(out.data() + first, ring.data(),
                    (size_t(kWindowSub) - first) * sizeof(int16_t));
    }
    return true;
}

bool EchoGuard::tryConfirmEcho(double threshold) {
    m_lastTestFrame = m_capTotal;
    if (m_playTotal < 5 || m_capTotal < 5) return false;

    // Janela de referência: os últimos 100 ms do playout.
    if (!copyWindow(m_playRing, m_playTotal * kSub, m_playWin))
        return false;
    double playEnergy = 0.0;
    for (int i = 0; i < kWindowSub; ++i)
        playEnergy += double(m_playWin[size_t(i)]) * double(m_playWin[size_t(i)]);
    const double playRms = std::sqrt(playEnergy / double(kWindowSub));
    if (playRms < kMinVoiceRmsSub) return false;   // playout mudo: sem eco

    // Procura a janela de captura (por IDADE, o mic é adiantado em relação
    // ao playout no crosstalk de rede) que casa com o playout atual.
    for (int ageSub = kSub; ageSub <= kSearchMaxSub;
         ageSub += kSearchStepSub) {
        if (!copyWindow(m_capRing, m_capTotal * kSub - ageSub, m_capWin))
            continue;
        double capEnergy = 0.0;
        for (int i = 0; i < kWindowSub; ++i)
            capEnergy += double(m_capWin[size_t(i)])
                       * double(m_capWin[size_t(i)]);
        // Energias comparáveis — early-out barato antes do dot.
        if (capEnergy < playEnergy * 0.05 || capEnergy > playEnergy * 20.0)
            continue;
        double dot = 0.0;
        for (int i = 0; i < kWindowSub; ++i)
            dot += double(m_capWin[size_t(i)]) * double(m_playWin[size_t(i)]);
        const double cosv = dot / std::sqrt(capEnergy * playEnergy);
        if (cosv >= threshold) {
            m_lastMatchLagMs = int(ageSub) / 12;  // 1 sub-amostra = 1/12 ms
            m_lastMatchScore = float(cosv);
            return true;
        }
    }
    return false;
}

bool EchoGuard::playoutSilentRecently() const {
    if (m_playTotal == 0 || m_playEnergy.empty()) return true;
    const int frames = int(std::min<int64_t>(m_playTotal, kSilenceFrames));
    double sum = 0.0;
    for (int i = 0; i < frames; ++i) {
        const size_t idx = size_t((m_playTotal - 1 - i) % kRingFrames);
        sum += m_playEnergy[idx];
    }
    const double rms = std::sqrt(sum / double(frames > 0 ? frames : 1));
    return rms < kMinVoiceRmsSub;
}

// ----------------------------------------------------------------- decisão
EchoGuard::Decision EchoGuard::noteCapture(const int16_t* pcm, int samples,
                                            bool aboveThreshold,
                                            bool transmitGateOpen) {
    pushRing(m_capRing, m_capTotal, pcm, samples);
    if (!aboveThreshold) {
        // A fala acabou no meio da validação: não há mais nada para casar
        // com o playout — cancela (a próxima fala recomeça do zero).
        m_validating = false;
        return Decision::Open;
    }

    if (transmitGateOpen) {
        // Revogação: microfone DOMINADO pelo eco enquanto transmite. Mistura
        // com fala própria real fica bem abaixo de kRevokeEcho e não revoga.
        if (m_capTotal - m_lastTestFrame >= kTestPeriodFrames) {
            if (tryConfirmEcho(kRevokeEcho)) {
                m_validating = false;
                m_echoUntilFrame = m_capTotal + kEchoHoldFrames;
            }
        }
        return m_echoUntilFrame > m_capTotal ? Decision::Blocked
                                             : Decision::Open;
    }

    // Abertura do gate.
    if (m_echoUntilFrame > m_capTotal) return Decision::Blocked;
    if (playoutSilentRecently()) return Decision::Open;   // não há fonte de eco
    if (!m_validating) {
        m_validating = true;
        m_validateStart = m_capTotal;
    }
    if (m_capTotal - m_lastTestFrame >= kTestPeriodFrames) {
        if (tryConfirmEcho(kConfirmEcho)) {
            m_validating = false;
            m_echoUntilFrame = m_capTotal + kEchoHoldFrames;
            return Decision::Blocked;
        }
    }
    if (m_capTotal - m_validateStart >= kValidateFrames) {
        // 400 ms sem casar com o playout: fala legítima.
        m_validating = false;
        return Decision::Open;
    }
    return Decision::Hold;
}

void EchoGuard::reset() {
    m_playRing.clear();
    m_capRing.clear();
    m_playWin.clear();
    m_capWin.clear();
    m_playEnergy.clear();
    m_playTotal = 0;
    m_capTotal = 0;
    m_validating = false;
    m_validateStart = 0;
    m_echoUntilFrame = 0;
    m_lastTestFrame = 0;
    m_lastMatchLagMs = 0;
    m_lastMatchScore = 0.0f;
}
