cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gateway)

include_directories(include)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

zephyr_linker_sources(RAM_SECTIONS ${CMAKE_CURRENT_SOURCE_DIR}/app_dram1.ld)