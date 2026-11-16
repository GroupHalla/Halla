#ifndef HALLA_PLUGIN_API_H
#define HALLA_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The base ABI stays at 1. Advanced features are optional, versioned
 * interfaces obtained through HallaHostApi::query_interface(). This keeps
 * plugins built against SDK 1.0.63 working without recompilation.
 */
#define HALLA_PLUGIN_ABI_VERSION 1u
#define HALLA_PLUGIN_ENTRY_SYMBOL "halla_plugin_entry"

#define HALLA_INTERFACE_CORE_V1       "halla.core.v1"
#define HALLA_INTERFACE_CONNECTION_V1 "halla.connection.v1"
#define HALLA_INTERFACE_AUDIO_V1      "halla.audio.v1"
#define HALLA_INTERFACE_DATA_V1       "halla.data.v1"
#define HALLA_INTERFACE_UI_V1         "halla.ui.v1"

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

typedef enum HallaResult {
    HALLA_RESULT_OK = 0,
    HALLA_RESULT_INVALID_ARGUMENT = -1,
    HALLA_RESULT_NOT_CONNECTED = -2,
    HALLA_RESULT_NOT_FOUND = -3,
    HALLA_RESULT_PERMISSION_DENIED = -4,
    HALLA_RESULT_UNAVAILABLE = -5,
    HALLA_RESULT_LIMIT_EXCEEDED = -6,
    HALLA_RESULT_WRONG_THREAD = -7,
    HALLA_RESULT_INTERNAL_ERROR = -8
} HallaResult;

typedef struct HallaVec3 {
    float x;
    float y;
    float z;
} HallaVec3;

typedef struct HallaTransform {
    HallaVec3 position;
    HallaVec3 forward;
    HallaVec3 up;
} HallaTransform;

/* ------------------------------------------------------------------ Core */
typedef void (*HallaUiTaskFn)(void* task_context);

typedef struct HallaCoreApiV1 {
    uint32_t version;
    uint32_t struct_size;
    void* context;

    uint64_t (*monotonic_time_ms)(void* context);
    size_t (*get_application_info_json)(void* context, char* buffer,
                                        size_t buffer_size);
    int (*post_to_ui)(void* context, HallaUiTaskFn task,
                      void* task_context);
} HallaCoreApiV1;

/* ---------------------------------------------------------- Connections */
typedef enum HallaChatScope {
    HALLA_CHAT_CHANNEL = 0,
    HALLA_CHAT_SERVER = 1,
    HALLA_CHAT_PRIVATE = 2
} HallaChatScope;

typedef enum HallaSelfFlag {
    HALLA_SELF_INPUT_MUTED = 1u << 0,
    HALLA_SELF_OUTPUT_MUTED = 1u << 1,
    HALLA_SELF_AWAY = 1u << 2
} HallaSelfFlag;

typedef struct HallaConnectionApiV1 {
    uint32_t version;
    uint32_t struct_size;
    void* context;

    /* UTF-8 JSON. connection_id 0 selects the active connection. */
    size_t (*get_connections_json)(void* context, char* buffer,
                                   size_t buffer_size);
    size_t (*get_connection_json)(void* context, uint64_t connection_id,
                                  char* buffer, size_t buffer_size);

    int (*move_self)(void* context, uint64_t connection_id,
                     int32_t channel_id, const char* password_utf8);
    int (*set_self_flags)(void* context, uint64_t connection_id,
                          uint32_t mask, uint32_t values);
    int (*set_nickname)(void* context, uint64_t connection_id,
                        const char* nickname_utf8);
    int (*send_chat)(void* context, uint64_t connection_id,
                     HallaChatScope scope, int32_t target_user_id,
                     const char* text_utf8);
    int (*set_whisper_targets)(void* context, uint64_t connection_id,
                               const int32_t* user_ids, size_t user_count);
    int (*set_local_mute)(void* context, uint64_t connection_id,
                          int32_t user_id, int muted);
    int (*set_local_volume_db)(void* context, uint64_t connection_id,
                               int32_t user_id, float volume_db);

    int (*move_user)(void* context, uint64_t connection_id,
                     int32_t user_id, int32_t channel_id);
    int (*poke_user)(void* context, uint64_t connection_id,
                     int32_t user_id, const char* message_utf8);
    int (*set_commander)(void* context, uint64_t connection_id,
                         int32_t user_id, int enabled);
    int (*kick_user)(void* context, uint64_t connection_id,
                     int32_t user_id, int from_server,
                     const char* reason_utf8);
    int (*ban_user)(void* context, uint64_t connection_id,
                    int32_t user_id, uint32_t minutes,
                    const char* reason_utf8);
    /* JSON objects with the same fields documented by the Halla protocol. */
    int (*create_channel_json)(void* context, uint64_t connection_id,
                               const char* channel_json_utf8);
    int (*edit_channel_json)(void* context, uint64_t connection_id,
                             const char* channel_json_utf8);
    int (*delete_channel)(void* context, uint64_t connection_id,
                          int32_t channel_id);
} HallaConnectionApiV1;

