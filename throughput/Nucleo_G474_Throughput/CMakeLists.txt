cmake_minimum_required(VERSION 3.22)

# ── Nucleo-G474RE ViewAlyzer throughput benchmark (bare-metal) ──
# Transport switch (on-board ST-LINK):
#   -DVA_TRANSPORT=RAM_BUFFER  (default) probe memory-read RAM ring
#   -DVA_TRANSPORT=ARM_ITM     ITM / SWO trace pin
set(VA_TRANSPORT "RAM_BUFFER" CACHE STRING "ViewAlyzer transport backend (RAM_BUFFER | ARM_ITM)")

# Offered-load ceiling in KiB/s (0 = unbounded blast).
set(TP_TARGET_KBPS "8000" CACHE STRING "Throughput benchmark offered load in KiB/s (0 = unbounded)")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

set(CMAKE_PROJECT_NAME Nucleo_G474_Throughput)
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

set(HAL_DIR Drivers/STM32G4xx_HAL_Driver)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    startup_stm32g474xx.s
    Core/Src/system_stm32g4xx.c
    Core/Src/syscalls.c
    Core/Src/sysmem.c
    Core/Src/main.c
    # HAL subset: clock bring-up only (RCC/PWR/FLASH + core)
    ${HAL_DIR}/Src/stm32g4xx_hal.c
    ${HAL_DIR}/Src/stm32g4xx_hal_cortex.c
    ${HAL_DIR}/Src/stm32g4xx_hal_rcc.c
    ${HAL_DIR}/Src/stm32g4xx_hal_rcc_ex.c
    ${HAL_DIR}/Src/stm32g4xx_hal_pwr.c
    ${HAL_DIR}/Src/stm32g4xx_hal_pwr_ex.c
    ${HAL_DIR}/Src/stm32g4xx_hal_flash.c
    ${HAL_DIR}/Src/stm32g4xx_hal_flash_ex.c
    ${HAL_DIR}/Src/stm32g4xx_hal_flash_ramfunc.c
    ${HAL_DIR}/Src/stm32g4xx_hal_exti.c
    ${VIEWALYZER_RECORDER_DIR}/core/ViewAlyzer.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Inc
    Drivers/CMSIS/Include
    Drivers/CMSIS/Device/ST/STM32G4xx/Include
    ${HAL_DIR}/Inc
    ${HAL_DIR}/Inc/Legacy
    ${VIEWALYZER_RECORDER_DIR}/core
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    STM32G474xx
    USE_HAL_DRIVER
    VA_ENABLED=1
    VA_RTOS_SELECT=0
    VA_DEVICE_HEADER=stm32g4xx.h
    VA_TRANSPORT=${VA_TRANSPORT}
    VA_TP_TEST=1
    TP_TARGET_KBPS=${TP_TARGET_KBPS}u
)

target_link_libraries(${CMAKE_PROJECT_NAME} m)
