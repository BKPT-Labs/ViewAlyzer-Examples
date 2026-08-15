cmake_minimum_required(VERSION 3.22)

# ── Nucleo-F767ZI ViewAlyzer throughput benchmark (bare-metal, CMSIS-only) ──
# Transport switch (J-Link-focused):
#   -DVA_TRANSPORT=RAM_BUFFER  (default) probe-agnostic RAM ring
#   -DVA_TRANSPORT=JLINK_RTT   SEGGER RTT via the vendored SEGGER_RTT sources
set(VA_TRANSPORT "RAM_BUFFER" CACHE STRING "ViewAlyzer transport backend (RAM_BUFFER | JLINK_RTT | ARM_ITM)")

# Offered-load ceiling in KiB/s (0 = unbounded blast).
set(TP_TARGET_KBPS "8000" CACHE STRING "Throughput benchmark offered load in KiB/s (0 = unbounded)")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

set(CMAKE_PROJECT_NAME Nucleo_F767_Throughput)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})
enable_language(C ASM)

add_executable(${CMAKE_PROJECT_NAME})

# ── Locate the ViewAlyzer Recorder ────────────────────────────────────
# Resolution order:
#   1. -DVIEWALYZER_RECORDER_DIR=<path> on the cmake command line
#   2. The VIEWALYZER_RECORDER_DIR environment variable
#   3. A ViewAlyzer checkout cloned next to this examples repo:
#      <workspace>/ViewAlyzer/ViewAlyzerRecorder
if(NOT DEFINED VIEWALYZER_RECORDER_DIR)
  if(DEFINED ENV{VIEWALYZER_RECORDER_DIR})
    set(VIEWALYZER_RECORDER_DIR "$ENV{VIEWALYZER_RECORDER_DIR}")
  else()
    set(VIEWALYZER_RECORDER_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../ViewAlyzer/ViewAlyzerRecorder")
  endif()
endif()
get_filename_component(VIEWALYZER_RECORDER_DIR "${VIEWALYZER_RECORDER_DIR}" ABSOLUTE)
if(NOT EXISTS "${VIEWALYZER_RECORDER_DIR}/core/ViewAlyzer.c")
  message(FATAL_ERROR
    "ViewAlyzer Recorder not found at: ${VIEWALYZER_RECORDER_DIR}\n"
    "Clone the ViewAlyzer repo next to this examples repo, or point "
    "VIEWALYZER_RECORDER_DIR (CMake option or environment variable) at a "
    "ViewAlyzerRecorder directory.")
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    startup_stm32f767xx.s
    Core/Src/system_stm32f7xx.c
    Core/Src/main.c
    ${VIEWALYZER_RECORDER_DIR}/core/ViewAlyzer.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Inc
    Drivers/CMSIS/Core/Include
    Drivers/CMSIS/Device/ST/STM32F7xx/Include
    ${VIEWALYZER_RECORDER_DIR}/core
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    STM32F767xx
    VA_ENABLED=1
    VA_RTOS_SELECT=0
    VA_DEVICE_HEADER=stm32f767xx.h
    VA_TRANSPORT=${VA_TRANSPORT}
    VA_TP_TEST=1
    TP_TARGET_KBPS=${TP_TARGET_KBPS}u
)

if(VA_TRANSPORT STREQUAL "JLINK_RTT")
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        SEGGER_RTT/SEGGER_RTT.c
        SEGGER_RTT/SEGGER_RTT_ASM_ARMv7M.S
    )
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE SEGGER_RTT)
    # Channel 1: SEGGER_RTT_ConfigUpBuffer() on channel 0 cannot resize the
    # compile-time terminal buffer (silently keeps 1 KB), so the recorder's
    # buffer only applies on channels >= 1 — which also matches the ViewAlyzer
    # app's default RTT channel. The recorder's default RTT mode is
    # NO_BLOCK_SKIP (drop whole packets when the host lags, same semantics
    # as the RAM buffer's DROP mode). Pass
    # -DVA_RTT_MODE=SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL for a lossless
    # benchmark run; a blocking build freezes once the buffer fills unless a
    # host is draining the channel.
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        VA_RTT_CHANNEL=1
        VA_RTT_BUFFER_SIZE=8192u
    )
    set(VA_RTT_MODE "" CACHE STRING "Override VA_RTT_MODE (empty = recorder default NO_BLOCK_SKIP)")
    if(NOT VA_RTT_MODE STREQUAL "")
        target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE VA_RTT_MODE=${VA_RTT_MODE})
    endif()
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} m)
