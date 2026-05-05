if(CONFIG_DAC_ES8388)
    set(srcs "dac_es8388.c")
else()
    set(srcs "")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES dac esp_codec_dev
    PRIV_REQUIRES esp_driver_i2c
)
