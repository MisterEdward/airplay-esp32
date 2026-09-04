set( SRC_FILES
    "main.c"
    "airplay_metrics.c"
    "alac_magic_cookie.c"
    "settings.c"
    "audio/audio_receiver.c"
    "audio/audio_stream.c"
    "audio/audio_stream_realtime.c"
    "audio/audio_stream_buffered.c"
    "audio/audio_decoder.c"
    "audio/audio_buffer.c"
    "audio/audio_timing.c"
    "audio/audio_crypto.c"
    "audio/audio_resample.c"
    "rtsp/rtsp_server.c"
    "rtsp/rtsp_conn.c"
    "rtsp/rtsp_message.c"
    "rtsp/rtsp_handlers.c"
    "rtsp/rtsp_fairplay.c"
    "rtsp/rtsp_crypto.c"
    "rtsp/rtsp_rsa.c"
    "rtsp/rtsp_events.c"
    "plist/plist_xml.c"
    "plist/bplist_parser.c"
    "plist/bplist_builder.c"
    "plist/base64.c"
    "hap/hap.c"
    "hap/hap_pair_verify.c"
    "hap/hap_pair_setup.c"
    "hap/hap_crypto.c"
    "hap/srp.c"
    "hap/tlv8.c"
    "network/wifi.c"
    "network/mdns_airplay.c"
    "network/ptp_clock.c"
    "network/ntp_clock.c"
    "network/socket_utils.c"
    "network/web_server.c"
    "network/ota.c"
    "network/dns_server.c"
    "network/log_stream.c"
    "led.c"
    "audio/eq_events.c"
    "buttons.c"
    "playback_control.c"
    "dacp_client.c"
)

# Select audio output implementation at compile time. audio_output_common.c
# carries weak defaults for the optional entry points, so a backend only has
# to implement what it actually supports.
list(APPEND SRC_FILES "audio/audio_output_common.c")
if(CONFIG_AUDIO_OUTPUT_SPDIF)
    list(APPEND SRC_FILES "audio/audio_output_spdif.c")
elseif(CONFIG_AUDIO_OUTPUT_USB)
    list(APPEND SRC_FILES "audio/audio_output_usb.c")
else()
    list(APPEND SRC_FILES "audio/audio_output.c")
endif()

# Bluetooth A2DP sink (only available on ESP32 with Classic BT)
if(CONFIG_BT_A2DP_ENABLE)
    list(APPEND SRC_FILES "audio/a2dp_sink.c")
    list(APPEND SRC_FILES "bt_coex.c")
endif()

if(CONFIG_USB_AUDIO_SOURCE)
    list(APPEND SRC_FILES
        "audio/usb_audio_source.c"
        "usb/usb_audio_descriptors.c"
    )
endif()

# W5500 SPI Ethernet
if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND SRC_FILES "network/ethernet.c")
endif()

set( INC_DIRS
    "." 
    "audio" 
    "rtsp" 
    "plist" 
    "hap" 
    "network"
)

set( DEPS
    driver
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_ringbuf
    esp_driver_i2s
    esp_timer
    esp_http_server
    esp_http_client
    app_update
    json
    mbedtls
    esp_driver_ledc
    boards
    dac
    dac_tas58xx
    dac_tas57xx
    display
    audio-resampler
    esp_eth
    spiffs_storage
)

# Bluetooth component only needed when A2DP is enabled
#if(CONFIG_BT_A2DP_ENABLE)
    list(APPEND DEPS bt)
#endif()

# Ethernet component only needed when W5500 is enabled
#if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND DEPS esp_eth)
#endif()

idf_component_register(
    SRCS ${SRC_FILES}
    INCLUDE_DIRS ${INC_DIRS}
    PRIV_REQUIRES ${DEPS}
)

if(CONFIG_USB_AUDIO_SOURCE)
    # In composite mode the application owns TinyUSB's configuration and
    # descriptors. Make the same UAC descriptor helpers used by Espressif's
    # component visible to both that component and TinyUSB itself.
    idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
    idf_component_get_property(uac_lib espressif__usb_device_uac COMPONENT_LIB)
    idf_component_get_property(uac_dir espressif__usb_device_uac COMPONENT_DIR)
    target_include_directories(${tusb_lib} PUBLIC
        "${COMPONENT_DIR}/usb"
        "${uac_dir}/tusb_uac"
    )
    target_include_directories(${uac_lib} PRIVATE
        "${COMPONENT_DIR}/usb"
        "${uac_dir}/tusb_uac"
    )
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${uac_dir}/tusb_uac"
    )

    # TinyUSB exports this definition while compiling its own component, but
    # application sources that include tusb.h do not inherit it through ESP-IDF's
    # private component dependency graph. Without it, TinyUSB selects the generic
    # FreeRTOS OSAL instead of Espressif's port.
    if(CONFIG_IDF_TARGET_ESP32S2)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE CFG_TUSB_MCU=OPT_MCU_ESP32S2)
    elseif(CONFIG_IDF_TARGET_ESP32S3)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE CFG_TUSB_MCU=OPT_MCU_ESP32S3)
    elseif(CONFIG_IDF_TARGET_ESP32P4)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE CFG_TUSB_MCU=OPT_MCU_ESP32P4)
    endif()
endif()

# Enable function wrapping for abort() to intercept panics and mute GPIO
if(CONFIG_BOARD_SQUEEZEAMP_COMMON)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=abort")
endif()
