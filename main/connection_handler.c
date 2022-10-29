#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "isotp.h"
#include "mutexes.h"
#include "queues.h"
#include "constants.h"
#include "connection_handler.h"
#include "ble_server.h"
#include "uart.h"
#include "twai.h"

#define CH_TAG 			"Connection_handler"

static SemaphoreHandle_t		ch_task_mutex				= NULL;
static SemaphoreHandle_t		ch_settings_mutex			= NULL;
static bool16					run_ch_task					= false;
static RTC_DATA_ATTR uint32_t	first_sleep_timer			= TIMEOUT_FIRSTBOOT;
static uint32_t 				uart_connection_timer 		= 0;
static uint32_t 				uart_packet_timer			= 0;
static uint32_t					can_connection_timer		= 0;

void ch_task(void *arg);

void ch_init()
{
	ch_deinit();

	ch_settings_mutex			= xSemaphoreCreateMutex();
	ch_uart_packet_timer_sem	= xSemaphoreCreateBinary();
	ch_can_timer_sem			= xSemaphoreCreateBinary();
	ch_sleep_sem				= xSemaphoreCreateBinary();
	ch_task_mutex				= xSemaphoreCreateMutex();

	ESP_LOGI(CH_TAG, "Init");
}

void ch_deinit()
{
	bool16 didDeInit = false;

	if (ch_settings_mutex) {
		vSemaphoreDelete(ch_settings_mutex);
		ch_settings_mutex = NULL;
		didDeInit = true;
	}

	if (ch_uart_packet_timer_sem) {
		vSemaphoreDelete(ch_uart_packet_timer_sem);
		ch_uart_packet_timer_sem = NULL;
		didDeInit = true;
	}

	if (ch_can_timer_sem) {
		vSemaphoreDelete(ch_can_timer_sem);
		ch_can_timer_sem = NULL;
		didDeInit = true;
	}
	

	if (ch_sleep_sem) {
		vSemaphoreDelete(ch_sleep_sem);
		ch_sleep_sem = NULL;
		didDeInit = true;
	}
	
	if (ch_task_mutex) {
		vSemaphoreDelete(ch_task_mutex);
		ch_task_mutex = NULL;
		didDeInit = true;
	}
	
	if (didDeInit)
		ESP_LOGI(CH_TAG, "Deinit");
}

void ch_start_task()
{
	ch_stop_task();

	xSemaphoreTake(ch_settings_mutex, pdMS_TO_TICKS(TIMEOUT_NORMAL));
	run_ch_task = true;
	xSemaphoreGive(ch_settings_mutex);

	ESP_LOGI(CH_TAG, "Task starting");
	xSemaphoreTake(sync_task_sem, 0);
	xTaskCreate(ch_task, "Connection_handling_process", TASK_STACK_SIZE, NULL, HANDLER_TSK_PRIO, NULL);
	xSemaphoreTake(sync_task_sem, portMAX_DELAY);
}

void ch_stop_task()
{
	xSemaphoreTake(ch_settings_mutex, pdMS_TO_TICKS(TIMEOUT_NORMAL));
	if (run_ch_task) {
		run_ch_task = false;
		xSemaphoreGive(ch_settings_mutex);
		xSemaphoreTake(ch_task_mutex, portMAX_DELAY);
		xSemaphoreGive(ch_task_mutex);
		ESP_LOGI(CH_TAG, "Task stopped");
	}
	else {
		xSemaphoreGive(ch_settings_mutex);
	}
}

void ch_reset_uart_timer()
{
	xSemaphoreTake(ch_settings_mutex, pdMS_TO_TICKS(TIMEOUT_NORMAL));
	if(!uart_connection_timer) {
		xSemaphoreGive(ch_settings_mutex);
		ble_stop_advertising();
		ch_on_uart_connect();
		xSemaphoreTake(ch_settings_mutex, pdMS_TO_TICKS(TIMEOUT_NORMAL));
	}
	uart_connection_timer = TIMEOUT_UARTCONNECTION;
	xSemaphoreGive(ch_settings_mutex);
}

bool16 ch_uart_connected()
{
	xSemaphoreTake(ch_settings_mutex, pdMS_TO_TICKS(TIMEOUT_NORMAL));
	bool16 connection_timer = uart_connection_timer != 0;
	xSemaphoreGive(ch_settings_mutex);

	return connection_timer;
}

bool16 ch_uart_connection_countdown()
{
	if(uart_connection_timer) {
		if(--uart_connection_timer == 0) {
			ble_start_advertising();
			ch_on_uart_disconnect();
		}
	}
	bool16 connection_timer = uart_connection_timer != 0;

	return connection_timer;
}

void ch_task(void *arg)
{
	xSemaphoreTake(ch_task_mutex, portMAX_DELAY);
	xSemaphoreGive(sync_task_sem);
	ESP_LOGI(CH_TAG, "Task started");
	while(run_ch_task)
	{
		//count down our first boot timer
		if(first_sleep_timer) {
			first_sleep_timer--;
		}

		//Did we receive a CAN or uart message?
		if((!ble_connected() && !ch_uart_connection_countdown()) && !first_sleep_timer && xSemaphoreTake(ch_can_timer_sem, 0) == pdTRUE) {
			if (can_connection_timer) {
				can_connection_timer--;
			}
#ifdef ALLOW_SLEEP
			if(!can_connection_timer)
				xSemaphoreGive(ch_sleep_sem);
#endif
		} else {
			can_connection_timer = TIMEOUT_CANCONNECTION;
		}

		//check for packet timeout
		xSemaphoreTake(uart_buffer_mutex, pdMS_TO_TICKS(TIMEOUT_NORMAL));
		if(ch_uart_connected() && xSemaphoreTake(ch_uart_packet_timer_sem, 0) == pdTRUE) {
			if (uart_packet_timer) {
				uart_packet_timer--;
			}

			if (!uart_packet_timer)
        		uart_buffer_clear();
		} else {
			uart_packet_timer = TIMEOUT_UARTPACKET;
		}
		xSemaphoreGive(uart_buffer_mutex);

		//give semaphores used for countdown timers
		xSemaphoreGive(ch_can_timer_sem);
		xSemaphoreGive(ch_uart_packet_timer_sem);

		vTaskDelay(pdMS_TO_TICKS(1000));
	}
	ESP_LOGI(CH_TAG, "Task stopping");
	xSemaphoreGive(ch_task_mutex);
    vTaskDelete(NULL);
}
