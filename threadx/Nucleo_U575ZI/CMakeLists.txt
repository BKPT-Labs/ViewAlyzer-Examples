# Copyright 2025-2026 BKPT, Inc.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
find_program(CMAKE_C_COMPILER arm-none-eabi-gcc REQUIRED)
set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
project(threadx_recorder C ASM)
foreach(path CUBE_U5_DIR THREADX_DIR VIEWALYZER_RECORDER_DIR)
  if(NOT IS_DIRECTORY "${${path}}")
    message(FATAL_ERROR "Supply -D${path}=<source-directory>; build.py fetches pinned dependencies")
  endif()
endforeach()
set(VA_TRANSPORT 4 CACHE STRING "1=ITM/SWO, 4=ViewAlyzer RAM buffer")
option(VA_U575_TRUSTZONE "Boot ThreadX as the NonSecure partner of ST's GTZC example" OFF)
set(ST_PROJECT "${CUBE_U5_DIR}/Projects/NUCLEO-U575ZI-Q/Applications/ThreadX/Tx_Thread_Creation")
set(ST_GTZC "${CUBE_U5_DIR}/Projects/NUCLEO-U575ZI-Q/Examples/GTZC/GTZC_TZSC_MPCBB_TrustZone")
set(HAL "${CUBE_U5_DIR}/Drivers/STM32U5xx_HAL_Driver")
set(NS_SYSTEM "${ST_PROJECT}/Src/system_stm32u5xx.c")
set(NS_LINKER "${ST_PROJECT}/STM32CubeIDE/STM32U575ZITXQ_FLASH.ld")
set(NS_STARTUP "${ST_PROJECT}/STM32CubeIDE/Application/Startup/startup_stm32u575zitxq.s")
if(VA_U575_TRUSTZONE)
  set(NS_SYSTEM "${ST_GTZC}/NonSecure/Src/system_stm32u5xx_ns.c")
  set(NS_LINKER "${ST_GTZC}/STM32CubeIDE/NonSecure/STM32U575ZITXQ_FLASH.ld")
  set(NS_STARTUP "${ST_GTZC}/STM32CubeIDE/NonSecure/Application/Startup/startup_stm32u575zitxq.s")
endif()
set(TX_PORT "${THREADX_DIR}/ports/cortex_m33/gnu")
file(GLOB TX_COMMON CONFIGURE_DEPENDS "${THREADX_DIR}/common/src/*.c")
file(GLOB TX_ASM CONFIGURE_DEPENDS "${TX_PORT}/src/*.S")
list(FILTER TX_ASM EXCLUDE REGEX "/tx_misra\\.S$")
# ST's kernel port also ships a generic low-level sample. The board donor
# supplies the actual vector symbols and 160 MHz SysTick configuration.
list(FILTER TX_ASM EXCLUDE REGEX "/tx_initialize_low_level\\.S$")
add_executable(threadx_recorder ../common/workload.c ${TX_COMMON} ${TX_ASM}
  "${ST_PROJECT}/Src/main.c" "${NS_SYSTEM}"
  "${ST_PROJECT}/Src/stm32u5xx_hal_msp.c" "${ST_PROJECT}/Src/stm32u5xx_it.c"
  "${ST_PROJECT}/Src/stm32u5xx_hal_timebase_tim.c" "${ST_PROJECT}/Src/tx_initialize_low_level.S"
  "${NS_STARTUP}"
  "${VIEWALYZER_RECORDER_DIR}/core/ViewAlyzer.c"
  "${VIEWALYZER_RECORDER_DIR}/core/viewalyzer_cobs.c"
  "${VIEWALYZER_RECORDER_DIR}/threadx/VA_Adapter_ThreadX.c")
foreach(module hal hal_cortex hal_rcc hal_rcc_ex hal_pwr hal_pwr_ex hal_flash hal_flash_ex
               hal_gpio hal_tim hal_tim_ex hal_icache)
  target_sources(threadx_recorder PRIVATE "${HAL}/Src/stm32u5xx_${module}.c")
endforeach()
set_source_files_properties("${ST_PROJECT}/Src/tx_initialize_low_level.S"
  PROPERTIES COMPILE_OPTIONS "-include;tx_user.h")
target_include_directories(threadx_recorder PRIVATE ../common "${ST_PROJECT}/Inc"
  "${HAL}/Inc" "${HAL}/Inc/Legacy" "${THREADX_DIR}/common/inc" "${TX_PORT}/inc"
  "${CUBE_U5_DIR}/Drivers/CMSIS/Include"
  "${CUBE_U5_DIR}/Drivers/CMSIS/Device/ST/STM32U5xx/Include"
  "${VIEWALYZER_RECORDER_DIR}/core" "${VIEWALYZER_RECORDER_DIR}/threadx")
