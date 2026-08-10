cmake_minimum_required(VERSION 3.21)

project(Halla VERSION 3.10.0 LANGUAGES CXX)

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
find_package(Qt6 6.2 QUIET COMPONENTS Widgets Network Multimedia TextToSpeech)
if (Qt6_FOUND)
    set(QT_LIBS Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::TextToSpeech)
    message(STATUS "Halla: usando Qt ${Qt6_VERSION}")
else()
    find_package(Qt6 REQUIRED COMPONENTS Widgets Network Multimedia TextToSpeech)
    set(QT_LIBS Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::TextToSpeech)
endif()

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
)

add_executable(Halla ${HALLA_SOURCES})

target_include_directories(Halla PRIVATE
    src
    src/core
    src/gui
    src/dialogs
    src/app
    src/net
    src/webrtc
)

target_link_libraries(Halla PRIVATE ${QT_LIBS})
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
                target_link_libraries(Halla PRIVATE winmm secur32 crypt32 iphlpapi dmoguids strmiids msdmo wmcodecdspuuid ole32 oleaut32 user32 advapi32)
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
    target_sources(Halla PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/halla.rc)
endif()
