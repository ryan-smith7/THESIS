# # SPDX-License-Identifier: Apache-2.0
# cmake_minimum_required(VERSION 3.20.0)
# find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
# project(task1)

# # Automatically find all C source files in src/ and ../mycode/mylib/
# FILE(GLOB app_sources src/*.c)
# # FILE(GLOB lib_sources ../../mycode/mylib/s4744413_rtc.c)
# # FILE(GLOB lib_sources ../../mycode/mylib/s4744413_sensor.c)
# # FILE(GLOB lib_sources ../../mycode/mylib/*.c)
# # FILE(GLOB lib_sources ../../mycode/mylib/s4744413_rtc.c 
# #                     ../../mycode/mylib/s4744413_sensor.c
# #                     ../../mycode/mylib/s4744413_file.c)

# FILE(GLOB lib_sources  ../../mycode/mylib/s4744413_sensor.c)


# # Add source files to the target
# target_sources(app PRIVATE ${app_sources} ${lib_sources})

# # Include the ../mycode/mylib directory for headers
# target_include_directories(app PRIVATE ../../mycode/mylib)



cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(task1)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})