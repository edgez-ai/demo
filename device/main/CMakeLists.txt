# Common sources
set(COMPONENT_SRCS "main.c" "sampling.c" "proto/factory_data.pb.c")
set(COMPONENT_INCLUDE_DIRS "." "driver/include" "proto")
set(COMPONENT_REQUIRES "")


# ESP32 platform
set(PLATFORM_SRC_DIR "driver/esp32")

# Add platform-specific sources
file(GLOB PLATFORM_SRCS "${PLATFORM_SRC_DIR}/*.c")
list(APPEND COMPONENT_SRCS ${PLATFORM_SRCS})

# Add morse-micro sources
file(GLOB MORSE_MICRO_SRCS "driver/morse-micro/*.c")
list(APPEND COMPONENT_SRCS ${MORSE_MICRO_SRCS})

# Add LwM2M object sources (button/temp/humidity/etc.)
file(GLOB LWM2M_SRCS "lwm2m/*.c")
list(APPEND COMPONENT_SRCS ${LWM2M_SRCS})

# Add ESP32-specific include dirs
list(APPEND COMPONENT_INCLUDE_DIRS
"../components/wakaama/examples/client"
"../components/wakaama/include"
"../components/wakaama/core"
"../components/wakaama/coap"
"driver/morse-micro"
"lwm2m")

# ESP32-specific requirements
set(COMPONENT_REQUIRES driver wakaama espressif__qrcode wifi_provisioning bt
                    nvs_flash mbedtls morselib mm_shims mmipal mmutils esp_psram nanopb
                    esp_adc)

# Register component (required for all platforms)
idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS ${COMPONENT_INCLUDE_DIRS}
    REQUIRES ${COMPONENT_REQUIRES}
)
