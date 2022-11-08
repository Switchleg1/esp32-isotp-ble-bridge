#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_task_wdt.h"
#include "queues.h"
#include "constants.h"
#include "ble_server.h"
#include "connection_handler.h"
#include "uart.h"

#define UART_TAG 		"UART"

static SemaphoreHandle_t	uart_buffer_mutex			= NULL;
static SemaphoreHandle_t	uart_receive_task_mutex		= NULL;
static SemaphoreHandle_t	uart_send_task_mutex		= NULL;
static bool16				uart_run_task				= false;
static bool16				uart_packet_started			= false;
static uint16_t				uart_buffer_length			= 0;
static uint16_t				uart_buffer_pos 			= 0;
static uint8_t				uart_buffer[UART_BUFFER_SIZE];

static bool16				uart_buffer_check_header();
static bool16 				uart_buffer_add(uint8_t* tmp_buffer, uint16_t size);
static bool16 				uart_buffer_get(uint8_t* tmp_buffer, uint16_t size);
static bool16 				uart_buffer_parse();
static uint8_t 				uart_buffer_check_byte(uint16_t pos);
static uint16_t				uart_buffer_check_word(uint16_t pos);

void	uart_buffer_clear_unsafe();
void 	uart_send_task(void *arg);
void 	uart_receive_task(void *arg);

void uart_init()
{
	uart_deinit();

	/* Configure parameters of an UART driver,
	 * communication pins and install the driver */
	uart_config_t uart_config = {
		.baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
	};
	ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_INTERNAL_BUFFER_SIZE, UART_INTERNAL_BUFFER_SIZE, UART_QUEUE_SIZE, &uart_receive_queue, 0));
	ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
	ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TXD, UART_RXD, UART_RTS, UART_CTS));

	uart_send_queue			= xQueueCreate(UART_QUEUE_SIZE, sizeof(send_message_t));
	uart_receive_task_mutex	= xSemaphoreCreateMutex();
	uart_send_task_mutex	= xSemaphoreCreateMutex();
	uart_buffer_mutex		= xSemaphoreCreateMutex();

	ESP_LOGI(UART_TAG, "Init");
}

void uart_deinit()
{
	bool16 didDeInit = false;

	uart_driver_delete(UART_PORT_NUM);

	if (uart_send_queue) {
		//clear and delete queue
		send_message_t msg;
		while (xQueueReceive(uart_send_queue, &msg, 0) == pdTRUE)
			if (msg.buffer)
				free(msg.buffer);
		vQueueDelete(uart_send_queue);
		uart_send_queue = NULL;
		didDeInit = true;
	}

	if (uart_receive_task_mutex) {
		vSemaphoreDelete(uart_receive_task_mutex);
		uart_receive_task_mutex = NULL;
		didDeInit = true;
	}

	if (uart_send_task_mutex) {
		vSemaphoreDelete(uart_send_task_mutex);
		uart_send_task_mutex = NULL;
		didDeInit = true;
	}

	if (uart_buffer_mutex) {
		vSemaphoreDelete(uart_buffer_mutex);
		uart_buffer_mutex = NULL;
		didDeInit = true;
	}

	if(didDeInit)
		ESP_LOGI(UART_TAG, "Deinit");
}

void uart_start_task()
{
	uart_stop_task();
	uart_run_task = true;
	ESP_LOGI(UART_TAG, "Tasks starting");
	xSemaphoreTake(sync_task_sem, 0);
	xTaskCreate(uart_receive_task, "UART_receive_process", TASK_STACK_SIZE, NULL, UART_TSK_PRIO, NULL);
	xSemaphoreTake(sync_task_sem, portMAX_DELAY);
	xTaskCreate(uart_send_task, "UART_send_process", TASK_STACK_SIZE, NULL, UART_TSK_PRIO, NULL);
	xSemaphoreTake(sync_task_sem, portMAX_DELAY);
	ESP_LOGI(UART_TAG, "Tasks started");
}

void uart_stop_task()
{
	if (uart_run_task) {
		uart_run_task = false;

		send_message_t msg;
		xQueueSend(uart_receive_queue, &msg, portMAX_DELAY);
		xSemaphoreTake(uart_receive_task_mutex, portMAX_DELAY);
		xSemaphoreGive(uart_receive_task_mutex);

		xQueueSend(uart_send_queue, &msg, portMAX_DELAY);
		xSemaphoreTake(uart_send_task_mutex, portMAX_DELAY);
		xSemaphoreGive(uart_send_task_mutex);

		ESP_LOGI(UART_TAG, "Tasks stopped");
	}
}

