# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# Ensure lwIP exposes netif callback APIs used by mmipal
idf_build_set_property(COMPILE_DEFINITIONS "LWIP_NETIF_LINK_CALLBACK=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LWIP_NETIF_STATUS_CALLBACK=1" APPEND)

project(edge-device-esp32)
