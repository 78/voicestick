idf_component_register(
    SRCS "voice_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update bt nvs_flash board esp_timer
)

target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ble_hs_hci_util_set_data_len")
