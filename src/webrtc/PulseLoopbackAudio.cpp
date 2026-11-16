#include "PulseLoopbackAudio.h"

#if defined(HALLA_WEBRTC_NATIVE) && defined(Q_OS_LINUX)

#include "core/AppLog.h"

#include <QByteArray>
#include <QScopeGuard>
#include <QString>

// pa_simple_* (blocking monitor recording in setup and in the loopback
// fallback) lives in pulse/simple.h, which pulseaudio.h does NOT pull in.
#include <pulse/simple.h>
#include <pulse/pulseaudio.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Name of the virtual sink isolating Halla (unique per process: two app
// instances do not collide and orphaned modules from a crash are
// identifiable).
std::string isoSinkName()
{
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "halla-iso-%ld", static_cast<long>(::getpid()));
    return std::string(buffer);
}

// Pulse operation synchronization (threaded-mainloop pattern): each
// callback runs on the mainloop thread WITH the lock held; signal() wakes
// whoever waits in await() (which releases the lock while sleeping and
// re-checks the condition -- no lost wakeup).
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

// Exclusion infrastructure: dedicated mainloop + loaded modules + list of
// Halla sink-inputs moved to the virtual sink. The destructor runs on the
// thread destroying the ADM (after the captureLoop join -- never from inside
// the mainloop, which would deadlock in pa_threaded_mainloop_stop).
struct LinuxLoopbackAudioDeviceModule::PulseExclusion {
    pa_threaded_mainloop* ml = nullptr;
    pa_context* ctx = nullptr;
    bool started = false; // stop() on a never-started mainloop is unsafe
    std::string defaultSink;
    bool connected = false;
    bool modulesLoaded = false;
    uint32_t nullSinkModule = PA_INVALID_INDEX;
    uint32_t loopbackModule = PA_INVALID_INDEX;
    uint32_t isoSinkIndex = PA_INVALID_INDEX;
    std::vector<uint32_t> movedInputs;
    int pendingMoves = 0;
    // When non-null, the last move to complete signals this OpWait -- both
    // setup and teardown moves decrement the SAME counter; without this, if
    // the last to complete were a setup move, the teardown await() would wait
    // forever (deadlock).
    OpWait* moveWait = nullptr;

    ~PulseExclusion()
    {
        if (!ml) return;
        if (ctx) {
            // Detach callbacks first: a late mainloop event must never fire
            // into freed state during teardown.
            pa_context_set_state_callback(ctx, nullptr, nullptr);
            pa_context_set_subscribe_callback(ctx, nullptr, nullptr);
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
// ADM lifecycle
// ---------------------------------------------------------------------------

// Constructor/destructor out-of-line on purpose: the ADM is born via
// make_ref_counted<>, whose RefCountedObject<T>::RefCountedObject() is a
// header-only SDK template -- with an implicit constructor GCC instantiates,
// in the calling TU, the exception-cleanup path that destroys the
// unique_ptr<PulseExclusion> (sizeof of an incomplete type there). Out of
// line, the instantiation stays only in this .cpp, where PulseExclusion is
// complete.
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
    // Start/Stop can arrive from different threads (GUI x libwebrtc
    // signaling): two concurrent joins on the same std::thread are UB and
    // terminate the process without a word (same lesson as the Windows ADM).
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
    if (m_pcm.size() > sampleRate) m_pcm.clear(); // server stalled: discard
}

void LinuxLoopbackAudioDeviceModule::playoutLoop()
{
    // Identical to the Windows loopback ADM: pulls the libwebrtc decoder
    // every 10 ms (NeedMorePlayData) so remote audio sinks receive PCM; the
    // output is discarded (the Halla Qt mixer actually plays the audio).
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
    // Already on the isolated sink (duplicate event): ignore.
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
// Capture: Halla exclusion (virtual sink) + default sink monitor
// ---------------------------------------------------------------------------

void LinuxLoopbackAudioDeviceModule::captureLoop()
{
    const uint32_t sampleRate = m_forcedSampleRate ? m_forcedSampleRate : 48000;
    m_exclusion = std::make_unique<PulseExclusion>();
    QByteArray monitorSource = "@DEFAULT_MONITOR@";

    // ---- Step 1: connect and discover the default sink (names the monitor
    // and allows returning the sink-inputs at the end). Failure is not fatal:
    // falls back to the @DEFAULT_MONITOR@ macro without exclusion.
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

    // ---- Step 2 (exclusion): virtual sink + return loopback + moves.
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

        // 2a. virtual sink "halla-iso-<pid>"
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

            // 2b. return the virtual sink audio to the user's speaker.
            // Without this return Halla would stay MUTED while streaming --
            // if it fails, undo the whole exclusion.
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

                // 2c. virtual sink index (move target)
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
                    // Roll back the partially loaded modules: leaving the
                    // loopback + null sink behind would keep Halla streams
                    // routed to a sink nobody unloads.
                    unloadNow(m_exclusion->loopbackModule);
                    unloadNow(m_exclusion->nullSinkModule);
                    m_exclusion->loopbackModule = PA_INVALID_INDEX;
                    m_exclusion->nullSinkModule = PA_INVALID_INDEX;
                    m_exclusion->modulesLoaded = false;
                } else {
                    // 2d. move Halla's CURRENT sink-inputs
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

                    // 2e. subscribe to NEW Halla sink-inputs (joining a
                    // channel after starting the stream)
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
                            // Async query: only move if it belongs to our
                            // process (and is not already on the virtual sink).
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

    // ---- Step 3: record from the monitor with pa_simple (the blocking read
    // sets the 10 ms pace; the server resamples to the requested rate).
    const int frameBytes = int(sampleRate / 100) * 2 /*channels*/ * 2 /*s16*/;
    // static_cast avoids both the vexing parse (parentheses with a simple
    // expression) and initializer_list (braces with size_t = 1 element).
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
    // Real signature: (server, name, dir, dev, STREAM_NAME, ss, map, attr, error)
    // -- 9 parameters; stream_name is mandatory and distinct from the client name.
    pa_simple* record = pa_simple_new(nullptr, "halla-screenshare",
                                      PA_STREAM_RECORD, monitorSource.constData(),
                                      "halla-pc-audio",
                                      &spec, nullptr, &attr, &error);
    if (!record) {
        AppLog::warn(QStringLiteral("Áudio da transmissão indisponível: %1")
                         .arg(QString::fromLatin1(pa_strerror(error))));
        m_running = false;
        // Fall through to the teardown below: returning here would leak the
        // loaded modules and leave Halla streams on the virtual sink.
    } else {
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
    }

    // ---- Step 4: undo the exclusion (move the sink-inputs back BEFORE
    // unloading the modules, otherwise Halla audio dies together with the
    // virtual sink).
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
        // Wait for the SETUP moves to finish before moving back: with the
        // lock held the callbacks do not run concurrently with this code, and
        // the `done` reset below is safe. Without this, an in-flight
        // move-to-iso could reach the server AFTER the move-back and leave
        // the Halla stream on a sink being unloaded.
        if (m_exclusion->pendingMoves > 0) teardown.wait.await();
        teardown.wait.done = false;
        for (uint32_t input : m_exclusion->movedInputs) {
            m_exclusion->pendingMoves++;
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
        // First the loopback (stops pulling from the monitor), then the sink.
        unloadNow(m_exclusion->loopbackModule);
        unloadNow(m_exclusion->nullSinkModule);
    }
}

#endif
