cmake_minimum_required(VERSION 3.5)

set(PROJECT_NAME "esp32-web-debugger")

# 👉 Tell IDF to use your custom partitions.csv file
set(PROJECT_PARTITION_TABLE_CSV "${CMAKE_SOURCE_DIR}/partitions.csv")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(${PROJECT_NAME})

spiffs_create_partition_image(spiffs data FLASH_IN_PROJECT)
