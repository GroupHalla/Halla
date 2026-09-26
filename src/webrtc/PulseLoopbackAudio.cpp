#include "PulseLoopbackAudio.h"

#if defined(HALLA_WEBRTC_NATIVE) && defined(Q_OS_LINUX)

#include "core/AppLog.h"

#include <QByteArray>
#include <QScopeGuard>
#include <QString>

// pa_simple_* (gravação bloqueante do monitor no setup e no fallback do
// loopback) vive em pulse/simple.h, que NÃO é puxado por pulseaudio.h.
#include <pulse/simple.h>
#include <pulse/pulseaudio.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Nome do sink virtual que isola o Halla (único por processo: duas
// instâncias do app não colidem e módulos órfãos de um crash são
// identificáveis).
std::string isoSinkName()
{
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "halla-iso-%ld", static_cast<long>(::getpid()));
    return std::string(buffer);
}

// Sincronização das operações Pulse (padrão threaded mainloop): cada
// callback roda na thread da mainloop COM a lock; signal() acorda quem
// espera em await() (que devolve a lock enquanto dorme e re-checa a
// condição — sem lost wakeup).
struct OpWait {
    pa_threaded_mainloop* ml = nullptr;
    bool done = false;
    void signal()
    {
        done = true;
        pa_threaded_mainloop_signal(ml, 0);
    }
    void await() // requer lock held
    {
        while (!done) pa_threaded_mainloop_wait(ml);
    }
};

const char* propString(const pa_proplist* list, const char* key)
{
    return list ? pa_proplist_gets(list, key) : nullptr;
}

bool isOwnProcess(const pa_proplist* list)
{
    const char* pidText = propString(list, PA_PROP_APPLICATION_PROCESS_ID);
    if (!pidText) return false;
    char own[32] = {};
    std::snprintf(own, sizeof(own), "%ld", static_cast<long>(::getpid()));
    return std::strcmp(pidText, own) == 0;
}

} // namespace

// Infraestrutura de exclusão: mainloop dedicada + módulos carregados +
// lista de sink-inputs do Halla movidos para o sink virtual. O destrutor
// roda na thread que destrói o ADM (depois do join do captureLoop — nunca
// de dentro da mainloop, o que seria deadlock no pa_threaded_mainloop_stop).
struct LinuxLoopbackAudioDeviceModule::PulseExclusion {
    pa_threaded_mainloop* ml = nullptr;
    pa_context* ctx = nullptr;
    bool started = false; // stop() em mainloop nunca iniciada não é seguro
    std::string defaultSink;
    bool connected = false;
    bool modulesLoaded = false;
    uint32_t nullSinkModule = PA_INVALID_INDEX;
    uint32_t loopbackModule = PA_INVALID_INDEX;
    uint32_t isoSinkIndex = PA_INVALID_INDEX;
    std::vector<uint32_t> movedInputs;
    int pendingMoves = 0;
    // Quando não-nulo, o último move a completar sinaliza este OpWait —
    // tanto os moves do setup quanto os do teardown decrementam o MESMO
    // contador; sem isto, se o último a completar fosse um move do setup,
    // o await() do teardown esperaria para sempre (deadlock).
    OpWait* moveWait = nullptr;

    ~PulseExclusion()
    {
        if (!ml) return;
        if (ctx) {
            pa_context_disconnect(ctx);
            pa_context_unref(ctx);
            ctx = nullptr;
        }
        if (started) {
            pa_threaded_mainloop_stop(ml);
            started = false;
        }
        pa_threaded_mainloop_free(ml);
        ml = nullptr;
    }
};

// ---------------------------------------------------------------------------
// Ciclo de vida do ADM
// ---------------------------------------------------------------------------

// Construtor/destrutor out-of-line de propósito: o ADM nasce via
// make_ref_counted<>, cujo RefCountedObject<T>::RefCountedObject() é um
// template header-only do SDK — com o construtor implícito o GCC instancia,
// no TU chamador, o caminho de cleanup de exceção que destrói o
// unique_ptr<PulseExclusion> (sizeof de tipo incompleto lá). Fora de linha,
// a instanciação fica só neste .cpp, onde PulseExclusion é completo.
LinuxLoopbackAudioDeviceModule::LinuxLoopbackAudioDeviceModule() = default;

