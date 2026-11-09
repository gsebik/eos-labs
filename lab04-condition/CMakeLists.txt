cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2_w CACHE STRING "Board type")
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

if(NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message(FATAL_ERROR "Set FREERTOS_KERNEL_PATH to a FreeRTOS V11.3.1 checkout")
endif()
set(FREERTOS_KERNEL_PATH $ENV{FREERTOS_KERNEL_PATH})
set(_freertos_import "${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake")
if(NOT EXISTS "${_freertos_import}")
    set(_freertos_import "${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake")
endif()
if(NOT EXISTS "${_freertos_import}")
    message(FATAL_ERROR "The configured FreeRTOS checkout has no RP2350 ARM NTZ port: ${FREERTOS_KERNEL_PATH}")
endif()
include(${_freertos_import})

set(EOS_CORES 1 CACHE STRING "1 = single core baseline, 2 = SMP")
set_property(CACHE EOS_CORES PROPERTY STRINGS 1 2)
set(EOS_PLACEMENT 0 CACHE STRING "0 = none, 1 = SMP global, 2 = SMP partitioned")
set_property(CACHE EOS_PLACEMENT PROPERTY STRINGS 0 1 2)
set(EOS_TARGET_KHZ 150000 CACHE STRING "Target clk_sys in kHz")
set(EOS_FFT_SIZE 256 CACHE STRING "FFT block length: 256, 512 or 1024")
set_property(CACHE EOS_FFT_SIZE PROPERTY STRINGS 256 512 1024)
math(EXPR _eos_default_deadline "${EOS_FFT_SIZE} * 1000000 / 50000")
math(EXPR _eos_default_buffer_deadline "${_eos_default_deadline} * 2")
set(EOS_DSP_REPEATS 4 CACHE STRING "Extra FFT arithmetic passes")
set(EOS_DISPLAY_ROWS 40 CACHE STRING "Plot height and per-band transfer budget")
set(EOS_DISPLAY_PERIOD_MS 100 CACHE STRING "Display redraw period")
set(EOS_UI_PERIOD_MS 20 CACHE STRING "Settings update period")
set(EOS_ALARM_PERIOD_MS 20 CACHE STRING "Alarm task period in ms")
set(EOS_ALARM_DEADLINE_US 2000 CACHE STRING "Alarm deadline in us")
set(EOS_ALARM_PRIORITY 4 CACHE STRING "Alarm task priority")
set(EOS_SEED_SMP_BUG 0 CACHE STRING "1 = optional unsafe unlocked-counter demonstration; off for measured lab route")
set(EOS_DSP_RAM 0 CACHE STRING "1 = place sig_dsp_process in SRAM")
set(EOS_SCHED_MODE 0 CACHE STRING "1 = build A/B/C scheduling mode in lab04_condition")
set(EOS_SCHED_PLACEMENT 0 CACHE STRING "Scheduler preset: 0 single, 1 partitioned, 2 global")
set(EOS_SCHED_DHALL 0 CACHE STRING "1 = Dhall-style A/B/C set")
set(EOS_SCHED_LOOP_PER_MS 13600 CACHE STRING "CPU-loop iterations per millisecond at 150 MHz")
set(EOS_SCHED_TRACE 0 CACHE STRING "1 = record and later dump a bounded early-run scheduler trace")
set(EOS_REPORT_ENABLE 1 CACHE STRING "1 = print the periodic status group; 0 = measure without it")
set(EOS_FAST_RING 0 CACHE STRING "1 = replace the FreeRTOS fast queue with an SPSC ring")
set(EOS_RING_DELAY_US 0 CACHE STRING "Busy wait in the ring consumer, for the slow-consumer run")
set(EOS_BUFFER_DEADLINE_US 0 CACHE STRING "Buffer-reuse deadline in us; 0 = two block periods")
set(EOS_FRESHNESS_DEADLINE_US 0 CACHE STRING "Result freshness deadline in us; 0 = one block period")
if(EOS_BUFFER_DEADLINE_US EQUAL 0)
    set(EOS_BUFFER_DEADLINE_US ${_eos_default_buffer_deadline})
endif()
if(EOS_FRESHNESS_DEADLINE_US EQUAL 0)
    set(EOS_FRESHNESS_DEADLINE_US ${_eos_default_deadline})
endif()
foreach(opt EOS_CORES EOS_PLACEMENT EOS_TARGET_KHZ EOS_FFT_SIZE EOS_DSP_REPEATS EOS_DISPLAY_ROWS EOS_DISPLAY_PERIOD_MS EOS_UI_PERIOD_MS EOS_ALARM_PERIOD_MS EOS_ALARM_DEADLINE_US EOS_ALARM_PRIORITY EOS_SEED_SMP_BUG EOS_DSP_RAM EOS_BUFFER_DEADLINE_US EOS_FRESHNESS_DEADLINE_US EOS_SCHED_MODE EOS_SCHED_PLACEMENT EOS_SCHED_LOOP_PER_MS EOS_SCHED_TRACE EOS_REPORT_ENABLE EOS_FAST_RING EOS_RING_DELAY_US)
    message(STATUS "${opt}=${${opt}}")
endforeach()
if(NOT EOS_CORES MATCHES "^[12]$")
    message(FATAL_ERROR "EOS_CORES must be 1 or 2")
endif()
if(EOS_CORES EQUAL 1 AND NOT EOS_PLACEMENT EQUAL 0)
    message(FATAL_ERROR "EOS_PLACEMENT must be 0 when EOS_CORES=1")
endif()

if(EOS_SCHED_MODE)
    if(EOS_CORES EQUAL 1 AND NOT EOS_SCHED_PLACEMENT EQUAL 0)
        message(FATAL_ERROR "One-core scheduling requires EOS_SCHED_PLACEMENT=0")
    endif()
    if(EOS_CORES EQUAL 2 AND NOT EOS_SCHED_PLACEMENT MATCHES "^[12]$")
        message(FATAL_ERROR "Two-core scheduling requires EOS_SCHED_PLACEMENT=1 or 2")
    endif()
endif()
project(eos_lab04_condition C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(lab04_condition main.c sched_mode.c placement.c sched_trace.c sched_release.c capture.c pool.c workload.c panel.c trace.c gt911.c)
target_include_directories(lab04_condition PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(lab04_condition PRIVATE
    EOS_CORES=${EOS_CORES}
    EOS_PLACEMENT=${EOS_PLACEMENT}
    EOS_TARGET_KHZ=${EOS_TARGET_KHZ}
    EOS_FFT_SIZE=${EOS_FFT_SIZE}
    EOS_DSP_REPEATS=${EOS_DSP_REPEATS}
    EOS_DISPLAY_ROWS=${EOS_DISPLAY_ROWS}
    EOS_DISPLAY_PERIOD_MS=${EOS_DISPLAY_PERIOD_MS}
    EOS_UI_PERIOD_MS=${EOS_UI_PERIOD_MS}
    EOS_ALARM_PERIOD_MS=${EOS_ALARM_PERIOD_MS}
    EOS_ALARM_DEADLINE_US=${EOS_ALARM_DEADLINE_US}
    EOS_ALARM_PRIORITY=${EOS_ALARM_PRIORITY}
    EOS_SEED_SMP_BUG=${EOS_SEED_SMP_BUG}
    EOS_SCHED_MODE=${EOS_SCHED_MODE}
    EOS_SCHED_PLACEMENT=${EOS_SCHED_PLACEMENT}
    EOS_SCHED_DHALL=${EOS_SCHED_DHALL}
    EOS_SCHED_LOOP_PER_MS=${EOS_SCHED_LOOP_PER_MS}
    EOS_SCHED_TRACE=${EOS_SCHED_TRACE}
    EOS_REPORT_ENABLE=${EOS_REPORT_ENABLE}
    EOS_FAST_RING=${EOS_FAST_RING}
    EOS_RING_DELAY_US=${EOS_RING_DELAY_US}
    EOS_DSP_RAM=${EOS_DSP_RAM}
    $<$<EQUAL:${EOS_CORES},2>:TASK_MARKER_DISABLE=1>
    EOS_BUFFER_DEADLINE_US=${EOS_BUFFER_DEADLINE_US}
    EOS_FRESHNESS_DEADLINE_US=${EOS_FRESHNESS_DEADLINE_US}
    EOS_KERNEL_VERSION="V11.3.1"
    EOS_SIG_RTOS=1)
target_compile_options(lab04_condition PRIVATE -Wall -Wextra)
target_link_libraries(lab04_condition pico_stdlib hardware_adc hardware_dma
    hardware_pwm hardware_spi hardware_i2c hardware_clocks m FreeRTOS-Kernel-Heap4)
pico_enable_stdio_usb(lab04_condition 0)
pico_enable_stdio_uart(lab04_condition 1)
pico_add_extra_outputs(lab04_condition)

# Three-task scheduling preset for the multicore theory comparison.
add_executable(lab04_sched sched.c)
target_include_directories(lab04_sched PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(lab04_sched PRIVATE
    EOS_CORES=${EOS_CORES}
    EOS_SCHED_PLACEMENT=${EOS_SCHED_PLACEMENT}
    $<$<EQUAL:${EOS_CORES},2>:TASK_MARKER_DISABLE=1>)
target_compile_options(lab04_sched PRIVATE -Wall -Wextra)
target_link_libraries(lab04_sched pico_stdlib hardware_clocks hardware_irq hardware_sync pico_multicore
    FreeRTOS-Kernel-Heap4)
pico_enable_stdio_usb(lab04_sched 0)
pico_enable_stdio_uart(lab04_sched 1)
pico_add_extra_outputs(lab04_sched)
