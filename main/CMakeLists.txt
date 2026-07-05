idf_component_register(
    SRCS
        "hello_world_main.c"
        "si5351.c"
        "display.c"
        "input.c"
        "keyer.c"
        "morse.c"
        "websrv.c"
        "menu.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver              # legacy driver umbrella (gpio, pcnt, i2c, spi)
        esp_timer
        nvs_flash           # persist VFO state across power cycles
        esp_wifi            # WiFi station
        esp_http_server     # HTTP + WebSocket control page
        json                # cJSON for WS command parsing
        mdns                # vfo-keyer.local discovery
        u8g2                # managed component from nixy4/u8g2 (idf_component.yml)
)
