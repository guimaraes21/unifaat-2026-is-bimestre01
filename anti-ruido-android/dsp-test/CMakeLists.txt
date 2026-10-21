cmake_minimum_required(VERSION 3.16)
project(anc_dsp_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(DSP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp/dsp)
file(GLOB DSP_SOURCES ${DSP_DIR}/*.cpp)

add_executable(anc_sim anc_sim.cpp ${DSP_SOURCES})
target_include_directories(anc_sim PRIVATE ${DSP_DIR})
target_compile_options(anc_sim PRIVATE -Wall -Wextra)
