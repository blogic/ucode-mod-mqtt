# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 John Crispin <john@phrozen.org>

cmake_minimum_required(VERSION 3.10)
project(ucode-mqtt C)

set(CMAKE_C_STANDARD 99)

# Force output to build directory even for in-source builds
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)

find_library(MOSQUITTO_LIB mosquitto)
find_library(UBOX_LIB ubox)
find_library(UCODE_LIB ucode)

if(NOT MOSQUITTO_LIB)
    message(FATAL_ERROR "libmosquitto not found - install libmosquitto-dev")
endif()

if(NOT UBOX_LIB)
    message(FATAL_ERROR "libubox not found - install libubox-dev")
endif()

if(NOT UCODE_LIB)
    message(FATAL_ERROR "libucode not found - install ucode-dev")
endif()

add_library(mqtt MODULE
    src/mqtt.c
    src/mqtt_util.c
    src/uloop_integration.c
)

target_compile_options(mqtt PRIVATE -Wall)

target_include_directories(mqtt PRIVATE
    include
)

target_link_libraries(mqtt
    ${MOSQUITTO_LIB}
    ${UBOX_LIB}
    ${UCODE_LIB}
)

set_target_properties(mqtt PROPERTIES
    PREFIX ""
    SUFFIX ".so"
)

install(TARGETS mqtt
    DESTINATION lib/ucode
)
