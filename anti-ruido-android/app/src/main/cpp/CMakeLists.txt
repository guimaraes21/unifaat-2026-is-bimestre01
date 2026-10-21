cmake_minimum_required(VERSION 3.22.1)
project(antiruido CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Oboe vem como AAR com "prefab" (dependência com.google.oboe:oboe no Gradle).
find_package(oboe REQUIRED CONFIG)

file(GLOB DSP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/dsp/*.cpp)

add_library(antiruido SHARED
        AncEngine.cpp
        jni_bridge.cpp
        ${DSP_SOURCES})

target_include_directories(antiruido PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/dsp)

# -O2 mesmo no build de debug: o laço por amostra precisa caber folgado em CPUs fracas de multimídia.
target_compile_options(antiruido PRIVATE -Wall -Wextra -O2)

target_link_libraries(antiruido oboe::oboe log)
