#include "halla_plugin_api.h"

#include <string>

static const HallaHostApi* g_host = nullptr;

static int initialize(const HallaHostApi* host) {
    if (!host || host->abi_version != HALLA_PLUGIN_ABI_VERSION) return 0;
    g_host = host;
    g_host->log(g_host->context, HALLA_PLUGIN_LOG_INFO,
                "Hello World inicializado");
    g_host->request_client_state(g_host->context);
    return 1;
}

static void shutdownPlugin() {
    if (g_host)
        g_host->log(g_host->context, HALLA_PLUGIN_LOG_INFO,
                    "Hello World encerrado");
    g_host = nullptr;
}

static void onEvent(const char* json, size_t size) {
    if (!g_host || !json) return;
    const std::string event(json, size);
    if (event.find("\"talking\":true") != std::string::npos)
        g_host->log(g_host->context, HALLA_PLUGIN_LOG_INFO,
                    "Alguém começou a falar");
}

static void onSettings(const char*, size_t) {}

static const HallaPluginApi kPlugin = {
    HALLA_PLUGIN_ABI_VERSION,
    sizeof(HallaPluginApi),
    "com.grouphalla.hello-world",
    "Hello World",
    "1.0.0",
    "Comunidade Halla",
    "Plugin mínimo de exemplo para a API nativa do Halla.",
    &initialize,
    &shutdownPlugin,
    &onEvent,
    &onSettings
};

extern "C" HALLA_PLUGIN_EXPORT
const HallaPluginApi* halla_plugin_entry(void) {
    return &kPlugin;
}
