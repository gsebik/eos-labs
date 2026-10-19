cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2_w CACHE STRING "Course target")
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
set(FREERTOS_KERNEL_PATH "$ENV{FREERTOS_KERNEL_PATH}")
set(_ports "${FREERTOS_KERNEL_PATH}/portable/ThirdParty")
set(_tail "GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake")
set(_port "${_ports}/Community-Supported-Ports/${_tail}")
if(NOT EXISTS "${_port}")
    set(_port "${_ports}/${_tail}")
endif()
if(NOT EXISTS "${_port}")
    message(FATAL_ERROR "Set FREERTOS_KERNEL_PATH to V11.3.1 with its RP2350 port")
endif()
project(eos_lab03_instrument C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()
include(${_port})

option(EOS_SIG_SETTINGS_MUTEX "LEGACY, unused: settings are always a mutex" ON)
option(EOS_INHERITANCE_MUTEX "Use an inheritance mutex in the priority-inheritance demo" ON)
option(EOS_LONG_RESULT_LOCK "ON: hold the spectrum mutex across the redraw (comparison)" OFF)
set(EOS_DEADLINE_US 10240 CACHE STRING "Block ready-to-release deadline in microseconds")
set(EOS_DSP_REPEATS 4 CACHE STRING "Extra block-processing repeats")
set(EOS_DISPLAY_ROWS 40 CACHE STRING "Plot height and per-band pixel-transfer budget")
set(EOS_DISPLAY_PERIOD_MS 100 CACHE STRING "Display redraw period")
set(EOS_UI_PERIOD_MS 20 CACHE STRING "Settings update period")
set(EOS_SETTINGS_ITERS 2000 CACHE STRING "LEGACY, unused: no artificial settings workload")
# This literal list is also the picolab build-knob discovery interface. It must
# stay on ONE line: picolab discovers the option names with a line-based search,
# so a wrapped list makes it warn that valid options are typos.
foreach(opt EOS_DEADLINE_US EOS_DSP_REPEATS EOS_DISPLAY_ROWS EOS_DISPLAY_PERIOD_MS EOS_UI_PERIOD_MS EOS_SETTINGS_ITERS)
    if(NOT "${${opt}}" MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "${opt} must be a positive integer")
    endif()
endforeach()
if(EOS_DEADLINE_US GREATER 1000000 OR EOS_DSP_REPEATS GREATER 64
   OR EOS_DISPLAY_ROWS GREATER 480 OR EOS_DISPLAY_PERIOD_MS GREATER 10000
   OR EOS_UI_PERIOD_MS GREATER 10000 OR EOS_SETTINGS_ITERS GREATER 3000000)
    message(FATAL_ERROR "One or more experiment settings exceed their limit")
endif()
foreach(opt EOS_SIG_SETTINGS_MUTEX EOS_LONG_RESULT_LOCK EOS_INHERITANCE_MUTEX)
    message(STATUS "${opt}=${${opt}}")
endforeach()

set(SIG_MAIN_SOURCE "${CMAKE_CURRENT_LIST_DIR}/main.c"
    CACHE FILEPATH "Student or private reference source")
add_executable(lab03_instrument_rtos "${SIG_MAIN_SOURCE}")
add_executable(lab03_instrument_foreground "${SIG_MAIN_SOURCE}")
foreach(target lab03_instrument_rtos lab03_instrument_foreground)
    target_sources(${target} PRIVATE capture.c pool.c workload.c panel.c trace.c)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(${target} PRIVATE
        EOS_SIG_RTOS=$<STREQUAL:${target},lab03_instrument_rtos>
        EOS_SIG_SETTINGS_MUTEX=$<BOOL:${EOS_SIG_SETTINGS_MUTEX}>
        EOS_LONG_RESULT_LOCK=$<BOOL:${EOS_LONG_RESULT_LOCK}>
        EOS_DEADLINE_US=${EOS_DEADLINE_US}
        EOS_DSP_REPEATS=${EOS_DSP_REPEATS}
        EOS_DISPLAY_ROWS=${EOS_DISPLAY_ROWS}
        EOS_DISPLAY_PERIOD_MS=${EOS_DISPLAY_PERIOD_MS}
        EOS_UI_PERIOD_MS=${EOS_UI_PERIOD_MS}
        EOS_SETTINGS_ITERS=${EOS_SETTINGS_ITERS})
    target_link_libraries(${target} pico_stdlib hardware_adc hardware_dma
        hardware_pwm hardware_spi hardware_clocks m)
    if(target STREQUAL "lab03_instrument_rtos")
        target_link_libraries(${target} FreeRTOS-Kernel-Heap4)
    endif()
    pico_enable_stdio_usb(${target} 0)
    pico_enable_stdio_uart(${target} 1)
    pico_add_extra_outputs(${target})
endforeach()

# Separate demonstration program for Task 2: three tasks, one resource. It does
# not share the instrument's acquisition, FFT or display code.
add_executable(lab03_priority_inheritance "${CMAKE_CURRENT_LIST_DIR}/priority_inheritance.c")
target_include_directories(lab03_priority_inheritance PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(lab03_priority_inheritance PRIVATE
    EOS_INHERITANCE_MUTEX=$<BOOL:${EOS_INHERITANCE_MUTEX}>)
target_link_libraries(lab03_priority_inheritance pico_stdlib FreeRTOS-Kernel-Heap4)
pico_enable_stdio_usb(lab03_priority_inheritance 0)
pico_enable_stdio_uart(lab03_priority_inheritance 1)
pico_add_extra_outputs(lab03_priority_inheritance)
