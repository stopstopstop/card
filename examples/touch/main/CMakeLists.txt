set(include_dirs "." "./gui" "./app")
set(src_dirs "." "./gui/image" "./gui/font" "./gui" "./app")

idf_component_register(SRC_DIRS "${src_dirs}"
                        INCLUDE_DIRS "${include_dirs}"
                        PRIV_REQUIRES nvs_flash spiffs driver esp_timer lvgl led_strip bsp 
)
target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-D RMAKER_DEMO_PROJECT_NAME=\"${CMAKE_PROJECT_NAME}\"")
