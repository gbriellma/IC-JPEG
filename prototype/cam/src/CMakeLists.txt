idf_component_register(
    SRCS "main.c" "../../common/experiment_protocol.c"
    INCLUDE_DIRS "." "../../common"
    REQUIRES driver esp_timer libimage esp32-camera
)
