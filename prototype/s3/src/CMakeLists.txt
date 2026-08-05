idf_component_register(
    SRCS "main.c" "tft.c" "sd_log.c" "metrics.c" "../../common/experiment_protocol.c"
    INCLUDE_DIRS "." "../../common"
    REQUIRES driver esp_timer libimage fatfs sdmmc led_strip mbedtls
)
