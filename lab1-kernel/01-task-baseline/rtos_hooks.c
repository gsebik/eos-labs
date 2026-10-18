#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    printf("STACK OVERFLOW: %s\n", task_name ? task_name : "unknown");
    taskDISABLE_INTERRUPTS();
    for (;;) { }
}
