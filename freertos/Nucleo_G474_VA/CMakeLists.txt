cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# NOTE: add_definitions() is directory-scoped, so everything built under this
# directory sees these — including the FreeRTOS kernel target created by
# cmake/stm32cubemx. That is REQUIRED, not incidental: the kernel compiles the
# ViewAlyzer trace hooks (through FreeRTOSConfig.h), so it must see exactly the
# same VA_* configuration as the recorder. A kernel with a smaller VA_TRACE_*
# set installs fewer hooks, and the result is a capture with zero events and no
# error anywhere. Never move a VA_* define to target_compile_definitions on one
# target only.
# VA_MAX_TASKS: this demo runs 17 application tasks plus IDLE and Tmr Svc;
# the recorder default of 16 slots would overflow (extra tasks trace as
# id 0 and health reports taskmap_full).
add_definitions(-DVA_ENABLED=1 -DVA_RTOS_SELECT=1 -DVA_DEVICE_HEADER=stm32g474xx.h
    -DVA_MAX_TASKS=24
)

# Selective tracing demo. OFF by default, so this example traces everything out
# of the box. Turn it on to see what a trimmed build looks like:
#
#   cmake -B build/selective -G Ninja -DVA_SELECTIVE_TRACING=ON \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
#
# This particular set answers "which task is blocking on which mutex, and how
# deep are the stacks?" without paying for the ~700 mutex give/take events a
# second that this demo produces. Mutex CONTENTION still works with mutex
# events off: the mutex and both tasks stay registered so the contention event
# can name them, but no give/take events reach the wire (measured on a
# Nucleo-G474RE: mutex 678 -> 0, contention 18 -> 19, everything else intact).
option(VA_SELECTIVE_TRACING
       "Trace tasks, stacks, ISRs and mutex contention only" OFF)
if(VA_SELECTIVE_TRACING)
    # The category set lives in the project-owned va_config.h (see the
    # VA_CONFIG_HEADER pattern in ViewAlyzerConfig.h). include_directories()
    # is directory-scoped for the same reason add_definitions() is above:
    # the kernel target must find va_config.h too.
    add_definitions(-DVA_CONFIG_HEADER=va_config.h)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR})
    message(STATUS "ViewAlyzer: selective tracing via va_config.h (tasks, "
                   "stacks, ISRs, mutex contention, user values)")
endif()

# RAM-buffer transport: the recorder owns a ring in SRAM and any debug probe
# (ST-Link included) drains it with running-target memory reads - no SWO pin,
# no SEGGER code. OFF keeps the default ITM/SWO build. Build into its own
# directory so the SWO build stays untouched:
#
#   cmake --preset Debug-RamBuf && cmake --build build/Debug-RamBuf
#
# The host finds the ring by scanning SRAM for its magic tag, or directly via
# the _VA_RAMBUF ELF symbol (the app auto-fills it when the ELF is loaded).
# FreeRTOS's idle task does not WFI in this configuration, so the Zephyr
# demo's busy-idle workaround is not needed here.
option(VA_RAMBUF "Use the RAM-buffer transport instead of ITM/SWO" OFF)
if(VA_RAMBUF)
    add_definitions(-DVA_TRANSPORT=RAM_BUFFER -DVA_RAMBUF_SIZE=16384u)
    message(STATUS "ViewAlyzer: RAM-buffer transport (16 KB ring)")
endif()

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME Nucelo_G474_VA)

# Include toolchain file
include("cmake/gcc-arm-none-eabi.cmake")

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

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

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${VIEWALYZER_RECORDER_DIR}/core/ViewAlyzer.c
    ${VIEWALYZER_RECORDER_DIR}/core/viewalyzer_cobs.c
    ${VIEWALYZER_RECORDER_DIR}/freertos/VA_Adapter_FreeRTOS.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${VIEWALYZER_RECORDER_DIR}/core
    ${VIEWALYZER_RECORDER_DIR}/freertos
)

# Force the FreeRTOS source files (compiled inside the RTOS2 OBJECT library
# created by cmake/stm32cubemx) to find the ViewAlyzer hook header in the
# shared recorder directory instead of any local copy under Core/Inc.
# BEFORE prepends our paths so they win over the stm32cubemx INTERFACE
# include directories (which list Core/Inc first).
target_include_directories(FreeRTOS BEFORE PRIVATE
    ${VIEWALYZER_RECORDER_DIR}/core
    ${VIEWALYZER_RECORDER_DIR}/freertos
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
