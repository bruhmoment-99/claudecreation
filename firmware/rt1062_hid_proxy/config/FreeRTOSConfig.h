/*
 * FreeRTOSConfig.h - FreeRTOS settings for the i.MX RT1062 HID proxy.
 *
 * Based on the SDK template rtos/freertos-kernel/template/ARM_CM4F_4_priority_bits
 * (FreeRTOS Kernel V10.5.1, MIT licence). Port: portable/GCC/ARM_CM4F, which is
 * the correct port for the RT1062's Cortex-M7 r1p1 (the ARM_CM7/r0p1 port is
 * only needed for r0p1 silicon).
 *
 * Values that matter for this firmware:
 *   configTICK_RATE_HZ 1000      cmd.sleep()/timeouts are in ms
 *   configMAX_PRIORITIES 7       app_config.h uses priorities 1..5
 *   configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 2
 *                                USB1 runs at NVIC 2 and calls FreeRTOS from
 *                                its ISR (see app_config.h)
 *   configTOTAL_HEAP_SIZE 32 KB  task stacks + USB OSA objects
 *   Stack overflow / malloc failure / configASSERT all end in
 *   app_fatal(), which blinks the fault pattern on the status LED.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#if defined(__ICCARM__) || defined(__CC_ARM) || defined(__GNUC__)
#include <stdint.h>
extern uint32_t SystemCoreClock;
/* status_led.c: never returns; blinks the fault code on the LED. */
extern void app_fatal(uint32_t code);
#endif

#define configUSE_PREEMPTION                    1
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      (SystemCoreClock)
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    7
#define configMINIMAL_STACK_SIZE                ((unsigned short)128)
#define configMAX_TASK_NAME_LEN                 12
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIME_SLICING                  0
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     1
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define configUSE_APPLICATION_TASK_TAG          0

/* Memory allocation (heap_4). */
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   ((size_t)(32U * 1024U))
#define configAPPLICATION_ALLOCATED_HEAP        0

/* Hooks: all failures are made visible on the status LED. */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         2

/* Software timers (used by the SDK OSA layer). Kept below the USB tasks. */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               2
#define configTIMER_QUEUE_LENGTH                8
#define configTIMER_TASK_STACK_DEPTH            (configMINIMAL_STACK_SIZE * 2)

#define configASSERT(x)      \
    if ((x) == 0)            \
    {                        \
        app_fatal(__LINE__); \
    }

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          0
#define INCLUDE_eTaskGetState                   0
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 0
#define INCLUDE_xTaskGetHandle                  0
#define INCLUDE_xTaskResumeFromISR              1

/* Cortex-M7 on the RT1062 implements 4 priority bits (16 levels). */
#ifdef __NVIC_PRIO_BITS
#define configPRIO_BITS __NVIC_PRIO_BITS
#else
#define configPRIO_BITS 4
#endif

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      ((1U << (configPRIO_BITS)) - 1)
/* ISRs that call FreeRTOS "FromISR" APIs must have a priority number >= this. */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 2
#define configKERNEL_INTERRUPT_PRIORITY      (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* Map the port handlers to the CMSIS vector names used by startup_MIMXRT1062.S. */
#define vPortSVCHandler    SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
