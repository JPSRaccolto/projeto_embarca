# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

set(PICO_BOARD pico2_w CACHE STRING "Board type")



# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(AHT20 C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(AHT20 AHT20.c lib/aht20.c)

pico_set_program_name(AHT20 "AHT20")
pico_set_program_version(AHT20 "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(AHT20 1)
pico_enable_stdio_usb(AHT20 1)

# Add the standard library to the build
target_link_libraries(AHT20
        pico_stdlib
        hardware_i2c
        pico_cyw43_arch_lwip_threadsafe_background
        pico_lwip_mqtt
        pico_mbedtls
        pico_lwip_mbedtls
        )

# Add the standard include files to the build
target_include_directories(AHT20 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/lib
)


pico_add_extra_outputs(AHT20)

