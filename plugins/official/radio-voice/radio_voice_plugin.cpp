// Complemento oficial "Voz de rádio policial" — versão em pacote .halla-addon.
//
// O DSP é o mesmo do complemento embutido nos aplicativos (RadioVoiceDsp.h,
// compartilhado entre Halla Desktop e Halla Mobile por cópia fiel). Esta
// biblioteca permite distribuir atualizações do efeito pelo catálogo oficial
// (https://grouphalla.github.io/Halla-Addons/) SEM publicar uma nova versão
// dos aplicativos: o pacote instalado substitui o complemento interno.
//
// Estágios de áudio usados:
//   HALLA_AUDIO_CAPTURE_AFTER_VAD — microfone local após a decisão de
//     transmissão (o AGC do filtro não abre o detector de voz do host);
//   HALLA_AUDIO_REMOTE_BEFORE_SPATIAL — cada voz recebida antes da
//     espacialização. No Halla Mobile a captura chega como
//     HALLA_AUDIO_CAPTURE (a decisão de transmissão é tomada antes da
//     camada nativa), então os dois estágios são tratados como envio.
//
// Configurações (mesmo esquema do complemento interno):
//   sendMode/receiveMode: "none" | "whisper" | "normal" | "both"
//   intensity/noise: 0–100 (%)   gain: 50–150 (%)

#include "halla_plugin_api.h"
#include "RadioVoiceDsp.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace {

// ------------------------------------------------------------- JSON mínimo
// Parseadores tolerantes para os campos de configuração entregues pelo host
// (mesma técnica do plugin_host.cpp do Halla Mobile: sem dependências).

int jsonIntField(const std::string& json, const char* key, int fallback) {
    const std::string needle = std::string("\"") + key + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return fallback;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    const bool negative = pos < json.size() && json[pos] == '-';
    if (negative) ++pos;
    if (pos >= json.size() || json[pos] < '0' || json[pos] > '9') return fallback;
    int value = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        value = value * 10 + (json[pos] - '0');
        if (value > 1000000) return fallback;
        ++pos;
    }
    return negative ? -value : value;
}

std::string jsonStringField(const std::string& json, const char* key,
                            const std::string& fallback) {
    const std::string needle = std::string("\"") + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return fallback;
    pos += needle.size();
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) ++pos;
        out += json[pos++];
    }
    return out;
}

