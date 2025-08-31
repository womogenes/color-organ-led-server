idf_component_register(SRCS "hello_world_main.c"
                       PRIV_REQUIRES esp_driver_ledc esp_driver_gpio esp_http_server esp_wifi nvs_flash esp_eth protocol_examples_common esp_timer
                       INCLUDE_DIRS "")