void uart_send(uint32_t txID, uint32_t rxID, uint8_t flags, const void* src, size_t size)
{
	send_message_t msg;
	msg.msg_length = size + sizeof(ble_header_t);
	msg.buffer = malloc(msg.msg_length);
	if (msg.buffer) {
		//Build header
		ble_header_t* header = (ble_header_t*)msg.buffer;
		header->hdID		= BLE_HEADER_ID;
		header->cmdFlags	= flags;
		header->rxID		= rxID;
		header->txID		= txID;
		header->cmdSize		= size;
		memcpy(msg.buffer + sizeof(ble_header_t), src, size);

		if (xQueueSend(uart_send_queue, &msg, pdMS_TO_TICKS(TIMEOUT_NORMAL)) != pdTRUE) {
			free(msg.buffer);
		}
	} else {
		ESP_LOGD(UART_TAG, "uart_send: malloc error size(%d)", msg.msg_length);
	}
}

void uart_send_task(void *arg)
{
	//subscribe to WDT
	ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
	ESP_ERROR_CHECK(esp_task_wdt_status(NULL));

	tMUTEX(uart_send_task_mutex);
		ESP_LOGI(UART_TAG, "Task send started");
		xSemaphoreGive(sync_task_sem);
		send_message_t event;
		while(uart_run_task)
		{
			if (xQueueReceive(uart_send_queue, &event, pdMS_TO_TICKS(TIMEOUT_LONG)) == pdTRUE) {
				if (event.buffer) {
					if (uart_run_task)
						uart_write_bytes(UART_PORT_NUM, (const char*)event.buffer, event.msg_length);

					free(event.buffer);
				}
			}

			//reset the WDT and yield to tasks
			esp_task_wdt_reset();
			taskYIELD();
		}
		ESP_LOGI(UART_TAG, "Task send stopped");
	rMUTEX(uart_send_task_mutex);

	//unsubscribe to WDT and delete task
	ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
	vTaskDelete(NULL);
}

void uart_buffer_clear()
{
	tMUTEX(uart_buffer_mutex);
		uart_buffer_clear_unsafe();
	rMUTEX(uart_buffer_mutex);
}

void uart_buffer_clear_unsafe()
{
	uart_packet_started	= false;
	uart_buffer_length	= 0;
	uart_buffer_pos 	= 0;
}

bool16 uart_buffer_check_header()
{
	if(uart_buffer_length) {
		if(uart_buffer_check_byte(0) == BLE_HEADER_ID) {
			if(uart_buffer_length >= sizeof(ble_header_t)) {

				//check command length, fail if oversized
				uint16_t packet_len = uart_buffer_check_word(6) + sizeof(ble_header_t);
				if(packet_len > UART_BUFFER_SIZE) {
					uart_buffer_clear_unsafe();
					return false;
				}

				return true;
			}
		} else {
			uart_buffer_clear_unsafe();
			return false;
		}
	}

	return false;
}

bool16 uart_buffer_add(uint8_t* tmp_buffer, uint16_t size)
{
	bool16 		over_flow 	= false;
	uint16_t	tmp_pos 	= 0;

	tMUTEX(uart_buffer_mutex);
		//check for over flow of the buffer
		if(size + uart_buffer_length > UART_BUFFER_SIZE) {
			size = UART_BUFFER_SIZE - uart_buffer_length;
			over_flow = true;
		}

		//check current write position (buffer pos + length) and if its beyond buffer length, wrap
		uint16_t write_pos = uart_buffer_pos + uart_buffer_length;
		if(write_pos >= UART_BUFFER_SIZE)
			write_pos -= UART_BUFFER_SIZE;

		//if the size of the data extends beyond end of buffer, write partial
		if(write_pos + size > UART_BUFFER_SIZE) {
			uint16_t partial_size = UART_BUFFER_SIZE - write_pos;
			memcpy(&uart_buffer[write_pos], tmp_buffer, partial_size);
			size -= partial_size;
			uart_buffer_length += partial_size;
			tmp_pos = partial_size;
			write_pos = 0;
		}

		//write the remaining data to buffer
		if(size) {
			memcpy(&uart_buffer[write_pos], &tmp_buffer[tmp_pos], size);
			uart_buffer_length += size;
		}
	rMUTEX(uart_buffer_mutex);

	return over_flow;
}

bool16 uart_buffer_get(uint8_t* tmp_buffer, uint16_t size)
{
	bool16 		over_flow 	= false;
	uint16_t	tmp_pos 	= 0;

	//check for over flow of the buffer
	if(size > uart_buffer_length) {
		size = uart_buffer_length;
		over_flow = true;
	}

	//check current to see if we will be reading beyond the end of the buffer, if so get partial data from end and wrap afterwards
	if(uart_buffer_pos + size > UART_BUFFER_SIZE) {
		uint16_t partial_size = UART_BUFFER_SIZE - uart_buffer_pos;
		memcpy(tmp_buffer, &uart_buffer[uart_buffer_pos], partial_size);
		size -= partial_size;
		uart_buffer_length -= partial_size;
		uart_buffer_pos = 0;
		tmp_pos = partial_size;
	}

	//read the remaining data from buffer
	if(size) {
		memcpy(&tmp_buffer[tmp_pos], &uart_buffer[uart_buffer_pos], size);
		uart_buffer_length -= size;
		uart_buffer_pos += size;
	}

	return over_flow;
}

