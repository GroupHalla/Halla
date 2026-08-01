cmake_minimum_required(VERSION 3.21)

project(Halla VERSION 3.6.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt 6 (com fallback para Qt 5.15)
find_package(Qt6 6.2 QUIET COMPONENTS Widgets)
if (Qt6_FOUND)
    set(QT_LIBS Qt6::Widgets)
    message(STATUS "Halla: usando Qt ${Qt6_VERSION}")
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets)
    set(QT_LIBS Qt5::Widgets)
    message(STATUS "Halla: usando Qt 5")
endif()

set(HALLA_SOURCES
    src/main.cpp
    src/core/AppLog.cpp
    src/gui/Icons.cpp
    src/gui/TsBanner.cpp
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
    src/dialogs/AboutDialog.cpp
    src/app/MainWindow.cpp
)

add_executable(Halla ${HALLA_SOURCES})

target_include_directories(Halla PRIVATE
    src
    src/core
    src/gui
    src/dialogs
    src/app
)

target_link_libraries(Halla PRIVATE ${QT_LIBS})

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
