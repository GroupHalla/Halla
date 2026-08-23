cmake_minimum_required(VERSION 3.21)
project(HallaHelloWorld LANGUAGES CXX)

add_library(halla_hello_world SHARED hello_world.cpp)
target_compile_features(halla_hello_world PRIVATE cxx_std_17)
target_include_directories(halla_hello_world PRIVATE ../../../sdk)
set_target_properties(halla_hello_world PROPERTIES
    PREFIX ""
    OUTPUT_NAME "halla_hello_world"
)
