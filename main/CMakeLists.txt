idf_component_register(SRCS "http_server.c" "camera_encoder.c" "camera_encoder_common.c" "camera_pattern.c" "camera_drawer.c" "camera.c" "main.c" "rtsp_server.c" "font.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_eth esp_event esp_netif lwip esp_video esp_http_server json
                       PRIV_REQUIRES esp_driver_gpio esp_h264)
