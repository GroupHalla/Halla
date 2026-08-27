cmake_minimum_required(VERSION 3.21)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" HALLA_VERSION LIMIT_COUNT 1)
string(STRIP "${HALLA_VERSION}" HALLA_VERSION)
if (NOT HALLA_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION inválida: '${HALLA_VERSION}'")
endif()
project(Halla VERSION ${HALLA_VERSION} LANGUAGES CXX)

set(HALLA_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${HALLA_GENERATED_DIR}")
configure_file(src/version.h.in "${HALLA_GENERATED_DIR}/version.h" @ONLY)
if (WIN32)
    configure_file(src/halla.rc.in "${HALLA_GENERATED_DIR}/halla.rc" @ONLY)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

option(HALLA_ENABLE_WEBRTC "Enable WebRTC signaling/scaffolding" ON)
option(HALLA_ENABLE_WEBRTC_NATIVE "Compile native WebRTC implementation with prebuilt SDK" OFF)
set(HALLA_WEBRTC_SDK_DIR "" CACHE PATH "Path to prebuilt Halla/libwebrtc SDK")

find_package(OpenSSL REQUIRED)

# Qt 6
find_package(Qt6 6.2 QUIET COMPONENTS Widgets Network Multimedia TextToSpeech DBus)
if (Qt6_FOUND)
    set(QT_LIBS Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::TextToSpeech Qt6::DBus)
    message(STATUS "Halla: usando Qt ${Qt6_VERSION}")
else()
    find_package(Qt6 REQUIRED COMPONENTS Widgets Network Multimedia TextToSpeech DBus)
    set(QT_LIBS Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::TextToSpeech Qt6::DBus)
endif()

include(FetchContent)
set(BUILD_WITH_QT6 ON CACHE BOOL "Build QtKeychain with Qt 6" FORCE)
set(BUILD_TEST_APPLICATION OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable bundled dependency tests" FORCE)
set(BUILD_QTQUICK_DEMO OFF CACHE BOOL "" FORCE)
set(BUILD_TRANSLATIONS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build bundled dependencies statically" FORCE)
FetchContent_Declare(qtkeychain
    GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain.git
    GIT_TAG ad7344c45a86a4f66cbafc4b081b5f7b876cb0b7
    GIT_SHALLOW FALSE
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DQTKEYCHAIN_CMAKELISTS=<SOURCE_DIR>/qtkeychain/CMakeLists.txt
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchQtKeychain.cmake
)
FetchContent_MakeAvailable(qtkeychain)

# Opus vem da toolchain/sistema. Não versionamos mais libopus.a no Git.
find_path(OPUS_INCLUDE_DIR opus.h PATH_SUFFIXES opus third_party/opus/include REQUIRED)
find_library(OPUS_LIBRARY opus REQUIRED)
add_library(opus UNKNOWN IMPORTED GLOBAL)
set_target_properties(opus PROPERTIES IMPORTED_LOCATION ${OPUS_LIBRARY})
target_include_directories(opus INTERFACE ${OPUS_INCLUDE_DIR})
set(OPUS_LIBS opus)

set(HALLA_SOURCES
    src/main.cpp
    src/halla.qrc
    src/core/AppLog.cpp
    src/core/BadgeRegistry.cpp
    src/core/BadgeRegistry.h
    src/core/GroupIconCache.cpp
    src/core/GroupIconCache.h
    src/core/SecureStore.cpp
    src/plugins/PluginManager.cpp
    src/plugins/PluginManager.h
    src/plugins/RadioVoiceEffect.cpp
    src/plugins/RadioVoiceEffect.h
    src/plugins/TalkingOverlay.cpp
    src/plugins/TalkingOverlay.h
    src/gui/Icons.cpp
    src/gui/TsBanner.cpp
    src/gui/HotkeyEdit.cpp
    src/gui/ServerTreeWidget.cpp
    src/gui/ChatPanel.cpp
    src/gui/RichTextBrowser.cpp
    src/gui/InfoPanel.cpp
    src/gui/ServerTab.cpp
    src/gui/WelcomePage.cpp
    src/dialogs/ConnectDialog.cpp
    src/dialogs/ChannelDialog.cpp
    src/dialogs/IdentityDialog.cpp
    src/dialogs/BookmarksDialog.cpp
    src/dialogs/OptionsDialog.cpp
    src/dialogs/GroupsDialog.cpp
    src/dialogs/MiniDialogs.cpp
    src/dialogs/LogDialog.cpp
    src/dialogs/ToolsDialogs.cpp
    src/dialogs/AdminDialogs.cpp
    src/dialogs/AboutDialog.cpp
    src/dialogs/ScreenShareDialog.cpp
    src/app/MainWindow.cpp
    src/app/MainWindowUpdates.cpp
    src/app/Theme.cpp
    src/app/SoundPack.cpp
    src/app/Speech.cpp
    src/net/NetSession.cpp
    src/net/VoiceEngine.cpp
    src/webrtc/HallaWebRtcSession.cpp
    src/webrtc/HallaWebRtcSession.h
    src/webrtc/MediaFoundationH264.cpp
    src/webrtc/MediaFoundationH264.h
)

add_executable(Halla ${HALLA_SOURCES})

target_include_directories(Halla PRIVATE
    "${HALLA_GENERATED_DIR}"
    src
    src/core
    src/gui
    src/dialogs
    src/app
    src/net
    src/webrtc
    src/plugins
    sdk
)

target_link_libraries(Halla PRIVATE ${QT_LIBS} qt6keychain)
target_include_directories(Halla PRIVATE ${OPUS_INCLUDE_DIR} ${OPENSSL_INCLUDE_DIR})
set(HALLA_WEBRTC_NATIVE_ACTIVE OFF)

if (HALLA_ENABLE_WEBRTC AND HALLA_WEBRTC_SDK_DIR)
    set(HALLA_WEBRTC_INCLUDE_DIR "${HALLA_WEBRTC_SDK_DIR}/include")
    if (WIN32)
        set(HALLA_WEBRTC_LIBRARY "${HALLA_WEBRTC_SDK_DIR}/lib/windows-x64/webrtc.lib")
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(HALLA_WEBRTC_LIBRARY "${HALLA_WEBRTC_SDK_DIR}/lib/linux-arm64/libwebrtc.a")
    else()
        set(HALLA_WEBRTC_LIBRARY "${HALLA_WEBRTC_SDK_DIR}/lib/linux-x64/libwebrtc.a")
    endif()
    if (EXISTS "${HALLA_WEBRTC_INCLUDE_DIR}" AND EXISTS "${HALLA_WEBRTC_LIBRARY}")
        message(STATUS "Halla: WebRTC SDK encontrado em ${HALLA_WEBRTC_SDK_DIR}")
        if (HALLA_ENABLE_WEBRTC_NATIVE)
            set(HALLA_WEBRTC_INCLUDE_DIRS "${HALLA_WEBRTC_INCLUDE_DIR}")
            foreach(extra_dir
                    "${HALLA_WEBRTC_INCLUDE_DIR}/third_party/abseil-cpp"
                    "${HALLA_WEBRTC_INCLUDE_DIR}/third_party/libyuv/include"
                    "${HALLA_WEBRTC_INCLUDE_DIR}/third_party/boringssl/src/include")
                if (EXISTS "${extra_dir}")
                    list(APPEND HALLA_WEBRTC_INCLUDE_DIRS "${extra_dir}")
                endif()
            endforeach()
            set(HALLA_WEBRTC_NATIVE_ACTIVE ON)
            target_compile_definitions(Halla PRIVATE HALLA_WEBRTC_NATIVE=1)
            if (WIN32)
                target_compile_definitions(Halla PRIVATE WEBRTC_WIN=1 NOMINMAX WIN32_LEAN_AND_MEAN)
            else()
                target_compile_definitions(Halla PRIVATE WEBRTC_POSIX=1)
            endif()
            # WebRTC headers from current Chromium use C++20 features such as
            # std::span and constrained templates. Keep the default project at
            # C++17, but raise the target when native WebRTC is explicitly ON.
            target_compile_features(Halla PRIVATE cxx_std_20)
            target_include_directories(Halla PRIVATE ${HALLA_WEBRTC_INCLUDE_DIRS})
            target_link_libraries(Halla PRIVATE "${HALLA_WEBRTC_LIBRARY}")
            if (WIN32)
                target_link_libraries(Halla PRIVATE winmm mmdevapi secur32 crypt32 iphlpapi dmoguids strmiids msdmo wmcodecdspuuid ole32 oleaut32 user32 advapi32 d3d11 dxgi mf mfplat mfuuid propsys)
            endif()
            message(STATUS "Halla: WebRTC nativo habilitado (${HALLA_WEBRTC_LIBRARY})")
        else()
            message(STATUS "Halla: WebRTC SDK disponível, implementação nativa desligada; usando stub")
        endif()
    else()
        message(WARNING "Halla: SDK WebRTC incompleto; usando stub WebRTC")
    endif()
else()
    message(STATUS "Halla: WebRTC nativo desabilitado/sem SDK; usando stub")
endif()

if (NOT HALLA_WEBRTC_NATIVE_ACTIVE)
    target_link_libraries(Halla PRIVATE ${OPUS_LIBS} OpenSSL::Crypto)
else()
    message(STATUS "Halla: usando Opus/BoringSSL embutidos no SDK WebRTC para evitar símbolos duplicados")
endif()

if (MSVC)
    target_compile_options(Halla PRIVATE /W3 /utf-8)
else()
    target_compile_options(Halla PRIVATE -Wall -Wextra)
endif()

# App GUI nativo (sem console) no Windows / bundle no macOS
set_target_properties(Halla PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME "Halla"
)

install(TARGETS Halla RUNTIME DESTINATION bin BUNDLE DESTINATION .)

# Ícone e informações de versão embutidos no Halla.exe (Windows)
if (WIN32)
    target_sources(Halla PRIVATE "${HALLA_GENERATED_DIR}/halla.rc")
    target_link_libraries(Halla PRIVATE user32)
endif()
