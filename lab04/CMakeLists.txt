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

option(EOS_PIN_CONTROL "Pin the SMP periodic control task to core 1" OFF)
option(EOS_CORE0_LOAD "Enable the mode-1 core-0 load for comparison" ON)
foreach(opt EOS_PIN_CONTROL EOS_CORE0_LOAD)
    message(STATUS "${opt}=${${opt}}")
endforeach()

# Task 5 (mode 4): hand records from core 1 to core 0 through one shared slot
# (OFF) or through a ring buffer whose ordering you complete (ON).
option(EOS_HANDOFF_RING "Mode 4: use the ring buffer instead of one shared slot" OFF)
set(EOS_PRODUCE_US 20 CACHE STRING "Mode 4: microseconds between produced records")
set(EOS_CONSUME_DELAY_US 0 CACHE STRING "Mode 4: extra consumer delay per record (0 = none)")
# Task 6 (mode 5): where the two pipeline stages run.
set(EOS_CAPACITY_PLACEMENT "SINGLE" CACHE STRING
    "Mode 5 placement: SINGLE, AMP, SMP_PINNED or SMP_FREE")
set_property(CACHE EOS_CAPACITY_PLACEMENT PROPERTY STRINGS SINGLE AMP SMP_PINNED SMP_FREE)
set(EOS_STAGE_A_ROUNDS 16 CACHE STRING "Mode 5: filter rounds in stage A")
set(EOS_STAGE_B_ROUNDS 24 CACHE STRING "Mode 5: filter rounds in stage B")
set(_eos_placements SINGLE AMP SMP_PINNED SMP_FREE)
list(FIND _eos_placements "${EOS_CAPACITY_PLACEMENT}" EOS_PLACEMENT_INDEX)
if(EOS_PLACEMENT_INDEX EQUAL -1)
    message(FATAL_ERROR "EOS_CAPACITY_PLACEMENT must be one of: ${_eos_placements}")
endif()
foreach(opt EOS_HANDOFF_RING EOS_PRODUCE_US EOS_CONSUME_DELAY_US
            EOS_CAPACITY_PLACEMENT EOS_STAGE_A_ROUNDS EOS_STAGE_B_ROUNDS)
    message(STATUS "${opt}=${${opt}}")
endforeach()
project(eos_lab04_multicore C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

set(L4_MAIN_SOURCE "${CMAKE_CURRENT_LIST_DIR}/main.c"
    CACHE FILEPATH "Student or private reference source")
add_executable(lab04_mode1 "${L4_MAIN_SOURCE}")
add_executable(lab04_mode2 "${L4_MAIN_SOURCE}")
add_executable(lab04_mode3 "${L4_MAIN_SOURCE}")
add_executable(lab04_mode4 "${L4_MAIN_SOURCE}")
add_executable(lab04_mode5 "${L4_MAIN_SOURCE}")
foreach(mode RANGE 1 5)
    target_include_directories(lab04_mode${mode} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_compile_definitions(lab04_mode${mode} PRIVATE EOS_LAB_MODE=${mode}
        EOS_CORE0_LOAD=$<BOOL:${EOS_CORE0_LOAD}>
        EOS_HANDOFF_RING=$<BOOL:${EOS_HANDOFF_RING}>
        EOS_PRODUCE_US=${EOS_PRODUCE_US}
        EOS_CONSUME_DELAY_US=${EOS_CONSUME_DELAY_US}
        EOS_PLACEMENT=${EOS_PLACEMENT_INDEX}
        EOS_STAGE_A_ROUNDS=${EOS_STAGE_A_ROUNDS}
        EOS_STAGE_B_ROUNDS=${EOS_STAGE_B_ROUNDS})
    if(EOS_PIN_CONTROL)
        target_compile_definitions(lab04_mode${mode} PRIVATE EOS_PIN_CONTROL=1)
    endif()
    target_link_libraries(lab04_mode${mode} pico_stdlib pico_multicore hardware_clocks FreeRTOS-Kernel-Heap4)
    pico_enable_stdio_usb(lab04_mode${mode} 0)
    pico_enable_stdio_uart(lab04_mode${mode} 1)
    pico_add_extra_outputs(lab04_mode${mode})
endforeach()
