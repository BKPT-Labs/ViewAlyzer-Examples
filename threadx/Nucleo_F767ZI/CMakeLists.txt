# Copyright 2025-2026 BKPT, Inc.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
find_program(CMAKE_C_COMPILER arm-none-eabi-gcc REQUIRED)
set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
project(threadx_recorder C ASM)

foreach(path CUBE_F7_DIR THREADX_DIR VIEWALYZER_RECORDER_DIR)
  if(NOT IS_DIRECTORY "${${path}}")
    message(FATAL_ERROR "Supply -D${path}=<source-directory>; build.py fetches pinned dependencies")
  endif()
endforeach()
set(VA_TRANSPORT 4 CACHE STRING "1=ITM/SWO, 2=SEGGER RTT, 4=ViewAlyzer RAM buffer")
set(ST_PROJECT "${CUBE_F7_DIR}/Projects/STM32F767ZI-Nucleo/Applications/ThreadX/Tx_Thread_Creation")
set(TX_PORT "${THREADX_DIR}/ports/cortex_m7/gnu")
file(GLOB TX_COMMON CONFIGURE_DEPENDS "${THREADX_DIR}/common/src/*.c")
file(GLOB TX_ASM CONFIGURE_DEPENDS "${TX_PORT}/src/*.S")
list(FILTER TX_ASM EXCLUDE REGEX "/tx_misra\\.S$")
list(FILTER TX_ASM EXCLUDE REGEX "/tx_initialize_low_level\\.S$")
add_executable(threadx_recorder
  ../common/workload.c
  ${TX_COMMON} ${TX_ASM}
  "${ST_PROJECT}/Src/system_stm32f7xx.c"
  "${ST_PROJECT}/Src/tx_initialize_low_level.s"
  "${ST_PROJECT}/STM32CubeIDE/Application/Startup/startup_stm32f767zitx.s"
  "${VIEWALYZER_RECORDER_DIR}/core/ViewAlyzer.c"
  "${VIEWALYZER_RECORDER_DIR}/core/viewalyzer_cobs.c"
  "${VIEWALYZER_RECORDER_DIR}/threadx/VA_Adapter_ThreadX.c")
set_source_files_properties("${ST_PROJECT}/Src/tx_initialize_low_level.s"
  PROPERTIES COMPILE_OPTIONS "-x;assembler-with-cpp;-include;tx_user.h")
target_include_directories(threadx_recorder PRIVATE ../common
  "${THREADX_DIR}/common/inc" "${TX_PORT}/inc"
  "${CUBE_F7_DIR}/Drivers/CMSIS/Include"
  "${CUBE_F7_DIR}/Drivers/CMSIS/Device/ST/STM32F7xx/Include"
  "${VIEWALYZER_RECORDER_DIR}/core" "${VIEWALYZER_RECORDER_DIR}/threadx")
target_compile_definitions(threadx_recorder PRIVATE STM32F767xx TX_INCLUDE_USER_DEFINE_FILE
  VA_ENABLED=1 VA_RTOS_SELECT=3 VA_DEVICE_HEADER=stm32f767xx.h
  VA_CONFIG_HEADER=va_config.h VA_TRANSPORT=${VA_TRANSPORT})
target_compile_options(threadx_recorder PRIVATE -mcpu=cortex-m7 -mthumb
  -mfpu=fpv5-d16 -mfloat-abi=hard -g3 -Og -ffunction-sections -fdata-sections
  $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wno-unused-parameter>)
target_link_options(threadx_recorder PRIVATE -mcpu=cortex-m7 -mthumb
  -mfpu=fpv5-d16 -mfloat-abi=hard --specs=nano.specs --specs=nosys.specs
  "-T${ST_PROJECT}/STM32CubeIDE/STM32F767ZITX_FLASH.ld"
  -Wl,--gc-sections "-Wl,-Map=${CMAKE_BINARY_DIR}/threadx_recorder.map")
set_target_properties(threadx_recorder PROPERTIES SUFFIX ".elf")
if(VA_TRANSPORT STREQUAL "2")
  # Reuse the repository's licensed, unmodified SEGGER sources.
  set(RTT "${CMAKE_CURRENT_SOURCE_DIR}/../../freertos/Nucleo_F446RE/SEGGER_RTT")
  target_sources(threadx_recorder PRIVATE "${RTT}/SEGGER_RTT.c")
  # SEGGER channel 0 only accepts its buffer size at compile time; the runtime
  # ConfigUpBuffer call preserves that default buffer. Keep nonblocking mode.
  set_source_files_properties("${RTT}/SEGGER_RTT.c" PROPERTIES
    COMPILE_DEFINITIONS "BUFFER_SIZE_UP=32768")
  target_include_directories(threadx_recorder PRIVATE "${RTT}")
endif()
find_program(OBJCOPY arm-none-eabi-objcopy REQUIRED)
add_custom_command(TARGET threadx_recorder POST_BUILD COMMAND "${OBJCOPY}" -O ihex
  "$<TARGET_FILE:threadx_recorder>" "${CMAKE_BINARY_DIR}/threadx_recorder.hex")
