cmake_minimum_required(VERSION 3.21)
project(halla_advanced_sdk_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(halla_advanced_sdk SHARED advanced_sdk.cpp)
target_include_directories(halla_advanced_sdk PRIVATE ../../../sdk)
target_compile_definitions(halla_advanced_sdk PRIVATE HALLA_PLUGIN_BUILD=1)
set_target_properties(halla_advanced_sdk PROPERTIES
    PREFIX ""
    OUTPUT_NAME "halla_advanced_sdk")
