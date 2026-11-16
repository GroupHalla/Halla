#include "EchoGuard.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr int kFrame = 960;  // 20 ms @ 48 kHz (mirrors the header contract)
constexpr int kSub = kFrame / 4;
} // namespace
// ------------------------------------------------------------------ push
void EchoGuard::pushRing(std::vector<int16_t>& ring, int64_t& total,
                          const int16_t* pcm, int samples) {
    if (!pcm || samples < kFrame) return;
    if (ring.empty()) ring.assign(size_t(kRingFrames) * size_t(kSub), 0);
    // Decimation by 4 with a moving average (coarse anti-aliasing): keeps
    // voice correlation (300-3400 Hz) intact and cuts the cost 4x.
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

    // Per-frame energy for the silence fast path (normalized rms^2).
    if (m_playEnergy.empty()) m_playEnergy.assign(size_t(kRingFrames), 0.0);
    const size_t slot = size_t((m_playTotal - 1) % kRingFrames) * size_t(kSub);
    double energy = 0.0;
    for (int i = 0; i < kSub; ++i)
        energy += double(m_playRing[slot + size_t(i)])
                * double(m_playRing[slot + size_t(i)]);
    m_playEnergy[size_t((m_playTotal - 1) % kRingFrames)] = energy / double(kSub);

    // Playout advanced: with validation in progress, the crosstalk
    // counterpart may have just arrived -- try to confirm now.
    if (m_validating && m_capTotal - m_lastTestFrame >= kTestPeriodFrames)
        tryConfirmEcho(kConfirmEcho);
}

// ------------------------------------------------------------- correlation
// Copies [endSub - kWindowSub, endSub) from the ring (linear with wrap) into "out".
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

    // Reference window: the last 100 ms of playout.
    if (!copyWindow(m_playRing, m_playTotal * kSub, m_playWin))
        return false;
    double playEnergy = 0.0;
    for (int i = 0; i < kWindowSub; ++i)
        playEnergy += double(m_playWin[size_t(i)]) * double(m_playWin[size_t(i)]);
    const double playRms = std::sqrt(playEnergy / double(kWindowSub));
    if (playRms < kMinVoiceRmsSub) return false;   // muted playout: no echo

    // Searches the capture window (by AGE -- the mic leads the playout in
    // network crosstalk) that matches the current playout.
    for (int ageSub = kSub; ageSub <= kSearchMaxSub;
         ageSub += kSearchStepSub) {
        if (!copyWindow(m_capRing, m_capTotal * kSub - ageSub, m_capWin))
            continue;
        double capEnergy = 0.0;
        for (int i = 0; i < kWindowSub; ++i)
            capEnergy += double(m_capWin[size_t(i)])
                       * double(m_capWin[size_t(i)]);
        // Comparable energies -- cheap early-out before the dot product.
        if (capEnergy < playEnergy * 0.05 || capEnergy > playEnergy * 20.0)
            continue;
        double dot = 0.0;
        for (int i = 0; i < kWindowSub; ++i)
            dot += double(m_capWin[size_t(i)]) * double(m_playWin[size_t(i)]);
        const double cosv = dot / std::sqrt(capEnergy * playEnergy);
        if (cosv >= threshold) {
            m_lastMatchLagMs = int(ageSub) / 12;  // 1 sub-sample = 1/12 ms
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

// ----------------------------------------------------------------- decision
EchoGuard::Decision EchoGuard::noteCapture(const int16_t* pcm, int samples,
                                            bool aboveThreshold,
                                            bool transmitGateOpen) {
    pushRing(m_capRing, m_capTotal, pcm, samples);
    if (!aboveThreshold) {
        // Speech ended mid-validation: there is nothing left to match
        // against the playout -- cancel (the next speech starts from zero).
        // The revocation streak also resets: evidence from an old speech
        // cannot revoke the next one alone.
        m_validating = false;
        m_revokeStreak = 0;
        return Decision::Open;
    }

    if (transmitGateOpen) {
        // Revocation: microphone DOMINATED by echo while transmitting. A mix
        // with real own speech stays well below kRevokeEcho and does not
        // revoke. Since v1.1.34 it requires kRevokeStreakFrames CONSECUTIVE
        // matches (sustained domination): an isolated match used to chop
        // legitimate speech mid-word -- the "mic stalling, chopping" of users
        // speaking with speakers on and the room talking together.
        if (m_capTotal - m_lastTestFrame >= kTestPeriodFrames) {
            if (tryConfirmEcho(kRevokeEcho)) {
                ++m_revokeStreak;
            } else {
                m_revokeStreak = 0;
            }
            if (m_revokeStreak >= kRevokeStreakFrames) {
                m_revokeStreak = 0;
                m_validating = false;
                m_echoUntilFrame = m_capTotal + kEchoHoldFrames;
                ++m_revokeCount;
            }
        }
        return m_echoUntilFrame > m_capTotal ? Decision::Blocked
                                             : Decision::Open;
    }

    // Gate opening. Evidence from revoking the previous transmission does
    // not count for this one: a new match must rebuild the streak.
    m_revokeStreak = 0;
    if (m_echoUntilFrame > m_capTotal) return Decision::Blocked;
    if (playoutSilentRecently()) return Decision::Open;   // no echo source
    if (!m_validating) {
        m_validating = true;
        m_validateStart = m_capTotal;
    }
    if (m_capTotal - m_lastTestFrame >= kTestPeriodFrames) {
        if (tryConfirmEcho(kConfirmEcho)) {
            m_validating = false;
            m_echoUntilFrame = m_capTotal + kEchoHoldFrames;
            ++m_blockCount;
            return Decision::Blocked;
        }
    }
    if (m_capTotal - m_validateStart >= kValidateFrames) {
        // 400 ms without matching the playout: legitimate speech.
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
    m_revokeStreak = 0;
    m_lastMatchLagMs = 0;
    m_lastMatchScore = 0.0f;
}
