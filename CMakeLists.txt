# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

set(SDKCONFIG_DEFAULTS "sdkconfig.175c")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "1.0.15")
project(xiaozhi)

if(CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75C)
    set(CONTENT_SEED_DIR "${CMAKE_BINARY_DIR}/content-seed")
    file(MAKE_DIRECTORY "${CONTENT_SEED_DIR}")
    fatfs_create_spiflash_image(content "${CONTENT_SEED_DIR}")
endif()