LinuxLoopbackAudioDeviceModule::~LinuxLoopbackAudioDeviceModule()
{
    StopPlayout();
    StopRecording();
}

int32_t LinuxLoopbackAudioDeviceModule::RegisterAudioCallback(
        webrtc::AudioTransport* audioCallback)
{
    m_audioCallback.store(audioCallback, std::memory_order_release);
    return 0;
}

int32_t LinuxLoopbackAudioDeviceModule::ActiveAudioLayer(
        webrtc::AudioDeviceModule::AudioLayer* audioLayer) const
{
    if (audioLayer) *audioLayer = webrtc::AudioDeviceModule::kLinuxPulseAudio;
    return 0;
}

int32_t LinuxLoopbackAudioDeviceModule::RecordingIsAvailable(bool* available)
{
    if (available) *available = true;
    return 0;
}

bool LinuxLoopbackAudioDeviceModule::RecordingIsInitialized() const { return true; }
int32_t LinuxLoopbackAudioDeviceModule::InitRecording() { return 0; }

int32_t LinuxLoopbackAudioDeviceModule::PlayoutIsAvailable(bool* available)
{
    if (available) *available = true;
    return 0;
}

bool LinuxLoopbackAudioDeviceModule::PlayoutIsInitialized() const { return true; }
int32_t LinuxLoopbackAudioDeviceModule::InitPlayout() { return 0; }

int32_t LinuxLoopbackAudioDeviceModule::StartPlayout()
{
    // Start/Stop podem chegar de threads diferentes (GUI x signaling do
    // libwebrtc): dois joins concorrentes no mesmo std::thread são UB e
    // terminam o processo sem diálogo (mesma lição do ADM do Windows).
    std::lock_guard<std::mutex> threadLock(m_threadMutex);
    if (m_playoutRunning.exchange(true)) return 0;
    if (m_playoutThread.joinable()) m_playoutThread.join();
    m_playoutThread = std::thread([this] { playoutLoop(); });
    return 0;
}

int32_t LinuxLoopbackAudioDeviceModule::StopPlayout()
{
    std::lock_guard<std::mutex> threadLock(m_threadMutex);
    m_playoutRunning.store(false);
    if (m_playoutThread.joinable()) m_playoutThread.join();
    return 0;
}

bool LinuxLoopbackAudioDeviceModule::Playing() const
{
    return m_playoutRunning.load();
}

int32_t LinuxLoopbackAudioDeviceModule::StartRecording()
{
    std::lock_guard<std::mutex> threadLock(m_threadMutex);
    if (m_running.exchange(true)) return 0;
    if (m_thread.joinable()) m_thread.join();
    m_thread = std::thread([this] { captureLoop(); });
    return 0;
}

int32_t LinuxLoopbackAudioDeviceModule::StopRecording()
{
    std::lock_guard<std::mutex> threadLock(m_threadMutex);
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
    return 0;
}

bool LinuxLoopbackAudioDeviceModule::Recording() const
{
    return m_running.load();
}

int LinuxLoopbackAudioDeviceModule::RestartPlayoutInternally()
{
    StopPlayout();
    return StartPlayout();
}

int LinuxLoopbackAudioDeviceModule::RestartRecordingInternally()
{
    StopRecording();
    return StartRecording();
}

int LinuxLoopbackAudioDeviceModule::SetPlayoutSampleRate(uint32_t)
{
    return 0;
}

int LinuxLoopbackAudioDeviceModule::SetRecordingSampleRate(uint32_t sample_rate)
{
    m_forcedSampleRate = sample_rate;
    return 0;
}

