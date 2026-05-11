cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gateway_wifi)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
)

target_sources(app PRIVATE
    src/main.c
    src/wifi.c
    ../common/src/azure_mqtt.c
    ../common/src/bluetooth.c
    ../common/src/my_json.c
    ../common/src/sntp_sync.c
    ../common/src/time_sync_writer.c
    ../common/src/http_time_sync.c
)