/* ----------------------------------------------------------------- Audio */
typedef enum HallaAudioStage {
    HALLA_AUDIO_CAPTURE = 1u << 0,
    HALLA_AUDIO_REMOTE_BEFORE_SPATIAL = 1u << 1,
    HALLA_AUDIO_MIXED_PLAYBACK = 1u << 2,
    /*
     * Additive (ABI 1 compatible): capture of the local microphone after the
     * transmission decision (VAD/PTT). AGC-style filters can raise the noise
     * floor and trip the host's own voice detector; at this stage the decision
     * to transmit has already been made and the filtered audio goes straight
     * to the encoder. On Halla Mobile the transmission decision happens before
     * the native layer, so capture is reported as HALLA_AUDIO_CAPTURE and this
     * stage is accepted as a synonym.
     */
    HALLA_AUDIO_CAPTURE_AFTER_VAD = 1u << 3
} HallaAudioStage;

typedef enum HallaAudioFrameFlag {
    HALLA_AUDIO_FLAG_WHISPER = 1u << 0
} HallaAudioFrameFlag;

typedef struct HallaAudioFrame {
    uint32_t struct_size;
    uint32_t stage;
    uint64_t connection_id;
    int32_t user_id;          /* 0 in the final mix; selfId on capture. */
    int16_t* samples;         /* Interleaved, mutable S16 PCM. */
    uint32_t frame_count;
    uint32_t channels;
    uint32_t sample_rate;
    /* Additive field: check struct_size before reading it. */
    uint32_t flags;
} HallaAudioFrame;

typedef void (*HallaAudioProcessorFn)(void* plugin_context,
                                      HallaAudioFrame* frame);

typedef struct HallaAudioApiV1 {
    uint32_t version;
    uint32_t struct_size;
    void* context;

    /* One callback per plugin; runs on the audio/UI thread and must not block. */
    int (*register_processor)(void* context, void* plugin_context,
                              HallaAudioProcessorFn processor,
                              uint32_t stage_mask);
    void (*unregister_processor)(void* context);

    int (*set_listener_transform)(void* context, uint64_t connection_id,
                                  const HallaTransform* transform);
    int (*set_user_transform)(void* context, uint64_t connection_id,
                              int32_t user_id, const HallaVec3* position,
                              float min_distance, float max_distance,
                              float rolloff);
    int (*set_user_gain)(void* context, uint64_t connection_id,
                         int32_t user_id, float linear_gain);
    int (*set_user_pan)(void* context, uint64_t connection_id,
                        int32_t user_id, float pan);
    int (*set_user_radio_effect)(void* context, uint64_t connection_id,
                                 int32_t user_id, int enabled,
                                 float strength, float noise_level);
    void (*reset_user)(void* context, uint64_t connection_id,
                       int32_t user_id);
    void (*reset_connection)(void* context, uint64_t connection_id);
    /* Injects 48 kHz S16 PCM, mono/stereo, for at most 10 seconds. */
    int (*play_pcm)(void* context, uint64_t connection_id,
                    const int16_t* samples, uint32_t frame_count,
                    uint32_t channels, float linear_gain);
} HallaAudioApiV1;

/* --------------------------------------------------------- Plugin data */
typedef enum HallaPluginDataTarget {
    HALLA_PLUGIN_DATA_CURRENT_CHANNEL = 0,
    HALLA_PLUGIN_DATA_USERS = 1,
    HALLA_PLUGIN_DATA_SERVER = 2
} HallaPluginDataTarget;