void LinuxLoopbackAudioDeviceModule::pushSamples(const int16_t* samples,
                                                 size_t count, uint32_t sampleRate)
{
    if (!samples || count == 0) return;
    m_pcm.insert(m_pcm.end(), samples, samples + count);
    const size_t frameSamples = std::max<size_t>(1, sampleRate / 100); // 10 ms mono
    webrtc::AudioTransport* cb = m_audioCallback.load(std::memory_order_acquire);
    while (cb && m_pcm.size() >= frameSamples) {
        uint32_t newMicLevel = 0;
        cb->RecordedDataIsAvailable(m_pcm.data(), frameSamples, 2, 1, sampleRate,
                                    0, 0, 0, false, newMicLevel);
        m_pcm.erase(m_pcm.begin(), m_pcm.begin() + ptrdiff_t(frameSamples));
        cb = m_audioCallback.load(std::memory_order_acquire);
    }
    if (m_pcm.size() > sampleRate) m_pcm.clear(); // servidor travou: descarta
}

void LinuxLoopbackAudioDeviceModule::playoutLoop()
{
    // Idêntico ao ADM de loopback do Windows: puxa o decodificador do
    // libwebrtc a cada 10 ms (NeedMorePlayData) para que os sinks de áudio
    // remoto recebam PCM; a saída é descartada (o mixer Qt do Halla toca o
    // áudio de verdade).
    constexpr uint32_t kSampleRate = 48000;
    constexpr size_t kChannels = 1;
    constexpr size_t kFrames = kSampleRate / 100; // 10 ms
    std::vector<int16_t> discarded(kFrames * kChannels);
    auto deadline = std::chrono::steady_clock::now();
    while (m_playoutRunning.load()) {
        if (webrtc::AudioTransport* cb =
                m_audioCallback.load(std::memory_order_acquire)) {
            size_t framesOut = 0;
            int64_t elapsedTimeMs = 0;
            int64_t ntpTimeMs = 0;
            cb->NeedMorePlayData(kFrames, sizeof(int16_t), kChannels,
                                 kSampleRate, discarded.data(), framesOut,
                                 &elapsedTimeMs, &ntpTimeMs);
        }
        deadline += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(deadline);
        const auto now = std::chrono::steady_clock::now();
        if (deadline < now - std::chrono::milliseconds(100)) deadline = now;
    }
}

void LinuxLoopbackAudioDeviceModule::moveSinkInput(PulseExclusion* excl,
                                                   uint32_t inputIndex)
{
    if (excl->isoSinkIndex == PA_INVALID_INDEX) return;
    // Já está no sink isolado (evento duplicado): ignora.
    if (std::find(excl->movedInputs.begin(), excl->movedInputs.end(), inputIndex)
            != excl->movedInputs.end())
        return;
    excl->pendingMoves++;
    pa_operation* op = pa_context_move_sink_input_by_index(
        excl->ctx, inputIndex, excl->isoSinkIndex,
        [](pa_context*, int success, void* userdata) {
            auto* excl = static_cast<PulseExclusion*>(userdata);
            if (excl->pendingMoves > 0) excl->pendingMoves--;
            if (excl->pendingMoves == 0 && excl->moveWait)
                excl->moveWait->signal();
            if (!success)
                AppLog::warn(QStringLiteral(
                    "Áudio do PC: não foi possível isolar um stream do Halla "
                    "(eco possível nesta transmissão)"));
        },
        excl);
    if (op) {
        excl->movedInputs.push_back(inputIndex);
        pa_operation_unref(op);
    } else if (excl->pendingMoves > 0) {
        excl->pendingMoves--;
        if (excl->pendingMoves == 0 && excl->moveWait) excl->moveWait->signal();
    }
}

// ---------------------------------------------------------------------------
// Captura: exclusão do Halla (sink virtual) + monitor do sink default
// ---------------------------------------------------------------------------

