# Copyright 2025-2026 BKPT, Inc.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
find_program(CMAKE_C_COMPILER arm-none-eabi-gcc REQUIRED)
set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
project(rtx5_recorder C ASM)

foreach(path CUBE_DIR RTX5_DIR CMSIS_DIR VIEWALYZER_RECORDER_DIR)
  if(NOT IS_DIRECTORY "${${path}}")
    message(FATAL_ERROR "Supply -D${path}=<source-directory>")
  endif()
endforeach()
set(BOARD g474 CACHE STRING "g474 (ST-Link) or f767 (J-Link)")
set(VA_TRANSPORT 4 CACHE STRING "1=SWO, 2=RTT, 4=ViewAlyzer RAM buffer")
option(VA_ENABLED "Enable recorder instrumentation" ON)
if(BOARD STREQUAL "g474")
  set(FAMILY g4)
  set(DEVICE stm32g474xx.h)
  set(DEVICE_DEFINE STM32G474xx)
  set(CPU cortex-m4)
  set(FPU fpv4-sp-d16)
  set(ST_PROJECT "${CUBE_DIR}/Projects/NUCLEO-G474RE/Applications/ThreadX/Tx_Thread_Creation")
  set(STARTUP "${ST_PROJECT}/STM32CubeIDE/Application/Startup/startup_stm32g474retx.s")
  set(LINKER "${ST_PROJECT}/STM32CubeIDE/STM32G474RETX_FLASH.ld")
  if(VA_TRANSPORT STREQUAL "2")
    message(FATAL_ERROR "G474 uses ST-Link; select SWO or RAM buffer")
  endif()
elseif(BOARD STREQUAL "f767")
  set(FAMILY f7)
  set(DEVICE stm32f767xx.h)
  set(DEVICE_DEFINE STM32F767xx)
  set(CPU cortex-m7)
  set(FPU fpv5-d16)
  set(ST_PROJECT "${CUBE_DIR}/Projects/STM32F767ZI-Nucleo/Applications/ThreadX/Tx_Thread_Creation")
  set(STARTUP "${ST_PROJECT}/STM32CubeIDE/Application/Startup/startup_stm32f767zitx.s")
  set(LINKER "${ST_PROJECT}/STM32CubeIDE/STM32F767ZITX_FLASH.ld")
else()
  message(FATAL_ERROR "Unsupported BOARD: ${BOARD}")
endif()
string(TOUPPER "${FAMILY}" FAMILY_UPPER)
file(READ "${LINKER}" RTX_LINKER_TEXT)
if(NOT RTX_LINKER_TEXT MATCHES "_Min_Stack_Size = 0x400 *;")
  message(FATAL_ERROR "Unexpected donor MSP reservation")
endif()
string(REGEX REPLACE "_Min_Stack_Size = 0x400 *;" "_Min_Stack_Size = 0x2000;"
  RTX_LINKER_TEXT "${RTX_LINKER_TEXT}")
set(LINKER "${CMAKE_BINARY_DIR}/rtx5_board.ld")
file(WRITE "${LINKER}" "${RTX_LINKER_TEXT}")

file(GLOB RTX_SOURCES CONFIGURE_DEPENDS "${RTX5_DIR}/Source/*.c")
# Our adapter supplies the selected native hooks. Never link the weak no-op provider.
list(FILTER RTX_SOURCES EXCLUDE REGEX "/rtx_evr\\.c$")
add_executable(rtx5_recorder common/workload.c common/board.c
  ${RTX_SOURCES} "${RTX5_DIR}/Config/RTX_Config.c"
  "${CMSIS_DIR}/CMSIS/RTOS2/Source/os_systick.c"
  "${ST_PROJECT}/Src/system_stm32${FAMILY}xx.c" "${STARTUP}"
  "${VIEWALYZER_RECORDER_DIR}/core/ViewAlyzer.c"
  "${VIEWALYZER_RECORDER_DIR}/core/viewalyzer_cobs.c")
if(VA_ENABLED)
  target_sources(rtx5_recorder PRIVATE "${VIEWALYZER_RECORDER_DIR}/rtx5/VA_Adapter_RTX5.c"
    "${VIEWALYZER_RECORDER_DIR}/rtx5/ports/irq_armv7m.S")
  target_compile_options(rtx5_recorder PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:-include;ViewAlyzerRTX5Hook.h>")
else()
  target_sources(rtx5_recorder PRIVATE "${RTX5_DIR}/Source/GCC/irq_armv7m.S")
  target_compile_definitions(rtx5_recorder PRIVATE EVR_RTX_DISABLE)
endif()
target_include_directories(rtx5_recorder PRIVATE common
  "${RTX5_DIR}/Include" "${RTX5_DIR}/Config"
  "${CMSIS_DIR}/CMSIS/Core/Include" "${CMSIS_DIR}/CMSIS/RTOS2/Include"
  "${CUBE_DIR}/Drivers/CMSIS/Device/ST/STM32${FAMILY_UPPER}xx/Include"
  "${VIEWALYZER_RECORDER_DIR}/core" "${VIEWALYZER_RECORDER_DIR}/rtx5")
target_compile_definitions(rtx5_recorder PRIVATE ${DEVICE_DEFINE}
  CMSIS_device_header="${DEVICE}" VA_DEVICE_HEADER=${DEVICE}
  VA_CONFIG_HEADER=va_config.h VA_RTOS_SELECT=4 VA_ENABLED=$<BOOL:${VA_ENABLED}>
  VA_TRANSPORT=${VA_TRANSPORT} OS_STACK_WATERMARK=1 OS_DYNAMIC_MEM_SIZE=32768
  OS_STACK_SIZE=2048 OS_TIMER_THREAD_STACK_SIZE=2048 OS_IDLE_THREAD_STACK_SIZE=512)
target_compile_options(rtx5_recorder PRIVATE -mcpu=${CPU} -mthumb -mfpu=${FPU}
  -mfloat-abi=hard -g3 -O2 -ffunction-sections -fdata-sections
  "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wno-unused-parameter>")
target_link_options(rtx5_recorder PRIVATE -mcpu=${CPU} -mthumb -mfpu=${FPU}
  -mfloat-abi=hard --specs=nano.specs --specs=nosys.specs "-T${LINKER}"
  -Wl,--gc-sections "-Wl,-Map=${CMAKE_BINARY_DIR}/rtx5_recorder.map")
set_target_properties(rtx5_recorder PROPERTIES SUFFIX ".elf")
if(VA_TRANSPORT STREQUAL "2")
  set(RTT "${CMAKE_CURRENT_SOURCE_DIR}/../freertos/Nucleo_F446RE/SEGGER_RTT")
  target_sources(rtx5_recorder PRIVATE "${RTT}/SEGGER_RTT.c")
  set_source_files_properties("${RTT}/SEGGER_RTT.c" PROPERTIES COMPILE_DEFINITIONS "BUFFER_SIZE_UP=32768")
  target_include_directories(rtx5_recorder PRIVATE "${RTT}")
endif()
find_program(OBJCOPY arm-none-eabi-objcopy REQUIRED)
add_custom_command(TARGET rtx5_recorder POST_BUILD COMMAND "${OBJCOPY}" -O ihex
  "$<TARGET_FILE:rtx5_recorder>" "${CMAKE_BINARY_DIR}/rtx5_recorder.hex")
