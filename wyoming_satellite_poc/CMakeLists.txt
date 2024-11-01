cmake_minimum_required(VERSION 3.26)
project(wyoming_satellite_poc C)

set(CMAKE_C_STANDARD 17)

add_executable(wyoming_satellite_poc main.c ringbuffer.c)

# I know cJSON have CMakeLists.txt, but on MCU project we will not have CMake
target_include_directories(wyoming_satellite_poc PRIVATE ./cJSON)
target_sources(wyoming_satellite_poc PRIVATE ./cJSON/cJSON.c)