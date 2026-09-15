idf_component_register(SRCS "main.cpp" "rpc.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp-tls nghttp json driver esp_http_server)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
