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

add_definitions(-DVA_ENABLED=1 -DVA_RTOS_SELECT=1 -DVA_DEVICE_HEADER=stm32u385xx.h)
# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME Nucleo_U385)

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
target_include_directories(RTOS2 BEFORE PRIVATE
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
