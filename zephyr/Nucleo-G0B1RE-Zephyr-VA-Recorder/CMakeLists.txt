# ViewAlyzer on the ST NUCLEO-G0B1RE (Cortex-M0+).
# The recorder is pulled in as a Zephyr extra module from this repo's
# shared ViewAlyzerRecorder tree - no sources are copied.
cmake_minimum_required(VERSION 3.20.0)

# ── Locate the ViewAlyzer Recorder (Zephyr module) ────────────────────
# Resolution order:
#   1. -DVIEWALYZER_MODULE_PATH=<path> on the west/cmake command line
#   2. The VIEWALYZER_MODULE_PATH environment variable
#   3. A ViewAlyzer checkout cloned next to this examples repo:
#      <workspace>/ViewAlyzer/ViewAlyzerRecorder
if(NOT DEFINED VIEWALYZER_MODULE_PATH)
  if(DEFINED ENV{VIEWALYZER_MODULE_PATH})
    set(VIEWALYZER_MODULE_PATH "$ENV{VIEWALYZER_MODULE_PATH}")
  else()
    set(VIEWALYZER_MODULE_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../ViewAlyzer/ViewAlyzerRecorder")
  endif()
endif()
get_filename_component(VIEWALYZER_MODULE_PATH "${VIEWALYZER_MODULE_PATH}" ABSOLUTE)
if(NOT EXISTS "${VIEWALYZER_MODULE_PATH}/zephyr/module.yml")
  message(FATAL_ERROR
    "ViewAlyzer Recorder not found at: ${VIEWALYZER_MODULE_PATH}\n"
    "Clone the ViewAlyzer repo next to this examples repo, or point "
    "VIEWALYZER_MODULE_PATH (CMake option or environment variable) at a "
    "ViewAlyzerRecorder directory.")
endif()
list(APPEND ZEPHYR_EXTRA_MODULES ${VIEWALYZER_MODULE_PATH})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nucleo-g0b1re-va-recorder)

target_sources(app PRIVATE src/main.c)
