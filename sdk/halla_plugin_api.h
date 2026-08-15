#ifndef HALLA_PLUGIN_API_H
#define HALLA_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HALLA_PLUGIN_ABI_VERSION 1u
#define HALLA_PLUGIN_ENTRY_SYMBOL "halla_plugin_entry"

#if defined(_WIN32)
#  define HALLA_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define HALLA_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

typedef enum HallaPluginLogLevel {
    HALLA_PLUGIN_LOG_DEBUG = 0,
    HALLA_PLUGIN_LOG_INFO = 1,
    HALLA_PLUGIN_LOG_WARNING = 2,
    HALLA_PLUGIN_LOG_ERROR = 3
} HallaPluginLogLevel;

/*
 * Funções oferecidas pelo Halla ao plugin. Todos os ponteiros são válidos
 * somente entre initialize() e shutdown(). As strings são UTF-8.
 */
typedef struct HallaHostApi {
    uint32_t abi_version;
    uint32_t struct_size;
    void* context;

    void (*log)(void* context, HallaPluginLogLevel level,
                const char* utf8_message);

    /* Retorna o tamanho necessário (incluindo NUL). buffer pode ser NULL. */
    size_t (*get_settings_json)(void* context, char* buffer,
                                size_t buffer_size);

    /* Solicita que o Halla envie novamente o evento client_state atual. */
    void (*request_client_state)(void* context);
} HallaHostApi;

/*
 * Tabela exportada pelo plugin. O plugin continua dono das strings e da
 * estrutura durante todo o período em que a DLL estiver carregada.
 */
typedef struct HallaPluginApi {
    uint32_t abi_version;
    uint32_t struct_size;

    const char* id;
    const char* name;
    const char* version;
    const char* author;
    const char* description;

    /* Retorne 1 em caso de sucesso e 0 em caso de falha. */
    int (*initialize)(const HallaHostApi* host);
    void (*shutdown)(void);

    /* JSON UTF-8. Eventos atuais: client_state e application_shutdown. */
    void (*on_event)(const char* utf8_json, size_t json_size);

    /* Chamado depois que o usuário salva a configuração do complemento. */
    void (*on_settings_changed)(const char* utf8_json, size_t json_size);
} HallaPluginApi;

typedef const HallaPluginApi* (*HallaPluginEntryFn)(void);

/*
 * Um plugin implementa e exporta `const HallaPluginApi*
 * halla_plugin_entry(void)` sem name mangling C++. A declaração não é emitida
 * aqui para que o executável host não tente exportar o mesmo símbolo.
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HALLA_PLUGIN_API_H */
