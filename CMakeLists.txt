cmake_minimum_required(VERSION 3.21)

project(Halla VERSION 3.10.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt 6
find_package(Qt6 6.2 QUIET COMPONENTS Widgets Network Multimedia TextToSpeech)
if (Qt6_FOUND)
    set(QT_LIBS Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::TextToSpeech)
    message(STATUS "Halla: usando Qt ${Qt6_VERSION}")
else()
    find_package(Qt6 REQUIRED COMPONENTS Widgets Network Multimedia TextToSpeech)
    set(QT_LIBS Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::TextToSpeech)
endif()

# Opus (biblioteca do sistema no Linux, estática incluída no Windows)
if (WIN32)
    add_library(opus STATIC IMPORTED GLOBAL)
    set_target_properties(opus PROPERTIES
        IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/libopus.a
        INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/include)
    set(OPUS_LIBS opus)
else()
    find_path(OPUS_INCLUDE_DIR opus.h PATH_SUFFIXES opus REQUIRED)
    find_library(OPUS_LIBRARY opus REQUIRED)
    add_library(opus SHARED IMPORTED GLOBAL)
    set_target_properties(opus PROPERTIES IMPORTED_LOCATION ${OPUS_LIBRARY})
    target_include_directories(opus INTERFACE ${OPUS_INCLUDE_DIR})
    set(OPUS_LIBS opus)
endif()

set(HALLA_SOURCES
    src/main.cpp
    src/core/AppLog.cpp
    src/gui/Icons.cpp
    src/gui/TsBanner.cpp
    src/gui/HotkeyEdit.cpp
    src/gui/ServerTreeWidget.cpp
    src/gui/ChatPanel.cpp
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
    src/app/MainWindow.cpp
    src/app/Theme.cpp
    src/app/SoundPack.cpp
    src/app/Speech.cpp
    src/net/NetSession.cpp
    src/net/VoiceEngine.cpp
)

add_executable(Halla ${HALLA_SOURCES})

target_include_directories(Halla PRIVATE
    src
    src/core
    src/gui
    src/dialogs
    src/app
    src/net
)

target_link_libraries(Halla PRIVATE ${QT_LIBS} ${OPUS_LIBS})

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