target_compile_definitions(threadx_recorder PRIVATE STM32U575xx USE_HAL_DRIVER
  TX_SINGLE_MODE_NON_SECURE=1 TX_INCLUDE_USER_DEFINE_FILE
  VA_ENABLED=1 VA_RTOS_SELECT=3 VA_DEVICE_HEADER=stm32u575xx.h
  VA_CONFIG_HEADER=va_config.h VA_TRANSPORT=${VA_TRANSPORT})
target_compile_options(threadx_recorder PRIVATE -mcpu=cortex-m33 -mthumb
  -mfpu=fpv5-sp-d16 -mfloat-abi=hard -g3 -Og -ffunction-sections -fdata-sections
  $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wno-unused-parameter>)
target_link_options(threadx_recorder PRIVATE -mcpu=cortex-m33 -mthumb
  -mfpu=fpv5-sp-d16 -mfloat-abi=hard --specs=nano.specs --specs=nosys.specs
  "-T${NS_LINKER}"
  -Wl,--gc-sections "-Wl,-Map=${CMAKE_BINARY_DIR}/threadx_recorder.map")
set_target_properties(threadx_recorder PROPERTIES SUFFIX ".elf")
find_program(OBJCOPY arm-none-eabi-objcopy REQUIRED)
add_custom_command(TARGET threadx_recorder POST_BUILD COMMAND "${OBJCOPY}" -O ihex
  "$<TARGET_FILE:threadx_recorder>" "${CMAKE_BINARY_DIR}/threadx_recorder.hex")

if(VA_U575_TRUSTZONE)
  add_executable(threadx_secure_boot secure_boot.c
    "${ST_GTZC}/Secure/Src/secure_nsc.c"
    "${ST_GTZC}/Secure/Src/system_stm32u5xx_s.c"
    "${ST_GTZC}/Secure/Src/stm32u5xx_hal_msp.c"
    "${ST_GTZC}/Secure/Src/stm32u5xx_it.c"
    "${ST_GTZC}/STM32CubeIDE/Secure/Application/Startup/startup_stm32u575zitxq.s"
    "${CUBE_U5_DIR}/Drivers/BSP/STM32U5xx_Nucleo/stm32u5xx_nucleo.c")
  foreach(module hal hal_cortex hal_dma hal_dma_ex hal_exti hal_flash hal_flash_ex
                 hal_gpio hal_gtzc hal_icache hal_pwr hal_pwr_ex hal_rcc hal_rcc_ex)
    target_sources(threadx_secure_boot PRIVATE "${HAL}/Src/stm32u5xx_${module}.c")
  endforeach()
  target_include_directories(threadx_secure_boot PRIVATE "${ST_GTZC}"
    "${ST_GTZC}/Secure/Inc" "${ST_GTZC}/Secure_nsclib" "${HAL}/Inc" "${HAL}/Inc/Legacy"
    "${CUBE_U5_DIR}/Drivers/CMSIS/Include"
    "${CUBE_U5_DIR}/Drivers/CMSIS/Device/ST/STM32U5xx/Include"
    "${CUBE_U5_DIR}/Drivers/BSP/STM32U5xx_Nucleo")
  target_compile_definitions(threadx_secure_boot PRIVATE STM32U575xx USE_HAL_DRIVER)
  target_compile_options(threadx_secure_boot PRIVATE -mcpu=cortex-m33 -mthumb -mcmse
    -mfpu=fpv5-sp-d16 -mfloat-abi=hard -g3 -Og -ffunction-sections -fdata-sections
    $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wno-unused-parameter>)
  target_link_options(threadx_secure_boot PRIVATE -mcpu=cortex-m33 -mthumb -mcmse
    -mfpu=fpv5-sp-d16 -mfloat-abi=hard --specs=nano.specs --specs=nosys.specs
    "-T${ST_GTZC}/STM32CubeIDE/Secure/STM32U575ZITXQ_FLASH.ld"
    -Wl,--gc-sections -Wl,--cmse-implib
    "-Wl,--out-implib=${CMAKE_BINARY_DIR}/secure_nsclib.o"
    "-Wl,-Map=${CMAKE_BINARY_DIR}/threadx_secure_boot.map")
  set_target_properties(threadx_secure_boot PROPERTIES SUFFIX ".elf")
  add_custom_command(TARGET threadx_secure_boot POST_BUILD COMMAND "${OBJCOPY}" -O ihex
    "$<TARGET_FILE:threadx_secure_boot>" "${CMAKE_BINARY_DIR}/threadx_secure_boot.hex"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/secure_nsclib.o")
  add_dependencies(threadx_recorder threadx_secure_boot)
  target_link_libraries(threadx_recorder PRIVATE "${CMAKE_BINARY_DIR}/secure_nsclib.o")
endif()
