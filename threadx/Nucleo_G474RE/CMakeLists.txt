# Copyright 2025-2026 BKPT, Inc.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
find_program(CMAKE_C_COMPILER arm-none-eabi-gcc REQUIRED)
set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
project(threadx_recorder C ASM)
foreach(path CUBE_G4_DIR THREADX_DIR VIEWALYZER_RECORDER_DIR)
  if(NOT IS_DIRECTORY "${${path}}")
    message(FATAL_ERROR "Supply -D${path}=<source-directory>; build.py fetches pinned dependencies")
  endif()
endforeach()
set(VA_TRANSPORT 4 CACHE STRING "1=ITM/SWO, 4=ViewAlyzer RAM buffer")
set(ST_PROJECT "${CUBE_G4_DIR}/Projects/NUCLEO-G474RE/Applications/ThreadX/Tx_Thread_Creation")
set(HAL "${CUBE_G4_DIR}/Drivers/STM32G4xx_HAL_Driver")
set(TX_PORT "${THREADX_DIR}/ports/cortex_m4/gnu")
file(GLOB TX_COMMON CONFIGURE_DEPENDS "${THREADX_DIR}/common/src/*.c")
file(GLOB TX_ASM CONFIGURE_DEPENDS "${TX_PORT}/src/*.S")
list(FILTER TX_ASM EXCLUDE REGEX "/tx_misra\\.S$")
list(FILTER TX_ASM EXCLUDE REGEX "/tx_initialize_low_level\\.S$")
add_executable(threadx_recorder ../common/workload.c board.c tx_initialize_low_level.S
  ${TX_COMMON} ${TX_ASM}
  "${ST_PROJECT}/Src/main.c" "${ST_PROJECT}/Src/system_stm32g4xx.c"
  "${ST_PROJECT}/Src/stm32g4xx_hal_msp.c" "${ST_PROJECT}/Src/stm32g4xx_it.c"
  "${ST_PROJECT}/Src/stm32g4xx_hal_timebase_tim.c"
  "${ST_PROJECT}/STM32CubeIDE/Application/Startup/startup_stm32g474retx.s"
  "${VIEWALYZER_RECORDER_DIR}/core/ViewAlyzer.c"
  "${VIEWALYZER_RECORDER_DIR}/core/viewalyzer_cobs.c"
  "${VIEWALYZER_RECORDER_DIR}/threadx/VA_Adapter_ThreadX.c")
foreach(module hal hal_cortex hal_rcc hal_rcc_ex hal_pwr hal_pwr_ex hal_flash hal_flash_ex
               hal_gpio hal_dma hal_dma_ex hal_tim hal_tim_ex hal_uart hal_uart_ex hal_exti)
  target_sources(threadx_recorder PRIVATE "${HAL}/Src/stm32g4xx_${module}.c")
endforeach()
target_include_directories(threadx_recorder PRIVATE ../common "${ST_PROJECT}" "${ST_PROJECT}/Inc"
  "${HAL}/Inc" "${HAL}/Inc/Legacy" "${THREADX_DIR}/common/inc" "${TX_PORT}/inc"
  "${CUBE_G4_DIR}/Drivers/CMSIS/Include"
  "${CUBE_G4_DIR}/Drivers/CMSIS/Device/ST/STM32G4xx/Include"
  "${VIEWALYZER_RECORDER_DIR}/core" "${VIEWALYZER_RECORDER_DIR}/threadx")
target_compile_definitions(threadx_recorder PRIVATE STM32G474xx USE_HAL_DRIVER
  TX_INCLUDE_USER_DEFINE_FILE VA_ENABLED=1 VA_RTOS_SELECT=3 VA_DEVICE_HEADER=stm32g474xx.h
  VA_CONFIG_HEADER=va_config.h VA_TRANSPORT=${VA_TRANSPORT})
target_compile_options(threadx_recorder PRIVATE -mcpu=cortex-m4 -mthumb
  -mfpu=fpv4-sp-d16 -mfloat-abi=hard -g3 -Og -ffunction-sections -fdata-sections
  $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wno-unused-parameter>)
target_link_options(threadx_recorder PRIVATE -mcpu=cortex-m4 -mthumb
  -mfpu=fpv4-sp-d16 -mfloat-abi=hard --specs=nano.specs --specs=nosys.specs
  "-T${ST_PROJECT}/STM32CubeIDE/STM32G474RETX_FLASH.ld"
  -Wl,--gc-sections "-Wl,-Map=${CMAKE_BINARY_DIR}/threadx_recorder.map")
set_target_properties(threadx_recorder PROPERTIES SUFFIX ".elf")
find_program(OBJCOPY arm-none-eabi-objcopy REQUIRED)
add_custom_command(TARGET threadx_recorder POST_BUILD COMMAND "${OBJCOPY}" -O ihex
  "$<TARGET_FILE:threadx_recorder>" "${CMAKE_BINARY_DIR}/threadx_recorder.hex")