uint8_t uart_buffer_check_byte(uint16_t pos)
{
	uint16_t tmp_pos = uart_buffer_pos + pos;
	if(tmp_pos >= UART_BUFFER_SIZE) tmp_pos -= UART_BUFFER_SIZE;
	uint8_t tmp_data = uart_buffer[tmp_pos];

	return tmp_data;
}

uint16_t uart_buffer_check_word(uint16_t pos)
{
	uint16_t tmp_pos = uart_buffer_pos + pos;
	uint16_t tmp_pos1 = tmp_pos + 1;
	if(tmp_pos >= UART_BUFFER_SIZE) tmp_pos -= UART_BUFFER_SIZE;
	if(tmp_pos1 >= UART_BUFFER_SIZE) tmp_pos1 -= UART_BUFFER_SIZE;
	uint16_t tmp_data = (uart_buffer[tmp_pos+1] << 8) + uart_buffer[tmp_pos];

	return tmp_data;
}

bool16 uart_buffer_parse()
{
	if(uart_buffer_check_header()) {
		//if we are just starting a packet set our timeout
		if(!uart_packet_started) {
			uart_packet_started = true;
			ch_take_uart_packet_timer_sem();
		}

		//get packet length
		uint16_t packet_len = uart_buffer_check_word(6) + sizeof(ble_header_t);
		if(packet_len > UART_BUFFER_SIZE)
			packet_len = UART_BUFFER_SIZE;

		//if data is >= to packet length, send it
		if(uart_buffer_length >= packet_len) {
			uint8_t* packet_data = malloc(packet_len);
			if (packet_data) {
				uart_buffer_get(packet_data, packet_len);
				uart_data_received((void*)packet_data, packet_len);
				free(packet_data);

				uart_packet_started = false;
				return true;
			} else {
				ESP_LOGD(UART_TAG, "uart_buffer_parse: malloc error size(%d)", packet_len);

				//clear current buffer on malloc error
				uart_buffer_clear_unsafe();
			}
		}
	}

	return false;
}

void uart_receive_task(void *arg)
{
	//subscribe to WDT
	ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
	ESP_ERROR_CHECK(esp_task_wdt_status(NULL));

	tMUTEX(uart_receive_task_mutex);
		ESP_LOGI(UART_TAG, "Task receive started");
		xSemaphoreGive(sync_task_sem);
		uart_event_t 	event;
		while(uart_run_task)
		{
			if(xQueueReceive(uart_receive_queue, &event, pdMS_TO_TICKS(TIMEOUT_LONG)) == pdTRUE) {
				if (uart_run_task) {
					tMUTEX(uart_buffer_mutex);
						ESP_LOGI(UART_TAG, "uart[%d] event:", UART_PORT_NUM);
						switch (event.type) {
							case UART_DATA:
								ESP_LOGI(UART_TAG, "[UART DATA]: %d", event.size);
								uint8_t* tmp_buffer = malloc(event.size);
								if (tmp_buffer) {
									uart_read_bytes(UART_PORT_NUM, tmp_buffer, event.size, portMAX_DELAY);
									uart_buffer_add(tmp_buffer, event.size);
									free(tmp_buffer);
									while (uart_buffer_parse());
								}
								else {
									ESP_LOGD(UART_TAG, "uart_receive_task: malloc error size(%d)", event.size);
								}
								break;
								//Event of HW FIFO overflow detected
							case UART_FIFO_OVF:
								ESP_LOGI(UART_TAG, "hw fifo overflow");
								uart_buffer_clear_unsafe();
								uart_flush_input(UART_PORT_NUM);
								xQueueReset(uart_receive_queue);
								break;
								//Event of UART ring buffer full
							case UART_BUFFER_FULL:
								ESP_LOGI(UART_TAG, "ring buffer full");
								uart_buffer_clear_unsafe();
								uart_flush_input(UART_PORT_NUM);
								xQueueReset(uart_receive_queue);
								break;
								//Event of UART RX break detected
							case UART_BREAK:
								ESP_LOGI(UART_TAG, "uart rx break");
								break;
								//Event of UART parity check error
							case UART_PARITY_ERR:
								ESP_LOGI(UART_TAG, "uart parity error");
								break;
								//Event of UART frame error
							case UART_FRAME_ERR:
								ESP_LOGI(UART_TAG, "uart frame error");
								break;
								//Others
							default:
								ESP_LOGI(UART_TAG, "uart event type: %d", event.type);
								break;
						}
					rMUTEX(uart_buffer_mutex);
				}
			}

			//reset the WDT and yield to tasks
			esp_task_wdt_reset();
			taskYIELD();
		}
		ESP_LOGI(UART_TAG, "Task receive stopped");
	rMUTEX(uart_receive_task_mutex);

	//unsubscribe to WDT and delete task
	ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
	vTaskDelete(NULL);
}
