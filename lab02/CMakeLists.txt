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
    message(FATAL_ERROR "Set FREERTOS_KERNEL_PATH to course V11.3.1 with RP2350 port")
endif()

project(eos_lab02 C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()
include(${_port})

foreach(opt EOS_SLOW_CONSUMER EOS_PROTECT_CONFIG EOS_ORDERED_LOCKS)
    option(${opt} "Enable the ${opt} experiment branch" OFF)
endforeach()
foreach(opt EOS_QUEUE_DEPTH)
    set(${opt} 4 CACHE STRING "Number of copied records buffered in mode 1")
endforeach()
if(NOT EOS_QUEUE_DEPTH MATCHES "^[1-9][0-9]*$" OR EOS_QUEUE_DEPTH GREATER 64)
    message(FATAL_ERROR "EOS_QUEUE_DEPTH must be an integer from 1 to 64")
endif()
set(L2_MAIN_SOURCE "${CMAKE_CURRENT_LIST_DIR}/main.c"
    CACHE FILEPATH "Student or private reference source")

# Literal target declarations also let the shared picolab CLI discover each mode.
add_executable(lab02_mode1 "${L2_MAIN_SOURCE}")
add_executable(lab02_mode2 "${L2_MAIN_SOURCE}")
add_executable(lab02_mode3 "${L2_MAIN_SOURCE}")
add_executable(lab02_mode4 "${L2_MAIN_SOURCE}")

foreach(mode RANGE 1 4)
    target_include_directories(lab02_mode${mode} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(lab02_mode${mode} PRIVATE EOS_LAB_MODE=${mode}
        EOS_QUEUE_DEPTH=${EOS_QUEUE_DEPTH}
        EOS_SLOW_CONSUMER=$<BOOL:${EOS_SLOW_CONSUMER}>
        EOS_PROTECT_CONFIG=$<BOOL:${EOS_PROTECT_CONFIG}>
        EOS_ORDERED_LOCKS=$<BOOL:${EOS_ORDERED_LOCKS}>)
    target_link_libraries(lab02_mode${mode}
        pico_stdlib hardware_clocks FreeRTOS-Kernel-Heap4)
    pico_enable_stdio_usb(lab02_mode${mode} 0)
    pico_enable_stdio_uart(lab02_mode${mode} 1)
    pico_add_extra_outputs(lab02_mode${mode})
endforeach()
