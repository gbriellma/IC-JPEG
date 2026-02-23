FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_server esp_event nvs_flash esp_netif
              esp_driver_ledc driver
    PRIV_REQUIRES esp_timer
)