typedef void (*HallaPluginDataFn)(void* plugin_context,
                                  uint64_t connection_id,
                                  int32_t sender_user_id,
                                  const char* topic_utf8,
                                  const uint8_t* data,
                                  size_t data_size);

typedef struct HallaDataApiV1 {
    uint32_t version;
    uint32_t struct_size;
    void* context;

    /* Trusted TLS transport. Max payload: 8 KiB; topic: 64 UTF-8 bytes. */
    int (*send)(void* context, uint64_t connection_id,
                HallaPluginDataTarget target,
                const int32_t* target_user_ids, size_t target_count,
                const char* topic_utf8,
                const uint8_t* data, size_t data_size);
    int (*set_receive_handler)(void* context, void* plugin_context,
                               HallaPluginDataFn handler);
} HallaDataApiV1;

/* ----------------------------------------------------------- Interface */
typedef void (*HallaUiActionFn)(void* plugin_context,
                                const char* action_id_utf8);

typedef struct HallaUiApiV1 {
    uint32_t version;
    uint32_t struct_size;
    void* context;

    int (*show_notification)(void* context, const char* title_utf8,
                             const char* message_utf8, uint32_t timeout_ms);
    int (*register_menu_action)(void* context, const char* action_id_utf8,
                                const char* label_utf8, void* plugin_context,
                                HallaUiActionFn callback);
    /* Global on Windows when the sequence can be registered; local fallback. */
    int (*register_hotkey)(void* context, const char* action_id_utf8,
                           const char* label_utf8,
                           const char* default_sequence_utf8,
                           void* plugin_context, HallaUiActionFn callback);
    void (*unregister_action)(void* context, const char* action_id_utf8);
} HallaUiApiV1;

/* --------------------------------------------------------------- Base */
/*
 * Functions offered by Halla to the plugin. Every pointer is valid only
 * between initialize() and shutdown(). Strings are UTF-8.
 */
typedef struct HallaHostApi {
    uint32_t abi_version;
    uint32_t struct_size;
    void* context;

    void (*log)(void* context, HallaPluginLogLevel level,
                const char* utf8_message);

    /* Returns the required size (NUL included). buffer may be NULL. */
    size_t (*get_settings_json)(void* context, char* buffer,
                                size_t buffer_size);

    /* Solicita que o Halla envie novamente o evento client_state atual. */
    void (*request_client_state)(void* context);

    /* Added after SDK 1.0.63; check struct_size before use. */
    const void* (*query_interface)(void* context,
                                   const char* interface_id_utf8,
                                   uint32_t minimum_version);
} HallaHostApi;

/* Size of the host part that exists in SDK 1.0.63. */
#define HALLA_HOST_API_BASE_SIZE \
    (offsetof(HallaHostApi, request_client_state) + sizeof(void (*)(void*)))

/*
 * Table exported by the plugin. The plugin keeps ownership of the strings and
 * of the structure for as long as the DLL stays loaded.
 */
typedef struct HallaPluginApi {
    uint32_t abi_version;
    uint32_t struct_size;

    const char* id;
    const char* name;
    const char* version;
    const char* author;
    const char* description;

    /* Return 1 on success and 0 on failure. */
    int (*initialize)(const HallaHostApi* host);
    void (*shutdown)(void);

    /* UTF-8 JSON. See PLUGINS.md for the event list. */
    void (*on_event)(const char* utf8_json, size_t json_size);

    /* Called after the user saves the addon settings. */
    void (*on_settings_changed)(const char* utf8_json, size_t json_size);
} HallaPluginApi;

#define HALLA_PLUGIN_API_BASE_SIZE \
    (offsetof(HallaPluginApi, on_settings_changed) + \
     sizeof(void (*)(const char*, size_t)))

typedef const HallaPluginApi* (*HallaPluginEntryFn)(void);

/*
 * A plugin implements and exports `const HallaPluginApi*
 * halla_plugin_entry(void)` without C++ name mangling. The declaration is not
 * emitted here so the host executable never tries to export the same symbol.
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HALLA_PLUGIN_API_H */
