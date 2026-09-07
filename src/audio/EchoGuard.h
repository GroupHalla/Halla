#pragma once

#include <cstdint>
#include <vector>

// Guarda de crosstalk/eco de rede para o gate de transmissão (v1.1.22).
//
// PROBLEMA: quando o usuário fala, a voz dele toca no alto-falante do
// dispositivo ao lado (ou entra direto no microfone compartilhado de um
// segundo cliente na mesma máquina). O VAD do OUTRO cliente abre com esse
// som e ele passa a transmitir/talking: na tela de quem falou, o anel de
// "falando" acende em dois usuários ao mesmo tempo, o sinal sonoro "ao
// falar" toca duplicado e o eco volta audível ("áudio sujo"). O AEC3 não
// cobre este caminho: a referência far-end do AEC é o que o PRÓPRIO cliente
// toca, e o crosstalk entra pelo ar/via mic compartilhado — não pelo
// alto-falante de quem retransmite.
//
// SOLUÇÃO: comparar o microfone com o que está sendo RECEBIDO da rede. Se o
// sinal capturado de X ms atrás é (quase) cópia atrasada do playout atual,
// o microfone está captando crosstalk — não a voz do usuário local. A
// correlação usada é normalizada (cosseno), invariante a ganho linear: a
// atenuação do caminho acústico não importa.
//
// DECISÕES (noteCapture com o VAD querendo abrir):
//   * Open     — sinal não casa com o playout (fala legítima; ou playout em
//                silêncio: não há fonte de crosstalk, abertura instantânea).
//   * Hold     — validação em curso: segurar a abertura por até 400 ms para
//                o playout receber o correspondente e casar (ou não) o lag.
//   * Blocked  — eco confirmado: não abrir (e revogar se já estiver aberto).
//
// Com o gate ABERTO, o quadro acima do limiar que casa com o playout com
// correlação MUITO alta (>0.92, dominado pelo eco) revoga a transmissão.
// A mistura "fala própria + eco de retorno" fica bem abaixo disso e não é
// revogada — quem está falando de verdade continua transmitindo.
//
// CONTRATO:
//   * notePlayout(): 960 amostras mono @ 48 kHz (20 ms) do mix que vai para
//     o alto-falante AGORA — mesmo sinal da referência do AEC3.
//   * noteCapture(): 960 amostras mono @ 48 kHz do microfone CRU
//     (pré-APM — o AEC3 remove justamente a componente correlacionada com
//     o playout que este guarda precisa enxergar).
//   * Tudo na MESMA thread do VoiceEngine (sem locks); moc-free e sem Qt
//     para poder ser linkado standalone no gate de smoke do CI.
//   * Fallback gracioso: sem playout alimentado, tudo é Open.
class EchoGuard {
public:
    enum class Decision { Open, Hold, Blocked };

    void notePlayout(const int16_t* pcm, int samples);
    Decision noteCapture(const int16_t* pcm, int samples,
                         bool aboveThreshold, bool transmitGateOpen);

    bool validating() const { return m_validating; }
    void reset();

    // Diagnóstico (log/smoke): último casamento que confirmou eco.
    int lastMatchLagMs() const { return m_lastMatchLagMs; }
    float lastMatchScore() const { return m_lastMatchScore; }

    static constexpr float kConfirmEcho = 0.78f;  // abertura: eco provável
    static constexpr float kRevokeEcho = 0.92f;   // revogação: eco domina o mic

private:
    bool tryConfirmEcho(double threshold);
    bool playoutSilentRecently() const;
    static bool copyWindow(const std::vector<int16_t>& ring, int64_t endSub,
                           std::vector<int16_t>& out);
    void pushRing(std::vector<int16_t>& ring, int64_t& total,
                  const int16_t* pcm, int samples);

    // 20 ms @ 48 kHz; decimação por 4 (média móvel) -> 240 sub-amostras.
    static constexpr int kFrame = 960;
    static constexpr int kSub = kFrame / 4;
    // Playout/captura retidos: 1120 ms (janela de 100 ms pesquisada até
    // 980 ms de idade).
    static constexpr int kRingFrames = 56;
    static constexpr int kWindowSub = kSub * 5;       // janela de 100 ms
    static constexpr int kSearchMaxSub = kSub * 49;   // 980 ms
    static constexpr int kSearchStepSub = kSub / 4;   // passo de 5 ms
    // Fast-path: playout sem voz nos últimos 360 ms -> abertura direta.
    static constexpr int kSilenceFrames = 18;
    static constexpr double kMinVoiceRmsSub = 25.0;
    // Eco confirmado bloqueia por 500 ms antes de revalidar (evita
    // flapping); a validação de fala legítima dura no máximo 400 ms.
    static constexpr int kEchoHoldFrames = 25;
    static constexpr int kValidateFrames = 20;
    // Correlação é cara: roda no máximo 1x a cada 60 ms.
    static constexpr int kTestPeriodFrames = 3;

    std::vector<int16_t> m_playRing;
    std::vector<int16_t> m_capRing;
    std::vector<int16_t> m_playWin;
    std::vector<int16_t> m_capWin;
    std::vector<double> m_playEnergy;   // rms^2 por quadro (fast-path)
    int64_t m_playTotal = 0;
    int64_t m_capTotal = 0;
    bool m_validating = false;
    int64_t m_validateStart = 0;
    int64_t m_echoUntilFrame = 0;
    int64_t m_lastTestFrame = 0;
    int m_lastMatchLagMs = 0;
    float m_lastMatchScore = 0.0f;
};