void LinuxLoopbackAudioDeviceModule::captureLoop()
{
    const uint32_t sampleRate = m_forcedSampleRate ? m_forcedSampleRate : 48000;
    m_exclusion = std::make_unique<PulseExclusion>();
    QByteArray monitorSource = "@DEFAULT_MONITOR@";

    // ---- Passo 1: conectar e descobrir o sink default (nomeia o monitor e
    // permite devolver os sink-inputs no fim). Falha não é fatal: cai no
    // macro @DEFAULT_MONITOR@ sem exclusão.
    if (pa_threaded_mainloop* ml = pa_threaded_mainloop_new()) {
        m_exclusion->ml = ml;
        pa_mainloop_api* api = pa_threaded_mainloop_get_api(ml);
        m_exclusion->ctx = pa_context_new(api, "halla-screenshare");
        if (m_exclusion->ctx
                && pa_threaded_mainloop_start(ml) >= 0) {
            m_exclusion->started = true;
            pa_threaded_mainloop_lock(ml);
            auto unlock = qScopeGuard([&ml] { pa_threaded_mainloop_unlock(ml); });

            struct ConnectWait {
                LinuxLoopbackAudioDeviceModule* self;
                OpWait wait;
            };
            ConnectWait connect{this, {ml}};
            pa_context_set_state_callback(
                m_exclusion->ctx,
                [](pa_context* c, void* userdata) {
                    auto* state = static_cast<ConnectWait*>(userdata);
                    switch (pa_context_get_state(c)) {
                    case PA_CONTEXT_READY:
                        state->self->m_exclusion->connected = true;
                        state->wait.signal();
                        break;
                    case PA_CONTEXT_FAILED:
                    case PA_CONTEXT_TERMINATED:
                        state->wait.signal();
                        break;
                    default: break;
                    }
                },
                &connect);
            if (pa_context_connect(m_exclusion->ctx, nullptr, PA_CONTEXT_NOFLAGS,
                                   nullptr) >= 0) {
                connect.wait.await();
            }

            if (m_exclusion->connected) {
                AppLog::info(QStringLiteral(
                    "PulseAudio conectado para o áudio da transmissão"));
                struct ServerWait {
                    PulseExclusion* excl;
                    OpWait wait;
                };
                ServerWait server{m_exclusion.get(), {ml}};
                pa_operation* op = pa_context_get_server_info(
                    m_exclusion->ctx,
                    [](pa_context*, const pa_server_info* info, void* userdata) {
                        auto* state = static_cast<ServerWait*>(userdata);
                        if (info && info->default_sink_name)
                            state->excl->defaultSink = info->default_sink_name;
                        state->wait.signal();
                    },
                    &server);
                if (op) {
                    pa_operation_unref(op);
                    server.wait.await();
                }
            }
        }
    }

    const bool haveDefaultSink =
        m_exclusion->connected && !m_exclusion->defaultSink.empty();
    if (!m_exclusion->connected) {
        AppLog::warn(QStringLiteral(
            "Áudio do PC sem exclusão do Halla: PulseAudio inacessível "
            "(capturando o monitor default; sons do próprio Halla entram na "
            "transmissão)"));
    }

    // ---- Passo 2 (exclusão): sink virtual + loopback de retorno + moves.
    bool excluded = false;
    if (haveDefaultSink) {
        pa_threaded_mainloop* ml = m_exclusion->ml;
        pa_context* ctx = m_exclusion->ctx;
        const std::string isoName = isoSinkName();
        const std::string monitorOfIso = isoName + ".monitor";

        struct StepWait {
            LinuxLoopbackAudioDeviceModule::PulseExclusion* excl = nullptr;
            bool ok = false;
            uint32_t index = PA_INVALID_INDEX;
            OpWait wait;
        };

        pa_threaded_mainloop_lock(ml);
        auto unlock = qScopeGuard([&ml] { pa_threaded_mainloop_unlock(ml); });

        auto unloadNow = [&](uint32_t moduleIndex) {
            if (moduleIndex == PA_INVALID_INDEX) return;
            struct UnloadWait { OpWait wait; };
            UnloadWait unload{{ml}};
            pa_operation* op = pa_context_unload_module(
                ctx, moduleIndex,
                [](pa_context*, int, void* userdata) {
                    static_cast<UnloadWait*>(userdata)->wait.signal();
                },
                &unload);
            if (op) {
                pa_operation_unref(op);
                unload.wait.await();
            }
        };

        // 2a. sink virtual "halla-iso-<pid>"
        StepWait nullSink{m_exclusion.get(), false, PA_INVALID_INDEX, {ml}};
        {
            const std::string args = "sink_name=" + isoName
                + " sink_properties=device.description='Halla (isolado)'";
            pa_operation* op = pa_context_load_module(
                ctx, "module-null-sink", args.c_str(),
                [](pa_context*, uint32_t index, void* userdata) {
                    auto* state = static_cast<StepWait*>(userdata);
                    state->ok = index != PA_INVALID_INDEX;
                    state->index = index;
                    state->wait.signal();
                },
                &nullSink);
            if (!op) nullSink.wait.signal();
            else pa_operation_unref(op);
            nullSink.wait.await();
        }

        if (!nullSink.ok) {
            AppLog::warn(QStringLiteral(
                "Áudio do PC SEM exclusão do Halla (este servidor de som não "
                "permite carregar módulos): sons do próprio Halla entram na "
                "transmissão — prefira 'Sem áudio do PC' se ouvir eco"));
        } else {
            m_exclusion->nullSinkModule = nullSink.index;
            m_exclusion->modulesLoaded = true;

            // 2b. devolver o áudio do sink virtual ao alto-falante do
            // usuário. Sem este retorno o Halla ficaria MUDO enquanto
            // transmite — se falhar, desfaz a exclusão inteira.
            StepWait loopback{m_exclusion.get(), false, PA_INVALID_INDEX, {ml}};
            {
                const std::string args = "source=" + monitorOfIso
                    + " sink=" + m_exclusion->defaultSink;
                pa_operation* op = pa_context_load_module(
                    ctx, "module-loopback", args.c_str(),
                    [](pa_context*, uint32_t index, void* userdata) {
                        auto* state = static_cast<StepWait*>(userdata);
                        state->ok = index != PA_INVALID_INDEX;
                        state->index = index;
                        state->wait.signal();
                    },
                    &loopback);
                if (!op) loopback.wait.signal();
                else pa_operation_unref(op);
                loopback.wait.await();
            }
            if (!loopback.ok) {
                AppLog::warn(QStringLiteral(
                    "Áudio do PC SEM exclusão do Halla (module-loopback "
                    "indisponível): sons do próprio Halla entram na transmissão"));
                unloadNow(m_exclusion->nullSinkModule);
                m_exclusion->nullSinkModule = PA_INVALID_INDEX;
                m_exclusion->modulesLoaded = false;
            } else {
                m_exclusion->loopbackModule = loopback.index;

                // 2c. index do sink virtual (alvo dos moves)
                StepWait sinkInfo{m_exclusion.get(), false, PA_INVALID_INDEX, {ml}};
                pa_operation* op = pa_context_get_sink_info_by_name(
                    ctx, isoName.c_str(),
                    [](pa_context*, const pa_sink_info* info, int eol,
                       void* userdata) {
                        auto* state = static_cast<StepWait*>(userdata);
                        if (info && eol == 0) {
                            state->ok = true;
                            state->index = info->index;
                        }
                        if (eol) state->wait.signal();
                    },
                    &sinkInfo);
                if (!op) sinkInfo.wait.signal();
                else pa_operation_unref(op);
                sinkInfo.wait.await();
                m_exclusion->isoSinkIndex =
                    sinkInfo.ok ? sinkInfo.index : PA_INVALID_INDEX;

                if (m_exclusion->isoSinkIndex == PA_INVALID_INDEX) {
                    AppLog::warn(QStringLiteral(
                        "Áudio do PC SEM exclusão do Halla (sink virtual não "
                        "respondeu): sons do próprio Halla entram na transmissão"));
                } else {
                    // 2d. mover os sink-inputs ATUAIS do Halla
                    StepWait list{m_exclusion.get(), false, PA_INVALID_INDEX, {ml}};
                    op = pa_context_get_sink_input_info_list(
                        ctx,
                        [](pa_context*, const pa_sink_input_info* info, int eol,
                           void* userdata) {
                            auto* state = static_cast<StepWait*>(userdata);
                            if (info && eol == 0 && isOwnProcess(info->proplist))
                                LinuxLoopbackAudioDeviceModule::moveSinkInput(
                                    state->excl, info->index);
                            if (eol) state->wait.signal();
                        },
                        &list);
                    if (!op) list.wait.signal();
                    else pa_operation_unref(op);
                    list.wait.await();

                    // 2e. assinar sink-inputs NOVOS do Halla (entrar em canal
                    // depois de iniciar a transmissão)
                    struct SubscribeState {
                        PulseExclusion* excl;
                        OpWait wait;
                    };
                    auto* subscribeState =
                        new SubscribeState{m_exclusion.get(), {ml}};
                    pa_context_set_subscribe_callback(
                        ctx,
                        [](pa_context*, pa_subscription_event_type_t type,
                           uint32_t index, void* userdata) {
                            auto* state = static_cast<SubscribeState*>(userdata);
                            if ((type & PA_SUBSCRIPTION_EVENT_TYPE_MASK)
                                    != PA_SUBSCRIPTION_EVENT_NEW)
                                return;
                            if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK)
                                    != PA_SUBSCRIPTION_EVENT_SINK_INPUT)
                                return;
                            // Consulta assíncrona: só move se for do nosso
                            // processo (e ainda não estiver no sink virtual).
                            struct InfoWait {
                                PulseExclusion* excl;
                                OpWait wait;
                            };
                            auto* info = new InfoWait{state->excl, {state->wait.ml}};
                            pa_operation* op = pa_context_get_sink_input_info(
                                state->excl->ctx, index,
                                [](pa_context*, const pa_sink_input_info* info,
                                   int eol, void* userdata) {
                                    auto* state = static_cast<InfoWait*>(userdata);
                                    if (info && eol == 0
                                            && isOwnProcess(info->proplist)
                                            && info->sink != state->excl->isoSinkIndex) {
                                        LinuxLoopbackAudioDeviceModule::moveSinkInput(
                                            state->excl, info->index);
                                    }
                                    if (eol) {
                                        state->wait.signal();
                                        delete state;
                                    }
                                },
                                info);
                            if (op) pa_operation_unref(op);
                            else delete info;
                        },
                        subscribeState);
                    op = pa_context_subscribe(
                        ctx, PA_SUBSCRIPTION_MASK_SINK_INPUT,
                        [](pa_context*, int, void* userdata) {
                            auto* state = static_cast<SubscribeState*>(userdata);
                            state->wait.signal();
                            delete state;
                        },
                        subscribeState);
                    if (op) pa_operation_unref(op);
                    else delete subscribeState;

                    excluded = true;
                    monitorSource =
                        QByteArray(m_exclusion->defaultSink.c_str()) + ".monitor";
                    AppLog::info(QStringLiteral(
                        "Áudio do PC com exclusão do Halla: sink virtual %1 ativo, "
                        "capturando o monitor de %2")
                        .arg(QString::fromStdString(isoName),
                             QString::fromStdString(m_exclusion->defaultSink)));
                }
            }
        }
        if (!excluded && haveDefaultSink) {
            monitorSource =
                QByteArray(m_exclusion->defaultSink.c_str()) + ".monitor";
        }
    }

    // ---- Passo 3: gravar do monitor com pa_simple (a leitura bloqueante
    // marca o ritmo de 10 ms; o servidor resampleia para a taxa pedida).
    const int frameBytes = int(sampleRate / 100) * 2 /*channels*/ * 2 /*s16*/;
    // static_cast evita tanto o vexing parse (parênteses com expr simples)
    // quanto a initializer_list (braces com size_t = 1 elemento).
    std::vector<char> buffer(static_cast<size_t>(frameBytes));
    std::vector<int16_t> mono(static_cast<size_t>(frameBytes) / 4 + 1);
    int error = 0;
    pa_sample_spec spec{};
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = sampleRate;
    spec.channels = 2;
    pa_buffer_attr attr{};
    attr.maxlength = uint32_t(frameBytes * 4);
    attr.fragsize = uint32_t(frameBytes);
    // Assinatura real: (server, name, dir, dev, STREAM_NAME, ss, map, attr, error)
    // — 9 parâmetros; o stream_name é obrigatório e distinto do nome do client.
    pa_simple* record = pa_simple_new(nullptr, "halla-screenshare",
                                      PA_STREAM_RECORD, monitorSource.constData(),
                                      "halla-pc-audio",
                                      &spec, nullptr, &attr, &error);
    if (!record) {
        AppLog::warn(QStringLiteral("Áudio da transmissão indisponível: %1")
                         .arg(QString::fromLatin1(pa_strerror(error))));
        m_running = false;
        return;
    }

    AppLog::info(QStringLiteral(
        "Loopback PulseAudio ativo: todo o som do PC (%1); %2 Hz")
        .arg(excluded ? QStringLiteral("Halla excluído")
                      : QStringLiteral("SEM exclusão do Halla"))
        .arg(sampleRate));

    while (m_running.load()) {
        if (pa_simple_read(record, buffer.data(), size_t(frameBytes), &error) < 0) {
            AppLog::warn(QStringLiteral("Loopback PulseAudio encerrado: %1")
                             .arg(QString::fromLatin1(pa_strerror(error))));
            break;
        }
        const size_t frames = size_t(frameBytes) / 4;
        const int16_t* input = reinterpret_cast<const int16_t*>(buffer.data());
        for (size_t i = 0; i < frames; ++i) {
            const int mixed = int(input[i * 2]) + int(input[i * 2 + 1]);
            mono[i] = int16_t(mixed / 2);
        }
        if (frames) pushSamples(mono.data(), frames, sampleRate);
    }

    pa_simple_free(record);

    // ---- Passo 4: desfazer a exclusão (mover os sink-inputs de volta ANTES
    // de descarregar os módulos, senão o áudio do Halla morre junto com o
    // sink virtual).
    if (m_exclusion->modulesLoaded && m_exclusion->ml && m_exclusion->ctx
            && !m_exclusion->defaultSink.empty()) {
        pa_threaded_mainloop* ml = m_exclusion->ml;
        pa_context* ctx = m_exclusion->ctx;
        pa_threaded_mainloop_lock(ml);
        auto unlock = qScopeGuard([&ml] { pa_threaded_mainloop_unlock(ml); });

        struct TeardownWait {
            OpWait wait;
        };
        TeardownWait teardown{{ml}};
        m_exclusion->moveWait = &teardown.wait;
        auto clearMoveWait = qScopeGuard([this] { m_exclusion->moveWait = nullptr; });
        // Espera os moves do SETUP terminarem antes de mover de volta: com
        // lock held os callbacks não rodam concorrentemente com este código,
        // e o reset de `done` abaixo é seguro. Sem isto, um move-para-iso
        // ainda em voo podia chegar ao servidor DEPOIS do move-de-volta e
        // deixar o stream do Halla num sink que está sendo descarregado.
        if (m_exclusion->pendingMoves > 0) teardown.wait.await();
        teardown.wait.done = false;
        for (uint32_t input : m_exclusion->movedInputs) {
            pa_operation* op = pa_context_move_sink_input_by_name(
                ctx, input, m_exclusion->defaultSink.c_str(),
                [](pa_context*, int, void* userdata) {
                    auto* excl = static_cast<PulseExclusion*>(userdata);
                    if (excl->pendingMoves > 0) excl->pendingMoves--;
                    if (excl->pendingMoves == 0 && excl->moveWait)
                        excl->moveWait->signal();
                },
                m_exclusion.get());
            if (op) pa_operation_unref(op);
            else {
                if (m_exclusion->pendingMoves > 0) m_exclusion->pendingMoves--;
                if (m_exclusion->pendingMoves == 0) teardown.wait.signal();
            }
        }
        if (m_exclusion->pendingMoves > 0) teardown.wait.await();

        auto unloadNow = [&](uint32_t moduleIndex) {
            if (moduleIndex == PA_INVALID_INDEX) return;
            struct UnloadWait { OpWait wait; };
            UnloadWait unload{{ml}};
            pa_operation* op = pa_context_unload_module(
                ctx, moduleIndex,
                [](pa_context*, int, void* userdata) {
                    static_cast<UnloadWait*>(userdata)->wait.signal();
                },
                &unload);
            if (op) {
                pa_operation_unref(op);
                unload.wait.await();
            }
        };
        // Primeiro o loopback (deixa de puxar do monitor), depois o sink.
        unloadNow(m_exclusion->loopbackModule);
        unloadNow(m_exclusion->nullSinkModule);
    }
}

#endif