int clampInt(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

// ----------------------------------------------------------- configuração

enum RadioMode {
    kModeNone = 0,
    kModeWhisper = 1,
    kModeNormal = 2,
    kModeBoth = 3
};

RadioMode modeFromName(const std::string& name) {
    if (name == "whisper") return kModeWhisper;
    if (name == "normal") return kModeNormal;
    if (name == "both") return kModeBoth;
    return kModeNone;
}

// Atualizados pela thread da interface (on_settings_changed) e lidos pela
// thread de áudio — por isso armazenados como atômicos.
std::atomic<int> g_sendMode{kModeWhisper};
std::atomic<int> g_receiveMode{kModeWhisper};
std::atomic<int> g_intensity{90};
std::atomic<int> g_noise{10};
std::atomic<int> g_gain{105};

void applySettings(const std::string& json) {
    if (json.empty()) return;
    g_sendMode.store(modeFromName(jsonStringField(json, "sendMode", "whisper")));
    g_receiveMode.store(modeFromName(jsonStringField(json, "receiveMode", "whisper")));
    g_intensity.store(clampInt(jsonIntField(json, "intensity", 90), 0, 100));
    g_noise.store(clampInt(jsonIntField(json, "noise", 10), 0, 100));
    g_gain.store(clampInt(jsonIntField(json, "gain", 105), 50, 150));
}

// ------------------------------------------------------------------ DSP

struct StreamKey {
    uint64_t connection;
    int32_t user;
    uint32_t stage;
    uint32_t channel;
    bool operator<(const StreamKey& other) const {
        if (connection != other.connection) return connection < other.connection;
        if (user != other.user) return user < other.user;
        if (stage != other.stage) return stage < other.stage;
        return channel < other.channel;
    }
};

// Estado de filtro por fluxo (conexão/usuário/estágio/canal). O mutex é
// curto (busca em mapa + 20 ms de áudio) para respeitar as regras de
// tempo real da ABI: captura e recepção chegam de threads diferentes.
std::mutex g_streamsMutex;
std::map<StreamKey, RadioVoiceDsp> g_streams;

bool modeMatches(int mode, bool whisper) {
    switch (mode) {
    case kModeBoth: return true;
    case kModeWhisper: return whisper;
    case kModeNormal: return !whisper;
    default: return false;
    }
}

bool frameHasFlags(const HallaAudioFrame* frame) {
    return frame->struct_size >= offsetof(HallaAudioFrame, flags) + sizeof(frame->flags);
}

void processAudio(void*, HallaAudioFrame* frame) {
    if (!frame || !frame->samples || frame->frame_count == 0) return;
    if (frame->sample_rate != 48000 || frame->channels == 0 || frame->channels > 8) return;

    const bool capture = frame->stage == HALLA_AUDIO_CAPTURE
        || frame->stage == HALLA_AUDIO_CAPTURE_AFTER_VAD;
    const bool remote = frame->stage == HALLA_AUDIO_REMOTE_BEFORE_SPATIAL;
    if (!capture && !remote) return;

    const uint32_t flags = frameHasFlags(frame) ? frame->flags : 0u;
    const bool whisper = (flags & HALLA_AUDIO_FLAG_WHISPER) != 0;
    if (!modeMatches((capture ? g_sendMode : g_receiveMode).load(), whisper)) return;

    const float intensity = float(g_intensity.load()) / 100.0f;
    const float noise = float(g_noise.load()) / 100.0f;
    const float gain = float(g_gain.load()) / 100.0f;

    std::lock_guard<std::mutex> lock(g_streamsMutex);
    for (uint32_t channel = 0; channel < frame->channels; ++channel) {
        const StreamKey key{frame->connection_id, frame->user_id,
                            frame->stage, channel};
        RadioVoiceDsp& dsp = g_streams[key];
        if (!dsp.seeded()) {
            // Mesma semente determinística do complemento interno.
            dsp.seed(0xA341316Cu ^ uint32_t(uint32_t(frame->user_id) * 2654435761u)
                ^ uint32_t(frame->connection_id) ^ (frame->stage << 8) ^ channel);
        }
        dsp.configure(intensity, noise, gain);
        dsp.process(frame->samples + channel, frame->frame_count, frame->channels);
    }
}

// --------------------------------------------------------------- lifecycle

const HallaHostApi* g_host = nullptr;
const HallaAudioApiV1* g_audio = nullptr;

int initialize(const HallaHostApi* host) {
    if (!host || host->abi_version != HALLA_PLUGIN_ABI_VERSION
            || host->struct_size < HALLA_HOST_API_BASE_SIZE) return 0;
    g_host = host;

    if (host->get_settings_json) {
        const size_t needed = host->get_settings_json(host->context, nullptr, 0);
        if (needed > 0) {
            std::string json(needed, '\0');
            host->get_settings_json(host->context, &json[0], json.size());
            applySettings(json);
        }
    }

    const size_t queryEnd = offsetof(HallaHostApi, query_interface)
        + sizeof(host->query_interface);
    if (host->struct_size >= queryEnd && host->query_interface) {
        g_audio = static_cast<const HallaAudioApiV1*>(host->query_interface(
            host->context, HALLA_INTERFACE_AUDIO_V1, 1));
    }
    if (g_audio && g_audio->register_processor) {
        g_audio->register_processor(g_audio->context, nullptr, &processAudio,
            HALLA_AUDIO_CAPTURE_AFTER_VAD | HALLA_AUDIO_REMOTE_BEFORE_SPATIAL);
    }
    return 1;
}

void shutdown() {
    if (g_audio && g_audio->unregister_processor)
        g_audio->unregister_processor(g_audio->context);
    {
        std::lock_guard<std::mutex> lock(g_streamsMutex);
        g_streams.clear();
    }
    g_audio = nullptr;
    g_host = nullptr;
}

void onEvent(const char*, size_t) {}

void onSettingsChanged(const char* json, size_t size) {
    if (json && size) applySettings(std::string(json, size));
}

const HallaPluginApi kPlugin{
    HALLA_PLUGIN_ABI_VERSION,
    sizeof(HallaPluginApi),
    "official.radio-voice",
    "Voz de rádio policial",
    "1.1.1",
    "Halla-DEV",
    "Simula um comunicador policial no microfone e nas vozes recebidas, "
    "com regras separadas para fala normal e sussurros.",
    &initialize,
    &shutdown,
    &onEvent,
    &onSettingsChanged
};

} // namespace

extern "C" HALLA_PLUGIN_EXPORT
const HallaPluginApi* halla_plugin_entry(void) {
    return &kPlugin;
}
