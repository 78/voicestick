cmake_minimum_required(VERSION 3.16)

# One firmware, several boards. A board is a directory under boards/ with
#   board.cmake          sets BOARD_TARGET (the chip)
#   sdkconfig.defaults   layered on top of ./sdkconfig.defaults
#   components/board     implements components/board_api
# Every build directory keeps its own sdkconfig, so boards build side by side:
#   idf.py build                                      # m5stack_stick_s3 -> build/
#   idf.py -B build-c3 -DBOARD=voicestick_c3 build    # VoiceStick C3 v1
set(BOARD "m5stack_stick_s3" CACHE STRING "Board directory under boards/")
if(BOARD STREQUAL "m5stick_s3")
    # Migrate build directories created before the StickS3 board was renamed.
    set(BOARD "m5stack_stick_s3" CACHE STRING "Board directory under boards/" FORCE)
endif()
set(BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}")
if(NOT EXISTS "${BOARD_DIR}/board.cmake")
    message(FATAL_ERROR "Unknown BOARD '${BOARD}': ${BOARD_DIR}/board.cmake not found")
endif()
include("${BOARD_DIR}/board.cmake")

set(IDF_TARGET "${BOARD_TARGET}")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${BOARD_DIR}/sdkconfig.defaults")
set(EXTRA_COMPONENT_DIRS "${BOARD_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${BOARD}")
project(voice_stick)
