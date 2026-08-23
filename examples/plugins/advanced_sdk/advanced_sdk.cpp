#include "halla_plugin_api.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

const HallaHostApi* g_host = nullptr;
const HallaConnectionApiV1* g_connections = nullptr;
const HallaAudioApiV1* g_audio = nullptr;
const HallaDataApiV1* g_data = nullptr;
const HallaUiApiV1* g_ui = nullptr;
uint64_t g_processedFrames = 0;

void logMessage(const std::string& message) {
    if (g_host && g_host->log)
        g_host->log(g_host->context, HALLA_PLUGIN_LOG_INFO, message.c_str());
}

void processAudio(void*, HallaAudioFrame* frame) {
    if (!frame || !frame->samples) return;
    // Exemplo deliberadamente transparente: prova que o callback PCM está
    // ligado ao pipeline sem alterar a voz do usuário.
    g_processedFrames += frame->frame_count;
}

void receiveData(void*, uint64_t connectionId, int32_t senderId,
                 const char* topic, const uint8_t*, size_t size) {
    logMessage("Dados do complemento recebidos: conexão="
        + std::to_string(connectionId) + " remetente=" + std::to_string(senderId)
        + " tópico=" + (topic ? topic : "") + " bytes=" + std::to_string(size));
}

void inspectConnections(void*, const char*) {
    if (!g_connections || !g_connections->get_connections_json) return;
    const size_t required = g_connections->get_connections_json(
        g_connections->context, nullptr, 0);
    std::vector<char> json(required ? required : 1);
    g_connections->get_connections_json(g_connections->context,
                                         json.data(), json.size());
    logMessage(std::string("Snapshot avançado: ") + json.data());
    if (g_ui && g_ui->show_notification) {
        g_ui->show_notification(g_ui->context, "Advanced SDK Example",
            "O snapshot de conexões foi escrito no registro do Halla.", 5000);
    }
    if (g_audio && g_audio->play_pcm
            && g_audio->struct_size >= offsetof(HallaAudioApiV1, play_pcm)
                + sizeof(g_audio->play_pcm)) {
        std::vector<int16_t> tone(4800); // 100 ms, 880 Hz
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = int16_t(std::sin(double(i) * 2.0 * 3.141592653589793
                                      * 880.0 / 48000.0) * 5000.0);
        g_audio->play_pcm(g_audio->context, 0, tone.data(),
                          uint32_t(tone.size()), 1, 1.0f);
    }
}

int initialize(const HallaHostApi* host) {
    if (!host || host->abi_version != HALLA_PLUGIN_ABI_VERSION
            || host->struct_size < HALLA_HOST_API_BASE_SIZE) return 0;
    g_host = host;
    const size_t queryEnd = offsetof(HallaHostApi, query_interface)
        + sizeof(host->query_interface);
    if (host->struct_size < queryEnd || !host->query_interface) {
        logMessage("O host oferece apenas a API-base legada.");
        return 1;
    }

    g_connections = static_cast<const HallaConnectionApiV1*>(host->query_interface(
        host->context, HALLA_INTERFACE_CONNECTION_V1, 1));
    g_audio = static_cast<const HallaAudioApiV1*>(host->query_interface(
        host->context, HALLA_INTERFACE_AUDIO_V1, 1));
    g_data = static_cast<const HallaDataApiV1*>(host->query_interface(
        host->context, HALLA_INTERFACE_DATA_V1, 1));
    g_ui = static_cast<const HallaUiApiV1*>(host->query_interface(
        host->context, HALLA_INTERFACE_UI_V1, 1));

    if (g_audio && g_audio->register_processor)
        g_audio->register_processor(g_audio->context, nullptr, &processAudio,
                                    HALLA_AUDIO_REMOTE_BEFORE_SPATIAL);
    if (g_data && g_data->set_receive_handler)
        g_data->set_receive_handler(g_data->context, nullptr, &receiveData);
    if (g_ui && g_ui->register_hotkey)
        g_ui->register_hotkey(g_ui->context, "inspect-connections",
            "Inspecionar conexões do SDK", "Ctrl+Alt+H", nullptr,
            &inspectConnections);

    logMessage("Interfaces avançadas inicializadas.");
    return 1;
}

void shutdown() {
    if (g_audio && g_audio->unregister_processor)
        g_audio->unregister_processor(g_audio->context);
    if (g_ui && g_ui->unregister_action)
        g_ui->unregister_action(g_ui->context, "inspect-connections");
    logMessage("Advanced SDK finalizado após "
               + std::to_string(g_processedFrames) + " frames.");
    g_host = nullptr;
    g_connections = nullptr;
    g_audio = nullptr;
    g_data = nullptr;
    g_ui = nullptr;
}

void onEvent(const char*, size_t) {}
void onSettingsChanged(const char*, size_t) {}

const HallaPluginApi kPlugin{
    HALLA_PLUGIN_ABI_VERSION,
    sizeof(HallaPluginApi),
    "community.advanced-sdk-example",
    "Advanced SDK Example",
    "1.0.0",
    "Halla-DEV",
    "Exemplo das interfaces modulares avançadas.",
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
