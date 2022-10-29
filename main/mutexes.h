#ifndef MUTEXES_H
#define MUTEXES_H

#include "freertos/semphr.h"
#include "constants.h"

SemaphoreHandle_t sync_task_sem;
SemaphoreHandle_t ch_can_timer_sem;

SemaphoreHandle_t ch_uart_packet_timer_sem;
SemaphoreHandle_t ch_sleep_sem;
SemaphoreHandle_t uart_buffer_mutex;

#endif
