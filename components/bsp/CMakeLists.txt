


idf_component_register(
    SRC_DIRS
        "src/boards"
        "src/peripherals"
        "src/storage"
        "src/indev"
        "src/codec"
    INCLUDE_DIRS
        "src/boards"
        "src"
        "include"
    REQUIRES
        iot_button
        esp_hid
        esp_lcd
        fatfs
        i2c_bus
        i2c_devices
        spiffs)
