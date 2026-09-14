cmake_minimum_required(VERSION 3.24)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" VOICESTICK_VERSION_STRING LIMIT_COUNT 1)
string(STRIP "${VOICESTICK_VERSION_STRING}" VOICESTICK_VERSION_STRING)
project(VoiceStickLinux VERSION ${VOICESTICK_VERSION_STRING} LANGUAGES C CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(ATSPI REQUIRED IMPORTED_TARGET atspi-2)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XEXT REQUIRED IMPORTED_TARGET xext)
pkg_check_modules(XTST REQUIRED IMPORTED_TARGET xtst)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
find_package(OpenSSL REQUIRED)

configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)

add_library(voicestick_core
    src/app_config.cc
    src/asr_protocol.cc
    src/ble_protocol.cc
    src/debug_audio_recorder.cc
    src/firmware_manifest.cc
    src/http_client_linux.cc
    src/llm_translation_client.cc
    third_party/cjson/cJSON.c
    src/log.cc
    src/ogg_crc.cc
    src/ogg_opus_muxer.cc
    src/voice_stick_coordinator.cc
)

target_include_directories(voicestick_core PUBLIC src third_party/cjson third_party/tomlplusplus)
target_link_libraries(voicestick_core PUBLIC PkgConfig::SOUP OpenSSL::Crypto)

add_executable(VoiceStickApp
    src/main.cc
    src/gtk4_app.cc
    src/status_item_sni.cc
    src/ui_dispatch.cc
    src/asr_client_linux.cc
    src/ble_central_bluez.cc
    src/input_injector_linux.cc
    src/voice_stick_cloud_api_linux.cc
    src/overlay_window.cc
    src/subtitle_window.cc
    src/pair_device_dialog.cc
    src/onboarding_dialog.cc
    src/preferences_window.cc
    src/firmware_update_dialog.cc
)

target_include_directories(VoiceStickApp PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_target_properties(VoiceStickApp PROPERTIES OUTPUT_NAME VoiceStick)
target_link_libraries(VoiceStickApp PRIVATE
    voicestick_core
    PkgConfig::GTK4
    PkgConfig::ADWAITA
    PkgConfig::GIO
    PkgConfig::ATSPI
    PkgConfig::X11
    PkgConfig::XEXT
    PkgConfig::XTST
    PkgConfig::PANGOCAIRO
)

add_executable(voicestick_linux_tests
    tests/core_tests.cc
)
target_link_libraries(voicestick_linux_tests PRIVATE voicestick_core)

enable_testing()
add_test(NAME voicestick_linux_tests COMMAND voicestick_linux_tests)

install(TARGETS VoiceStickApp RUNTIME DESTINATION bin)
install(FILES resources/me.voicestick.VoiceStick.desktop DESTINATION share/applications)
