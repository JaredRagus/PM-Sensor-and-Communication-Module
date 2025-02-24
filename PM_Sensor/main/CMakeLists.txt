idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES pmsa003i esp_wifi freertos nvs_flash mqtt
)
                       
