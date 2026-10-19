/* Task marker: publish the number of the running task on three GPIOs.
 *
 * The FreeRTOS switch hook stores the current task's number into three pins, so
 * a logic analyzer can show which task is running, live, without halting the
 * target or reading its memory. The store is a single write to the SIO output
 * register, so the hook costs a few cycles per context switch.
 *
 * The same three pins carry the same task numbers that the firmware reports, so
 * the pin view and the RAM event log can be compared directly.
 *
 *   bit 0 -> GP19   bit 1 -> GP20   bit 2 -> GP16
 *
 * The EP-0172 carrier's soldered PicoGremlin already connects these pins to
 * analyzer channels: channel i is GPIO 2+i for GPIO 2..22.
 *
 * Define TASK_MARKER_DISABLE to compile the hook away and measure the firmware
 * without it.
 */
#pragma once
#include <stdint.h>

/* SIO base + GPIO_OUT offset. Written as an address rather than by including
 * an SDK header, because FreeRTOSConfig.h is also preprocessed for the port's
 * assembly files, where C declarations are not valid. */
#define TASK_MARKER_GPIO_OUT  (*(volatile uint32_t *)0xd0000010u)

#define TASK_MARKER_MASK  ((1u << 16) | (1u << 19) | (1u << 20))

/* Task numbers, shared with the report and with analyzer.py. */
enum {
    TASK_ID_IDLE = 0,
    TASK_ID_DSP = 1,
    TASK_ID_DISPLAY = 2,
    TASK_ID_UI = 3,
    TASK_ID_REPORT = 4,
    TASK_ID_CONTROLLER = 5,
};

/* Publish a task number. Only three bits are used, so values above 7 wrap. */
#ifndef __ASSEMBLER__
static inline void task_marker_publish(uint32_t task_number)
{
    uint32_t value = 0;
    if (task_number & 1u) value |= 1u << 19;
    if (task_number & 2u) value |= 1u << 20;
    if (task_number & 4u) value |= 1u << 16;
    TASK_MARKER_GPIO_OUT = (TASK_MARKER_GPIO_OUT & ~TASK_MARKER_MASK) | value;
}

#if !defined(TASK_MARKER_DISABLE) && !defined(__ASSEMBLER__)
#define traceTASK_SWITCHED_IN() \
    task_marker_publish((uint32_t)uxTaskGetTaskNumber(xTaskGetCurrentTaskHandle()))
#endif
#endif /* __ASSEMBLER__ */
