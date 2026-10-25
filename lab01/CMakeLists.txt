cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2_w CACHE STRING "Course target")

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

set(FREERTOS_KERNEL_PATH "$ENV{FREERTOS_KERNEL_PATH}")
set(_ports "${FREERTOS_KERNEL_PATH}/portable/ThirdParty")
set(_import "GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake")
set(_port "${_ports}/Community-Supported-Ports/${_import}")

if(NOT EXISTS "${_port}")
    set(_port "${_ports}/${_import}")
endif()

if(NOT EXISTS "${_port}")
    message(FATAL_ERROR
        "Set FREERTOS_KERNEL_PATH to course V11.3.1, including its RP2350 port")
endif()

project(eos_lab01 C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()
include(${_port})

foreach(opt EOS_EXPLICIT_YIELD EOS_ABSOLUTE_RELEASE)
    option(${opt} "Enable the ${opt} experiment branch" OFF)
endforeach()
set(L1_MAIN_SOURCE "${CMAKE_CURRENT_LIST_DIR}/main.c"
    CACHE FILEPATH "Student or private reference source")

# Literal target declarations also let the shared picolab CLI discover each mode.
add_executable(lab01_calibration calibration.c)
target_link_libraries(lab01_calibration pico_stdlib hardware_clocks)
pico_enable_stdio_usb(lab01_calibration 0)
pico_enable_stdio_uart(lab01_calibration 1)
pico_add_extra_outputs(lab01_calibration)

add_executable(lab01_mode1 "${L1_MAIN_SOURCE}")
add_executable(lab01_mode2 "${L1_MAIN_SOURCE}")
add_executable(lab01_mode3 "${L1_MAIN_SOURCE}")
add_executable(lab01_mode4 "${L1_MAIN_SOURCE}")

foreach(mode RANGE 1 4)
    target_include_directories(lab01_mode${mode} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(lab01_mode${mode} PRIVATE EOS_LAB_MODE=${mode}
        EOS_EXPLICIT_YIELD=$<BOOL:${EOS_EXPLICIT_YIELD}>
        EOS_ABSOLUTE_RELEASE=$<BOOL:${EOS_ABSOLUTE_RELEASE}>)
    target_link_libraries(lab01_mode${mode}
        pico_stdlib hardware_clocks FreeRTOS-Kernel-Heap4)
    pico_enable_stdio_usb(lab01_mode${mode} 0)
    pico_enable_stdio_uart(lab01_mode${mode} 1)
    pico_add_extra_outputs(lab01_mode${mode})
endforeach()
