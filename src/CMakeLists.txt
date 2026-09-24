idf_component_register(SRCS "main.cpp" "rpc.cpp" "request_display.cpp" "edge_dns.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp-tls nghttp json driver esp_driver_tsens esp_http_server esp_timer esp_system spi_flash)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
