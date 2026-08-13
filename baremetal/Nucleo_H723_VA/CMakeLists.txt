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

# ViewAlyzer recorder: bare-metal. Default transport is the RAM buffer
# (works with the on-board ST-Link — no SWO pin, no SEGGER RTT).
# Override at configure time to try another backend, e.g. -DVA_TRANSPORT=ARM_ITM.
set(VA_TRANSPORT "RAM_BUFFER" CACHE STRING "ViewAlyzer transport backend (RAM_BUFFER | ARM_ITM | JLINK_RTT | CUSTOM_TRANSPORT)")
add_definitions(-DVA_ENABLED=1 -DVA_RTOS_SELECT=0 -DVA_TRANSPORT=${VA_TRANSPORT}
                -DVA_DEVICE_HEADER=stm32h723xx.h)

# Selective tracing demo. OFF by default, so this example traces everything
# out of the box. Turn it on with -DVA_SELECTIVE_TRACING=ON and the recorder
# compiles in only the categories named in va_config.h — the rest disappear
# entirely (events, packet builders, registries), and calls to their APIs
# still build. The ViewAlyzer app reads the enabled set off the wire, so a
# view that is empty because of this says so instead of looking like nothing
# happened.
option(VA_SELECTIVE_TRACING
       "Trace only the categories listed in va_config.h (see that file)" OFF)
if(VA_SELECTIVE_TRACING)
    # Bare token, never quoted: quoted forms are mangled by CMD, Ninja
    # response files, Makefiles, and the Keil/IAR define boxes.
    add_definitions(-DVA_CONFIG_HEADER=va_config.h)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR})
    message(STATUS "ViewAlyzer: selective tracing via va_config.h")
endif()

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME Nucleo_H723_VA)

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
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${VIEWALYZER_RECORDER_DIR}/core
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
