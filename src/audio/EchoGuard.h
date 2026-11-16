#pragma once

#include <cstdint>
#include <vector>

// Crosstalk/network-echo guard for the transmission gate (v1.1.22).
//
// PROBLEM: when a user speaks, their voice plays on the speaker of the device
// next door (or enters the shared microphone of a second client on the same
// machine). The OTHER client's VAD opens with that sound and it starts
// transmitting/talking: on the speaker's screen, the "talking" ring lights up
// on two users at once, the "on speech" sound cue plays duplicated, and echo
// comes back audible ("dirty audio"). AEC3 does not cover this path: the AEC
// far-end reference is what the client ITSELF plays, and crosstalk enters
// through the air / a shared mic -- not through the retransmitter's speaker.
//
// SOLUTION: compare the microphone with what is being RECEIVED from the
// network. If the captured signal from X ms ago is (almost) a delayed copy of
// the current playout, the microphone is picking up crosstalk -- not the
// local user's voice. The correlation used is normalized (cosine), invariant
// to linear gain: acoustic-path attenuation does not matter.
//
// DECISIONS (noteCapture with the VAD wanting to open):
//   * Open     -- signal does not match the playout (legitimate speech; or
//                 silent playout: no crosstalk source, instant opening).
//   * Hold     -- validation in progress: hold the opening for up to 400 ms
//                 for the playout to receive the counterpart and match (or
//                 not) the lag.
//   * Blocked  -- confirmed echo: do not open (and revoke if already open).
//
// With the gate OPEN, an above-threshold frame matching the playout with VERY
// high correlation (>0.92, echo-dominated) revokes transmission. The
// "own speech + return echo" mix stays well below that and is not revoked --
// whoever is really speaking keeps transmitting.
//
// CONTRACT:
//   * notePlayout(): 960 mono samples @ 48 kHz (20 ms) of the mix going to
//     the speaker NOW -- same signal as the AEC3 reference.
//   * noteCapture(): 960 mono samples @ 48 kHz of the RAW microphone
//     (pre-APM -- AEC3 removes exactly the playout-correlated component this
//     guard needs to see).
//   * Everything on the SAME VoiceEngine thread (no locks); moc-free and
//     Qt-free so it can be linked standalone into the CI smoke gate.
//   * Graceful fallback: without fed playout, everything is Open.
class EchoGuard {
public:
    enum class Decision { Open, Hold, Blocked };

    void notePlayout(const int16_t* pcm, int samples);
    Decision noteCapture(const int16_t* pcm, int samples,
                         bool aboveThreshold, bool transmitGateOpen);

    bool validating() const { return m_validating; }
    void reset();

    // Diagnostics (log/smoke): last match that confirmed echo.
    int lastMatchLagMs() const { return m_lastMatchLagMs; }
    float lastMatchScore() const { return m_lastMatchScore; }
    // Support diagnostics ("mic stalling, chopping"): how many times the
    // guard blocked the transmission opening and how many times it revoked
    // mid-speech. Counters accumulate per engine (not reset by reset() --
    // support wants the whole session history, not the current state).
    int64_t blockCount() const { return m_blockCount; }
    int64_t revokeCount() const { return m_revokeCount; }

    static constexpr float kConfirmEcho = 0.78f;  // opening: probable echo
    static constexpr float kRevokeEcho = 0.92f;   // revocation: echo dominates the mic
    // Revocation requires SUSTAINED DOMINATION: 2 consecutive matches at the
    // revocation threshold (~120 ms) instead of a single 60 ms window. An
    // isolated match (own speech with a passage resembling the playout) used
    // to chop voice mid-word -- the "mic stalling, chopping" report from
    // users on speakers with the room talking together. Real echo persists
    // and revokes normally on the next test.
    static constexpr int kRevokeStreakFrames = 2;

private:
    bool tryConfirmEcho(double threshold);
    bool playoutSilentRecently() const;
    static bool copyWindow(const std::vector<int16_t>& ring, int64_t endSub,
                           std::vector<int16_t>& out);
    void pushRing(std::vector<int16_t>& ring, int64_t& total,
                  const int16_t* pcm, int samples);

    // 20 ms @ 48 kHz; decimation by 4 (moving average) -> 240 sub-samples.
    static constexpr int kFrame = 960;
    static constexpr int kSub = kFrame / 4;
    // Retained playout/capture: 1120 ms (100 ms window searched up to
    // 980 ms of age).
    static constexpr int kRingFrames = 56;
    static constexpr int kWindowSub = kSub * 5;       // 100 ms window
    static constexpr int kSearchMaxSub = kSub * 49;   // 980 ms
    static constexpr int kSearchStepSub = kSub / 4;   // 5 ms step
    // Fast path: playout without voice in the last 360 ms -> direct opening.
    static constexpr int kSilenceFrames = 18;
    static constexpr double kMinVoiceRmsSub = 25.0;
    // Confirmed echo blocks for 500 ms before revalidating (avoids flapping);
    // legitimate-speech validation lasts at most 400 ms.
    static constexpr int kEchoHoldFrames = 25;
    static constexpr int kValidateFrames = 20;
    // Correlation is expensive: runs at most once every 60 ms.
    static constexpr int kTestPeriodFrames = 3;

    std::vector<int16_t> m_playRing;
    std::vector<int16_t> m_capRing;
    std::vector<int16_t> m_playWin;
    std::vector<int16_t> m_capWin;
    std::vector<double> m_playEnergy;   // rms^2 per frame (fast path)
    int64_t m_playTotal = 0;
    int64_t m_capTotal = 0;
    bool m_validating = false;
    int64_t m_validateStart = 0;
    int64_t m_echoUntilFrame = 0;
    int64_t m_lastTestFrame = 0;
    int m_revokeStreak = 0;
    int64_t m_blockCount = 0;
    int64_t m_revokeCount = 0;
    int m_lastMatchLagMs = 0;
    float m_lastMatchScore = 0.0f;
};
