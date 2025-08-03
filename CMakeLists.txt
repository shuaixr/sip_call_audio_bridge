cmake_minimum_required(VERSION 3.5)
project(sip_call_audio_bridge)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_library(PJSUA2_LIB pjsua2)
find_library(PJSUA_LIB pjsua)
find_library(PJSIP_LIB pjsip)
find_library(PJLIB_UTIL_LIB pjlib-util)
find_library(PJLIB_LIB pj)

include_directories(/usr/local/include/pj)

add_executable(sip_call_audio_bridge main.cpp)

target_link_libraries(sip_call_audio_bridge
    ${PJSUA2_LIB}
    ${PJSUA_LIB}
    ${PJSIP_LIB}
    ${PJLIB_UTIL_LIB}
    ${PJLIB_LIB}
)
