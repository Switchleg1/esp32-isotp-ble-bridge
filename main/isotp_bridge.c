#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/twai.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_err.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/dport_reg.h"
#include "isotp.h"
#include "ble_server.h"
#include "isotp_link_containers.h"
#include "twai.h"
#include "queues.h"
#include "persist.h"
#include "constants.h"
#include "led.h"
#include "eeprom.h"
#include "uart.h"
#include "connection_handler.h"
#include "isotp_bridge.h"

#define BRIDGE_TAG 					"Bridge"

/* ------------------------ global vars ------------------------------------ */
static SemaphoreHandle_t	isotp_send_task_mutex	= NULL;
static SemaphoreHandle_t	isotp_settings_mutex	= NULL;
static SemaphoreHandle_t	isotp_receive_mutex		= NULL;
static bool16				isotp_run_tasks			= false;
static ble_header_t			split_header;
static uint16_t				split_enabled 			= false;
static uint8_t 				split_count 			= 0;
static uint16_t				split_length 			= 0;
static uint8_t*				split_data 				= NULL;
#ifdef PASSWORD_CHECK
static bool16				passwordChecked			= false;
#endif

bool16	isotp_allow_run_tasks();
void	isotp_set_run_tasks(bool16 allow);

/* ---------------------------- ISOTP Callbacks ---------------------------- */

int isotp_user_send_can(const uint32_t arbitration_id, const uint8_t* data, const uint16_t size) {
    twai_message_t frame = {.identifier = arbitration_id, .data_length_code = size};
    memcpy(frame.data, data, sizeof(frame.data));
	xQueueSend(can_send_queue, &frame, portMAX_DELAY);

    return ISOTP_RET_OK;                           
}

uint64_t isotp_user_get_us(void) {
	return esp_timer_get_time();
}

void isotp_user_debug(const char* message, ...) {
	ESP_LOGD(BRIDGE_TAG, "ISOTP: %s", message);
}

/* --------------------------- Functions --------------------------------- */
void set_password_checked(bool16 allow)
{
#ifdef PASSWORD_CHECK
	tMUTEX(isotp_settings_mutex);
		passwordChecked = allow;
	rMUTEX(isotp_settings_mutex);
#endif
}

bool16 get_password_checked()
{
#ifdef PASSWORD_CHECK
	tMUTEX(isotp_settings_mutex);
		bool16 checked = passwordChecked;
	rMUTEX(isotp_settings_mutex);

	return checked;
#else
	return true;
#endif
}

bool16 check_password(char* data)
{
#ifdef PASSWORD_CHECK
	if(data) {
		char* pass = eeprom_read_str(PASSWORD_KEY);
		if(pass) {
			if(!strcmp(data, pass))
				set_password_checked(true);

			free(pass);
		} else {
			eeprom_write_str(PASSWORD_KEY, PASSWORD_DEFAULT);
			eeprom_commit();

			if(!strcmp(data, PASSWORD_DEFAULT))
				set_password_checked(true);
		}
	}
	
	return get_password_checked();
#else
	return true;
#endif
}

void write_password(char* data)
{
#ifdef PASSWORD_CHECK
	eeprom_write_str(PASSWORD_KEY, data);
	eeprom_commit();
	ESP_LOGI(BRIDGE_TAG, "Set password [%s]", data);
#endif
}

void send_packet(uint32_t txID, uint32_t rxID, uint8_t flags, const void* src, size_t size)
{
	if(ble_connected()) {
		ble_send(txID, rxID, flags, src, size);
	} else {
		uart_send(txID, rxID, flags, src, size);
	}
}

/* --------------------------- ISOTP Tasks -------------------------------------- */

static void isotp_processing_task(void *arg)
{
	//subscribe to WDT
	ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
	ESP_ERROR_CHECK(esp_task_wdt_status(NULL));

    IsoTpLinkContainer *isotp_link_container = (IsoTpLinkContainer*)arg;
    IsoTpLink *link_ptr = &isotp_link_container->link;
	uint8_t *payload_buf = isotp_link_container->payload_buf;
	uint16_t number = isotp_link_container->number;

	tMUTEX(isotp_link_container->task_mutex);
		ESP_LOGI(BRIDGE_TAG, "Receive task started: %d", number);
		xSemaphoreGive(sync_task_sem);
		while (isotp_allow_run_tasks())
		{
			if (link_ptr->send_status != ISOTP_SEND_STATUS_INPROGRESS &&
				link_ptr->receive_status != ISOTP_RECEIVE_STATUS_INPROGRESS) {
				xSemaphoreTake(isotp_link_container->wait_for_isotp_data_sem, pdMS_TO_TICKS(TIMEOUT_LONG));
			}
			
			// poll
			tMUTEX(isotp_link_container->data_mutex);
				isotp_poll(link_ptr);
				uint16_t out_size;
				int ret = isotp_receive(link_ptr, payload_buf, isotp_link_container->buffer_size, &out_size);
			rMUTEX(isotp_link_container->data_mutex);

			// if it is time to send fully received + parsed ISO-TP data over BLE and/or websocket
			if (ret == ISOTP_RET_OK) {
				ESP_LOGI(BRIDGE_TAG, "Received ISO-TP message with length: %04X", out_size);
				for (int i = 0; i < out_size; i++) {
					ESP_LOGD(BRIDGE_TAG, "payload_buf[%d] = %02x", i, payload_buf[i]);
				}

				//Are we in persist mode?
				if(number < PERSIST_COUNT && persist_enabled())
				{
					//send time stamp instead of rx/tx
					uint32_t time = (esp_timer_get_time() / 1000UL) & 0xFFFFFFFF;
					uint16_t rxID = (time >> 16) & 0xFFFF;
					uint16_t txID = time & 0xFFFF;
					send_packet(txID, rxID, 0, payload_buf, out_size);
					persist_allow_send(number);
				} else {
					tMUTEX(isotp_link_container->data_mutex);
						uint32_t txID = link_ptr->receive_arbitration_id;
						uint32_t rxID = link_ptr->send_arbitration_id;
					rMUTEX(isotp_link_container->data_mutex);
					send_packet(txID, rxID, 0, payload_buf, out_size);
				}
			}

			//reset the WDT and yield to tasks
			esp_task_wdt_reset();
			taskYIELD();
		}
		ESP_LOGI(BRIDGE_TAG, "Receive task stopped: %d", number);
	rMUTEX(isotp_link_container->task_mutex);

	//unsubscribe to WDT and delete task
	ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
}

static void isotp_send_queue_task(void *arg)
{
	//subscribe to WDT
	ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
	ESP_ERROR_CHECK(esp_task_wdt_status(NULL));

	tMUTEX(isotp_send_task_mutex);
		ESP_LOGI(BRIDGE_TAG, "Send task started");
		xSemaphoreGive(sync_task_sem);
		while (isotp_allow_run_tasks()) {
			send_message_t msg;
			if (xQueueReceive(isotp_send_message_queue, &msg, pdMS_TO_TICKS(TIMEOUT_LONG)) == pdTRUE) {
				if (isotp_allow_run_tasks()) {
					ESP_LOGD(BRIDGE_TAG, "isotp_send_queue_task: sending message with %d size (rx id: %04x / tx id: %04x)", msg.msg_length, msg.rxID, msg.txID);
					for (uint16_t i = 0; i < NUM_ISOTP_LINK_CONTAINERS; i++) {
						bool16 found_container = false;
						IsoTpLinkContainer* isotp_link_container = &isotp_link_containers[i];
						tMUTEX(isotp_link_container->data_mutex);
							if (msg.txID == isotp_link_container->link.receive_arbitration_id &&
								msg.rxID == isotp_link_container->link.send_arbitration_id) {
								ESP_LOGD(BRIDGE_TAG, "container match [%d]", i);
								isotp_link_container_id = i;
								isotp_send(&isotp_link_container->link, msg.buffer, msg.msg_length);
								xSemaphoreGive(isotp_link_container->wait_for_isotp_data_sem);
								found_container = true;
							}
						rMUTEX(isotp_link_container->data_mutex);

						if(found_container)
							break;
					}
					free(msg.buffer);
				}
			}

			//reset the WDT and yield to tasks
			esp_task_wdt_reset();
			taskYIELD();
		}
		ESP_LOGI(BRIDGE_TAG, "Send task stopped");
	rMUTEX(isotp_send_task_mutex);

	//unsubscribe to WDT and delete task
	ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
}

/* --------------------------- ISOTP Functions -------------------------------------- */
void isotp_init()
{
	isotp_deinit();

	isotp_send_task_mutex		= xSemaphoreCreateMutex();
	isotp_settings_mutex		= xSemaphoreCreateMutex();
	isotp_receive_mutex			= xSemaphoreCreateMutex();
	isotp_send_message_queue	= xQueueCreate(ISOTP_QUEUE_SIZE, sizeof(send_message_t));

	configure_isotp_links();
}

void isotp_deinit()
{
	bool16 didDeinit = false;

	if (isotp_send_task_mutex) {
		vSemaphoreDelete(isotp_send_task_mutex);
		isotp_send_task_mutex = NULL;
		didDeinit = true;
	}

	if (isotp_settings_mutex) {
		vSemaphoreDelete(isotp_settings_mutex);
		isotp_settings_mutex = NULL;
		didDeinit = true;
	}

	if (isotp_receive_mutex) {
		vSemaphoreDelete(isotp_receive_mutex);
		isotp_receive_mutex = NULL;
		didDeinit = true;
	}

	if (isotp_send_message_queue) {
		//clear and delete queue
		send_message_t msg;
		while (xQueueReceive(isotp_send_message_queue, &msg, 0) == pdTRUE)
			if (msg.buffer)
				free(msg.buffer);
		vQueueDelete(isotp_send_message_queue);
		isotp_send_message_queue = NULL;
		didDeinit = true;
	}

	disable_isotp_links();

	if (didDeinit)
		ESP_LOGI(BRIDGE_TAG, "Deinit");
}

void isotp_start_task()
{
	isotp_stop_task();
	isotp_set_run_tasks(true);

	ESP_LOGI(BRIDGE_TAG, "Tasks starting");
	xSemaphoreTake(sync_task_sem, 0);
	// create tasks for each isotp_link
	for (uint16_t i = 0; i < NUM_ISOTP_LINK_CONTAINERS; i++)
	{
		IsoTpLinkContainer* isotp_link_container = &isotp_link_containers[i];
		xTaskCreate(isotp_processing_task, isotp_link_container->name, TASK_STACK_SIZE, isotp_link_container, ISOTP_TSK_PRIO, NULL);
		xSemaphoreTake(sync_task_sem, portMAX_DELAY);
	}

	// "ISOTP_process" pumps the ISOTP library's "poll" method, which will call the send queue callback if a message needs to be sent.
	// ISOTP_process also polls the ISOTP library's non-blocking receive method, which will produce a message if one is ready.
	xTaskCreate(isotp_send_queue_task, "ISOTP_process_send_queue", TASK_STACK_SIZE, NULL, MAIN_TSK_PRIO, NULL);
	xSemaphoreTake(sync_task_sem, portMAX_DELAY);
	ESP_LOGI(BRIDGE_TAG, "Tasks started");
}

void isotp_stop_task()
{
	if (isotp_allow_run_tasks()) {
		isotp_set_run_tasks(false);
		for (uint16_t i = 0; i < NUM_ISOTP_LINK_CONTAINERS; i++) {
			IsoTpLinkContainer* isotp_link_container = &isotp_link_containers[i];
			xSemaphoreGive(isotp_link_container->wait_for_isotp_data_sem);
			xSemaphoreTake(isotp_link_container->task_mutex, portMAX_DELAY);
			xSemaphoreGive(isotp_link_container->task_mutex);
		}

		send_message_t msg;
		xQueueSend(isotp_send_message_queue, &msg, portMAX_DELAY);
		xSemaphoreTake(isotp_send_task_mutex, portMAX_DELAY);
		xSemaphoreGive(isotp_send_task_mutex);
		ESP_LOGI(BRIDGE_TAG, "Tasks stopped");
	}
}

bool16 isotp_allow_run_tasks()
{
	tMUTEX(isotp_settings_mutex);
		bool16 run_tasks = isotp_run_tasks;
	rMUTEX(isotp_settings_mutex);

	return run_tasks;
}

void isotp_set_run_tasks(bool16 allow)
{
	tMUTEX(isotp_settings_mutex);
		isotp_run_tasks = allow;
	rMUTEX(isotp_settings_mutex);
}

/* ----------- Receive packet functions ---------------- */

void split_clear()
{
	memset(&split_header, 0, sizeof(ble_header_t));
	split_enabled = false;
	split_count = 0;
	split_length = 0;
	if(split_data) {
		free(split_data);
		split_data = NULL;
	}
}

bool16 parse_packet(ble_header_t* header, uint8_t* data)
{
	if(get_password_checked()) {
		//Is client trying to set a setting?
		if(header->cmdFlags & BLE_COMMAND_FLAG_SETTINGS)
		{
			//Are we setting or getting?
			if(header->cmdFlags & BLE_COMMAND_FLAG_SETTINGS_GET)
			{   //Make sure payload is empty
				if(header->cmdSize == 0)
				{
					//send requested information
					switch(header->cmdFlags ^ (BLE_COMMAND_FLAG_SETTINGS | BLE_COMMAND_FLAG_SETTINGS_GET))
					{
						case BRG_SETTING_ISOTP_STMIN:
							for(uint16_t i = 0; i < NUM_ISOTP_LINK_CONTAINERS; i++)
							{
								IsoTpLinkContainer *isotp_link_container = &isotp_link_containers[i];
								if(header->rxID == isotp_link_container->link.receive_arbitration_id &&
									header->txID == isotp_link_container->link.send_arbitration_id)
								{
									uint16_t stmin = isotp_link_container->link.stmin_override;
									ESP_LOGI(BRIDGE_TAG, "Sending stmin [%04X] from container [%02X]", stmin, i);
									send_packet(isotp_link_container->link.receive_arbitration_id, isotp_link_container->link.send_arbitration_id, BLE_COMMAND_FLAG_SETTINGS | BRG_SETTING_ISOTP_STMIN, &stmin, sizeof(uint16_t));
								}
							}
							break;
						case BRG_SETTING_LED_COLOR:
							{
								uint32_t color = led_getcolor();
								ESP_LOGI(BRIDGE_TAG, "Sending color [%06X]", color);
								send_packet(0, 0, BLE_COMMAND_FLAG_SETTINGS | BRG_SETTING_LED_COLOR, &color, sizeof(uint32_t));
							}
							break;
						case BRG_SETTING_PERSIST_DELAY:
							{
								uint16_t delay = persist_get_delay();
								ESP_LOGI(BRIDGE_TAG, "Sending persist delay [%04X]", delay);
								send_packet(0, 0, BLE_COMMAND_FLAG_SETTINGS | BRG_SETTING_PERSIST_DELAY, &delay, sizeof(uint16_t));
							}
							break;
						case BRG_SETTING_PERSIST_Q_DELAY:
							{
								uint16_t delay = persist_get_q_delay();
								ESP_LOGI(BRIDGE_TAG, "Sending persist queue delay [%04X]", delay);
								send_packet(0, 0, BLE_COMMAND_FLAG_SETTINGS | BRG_SETTING_PERSIST_Q_DELAY, &delay, sizeof(uint16_t));
							}
							break;
						case BRG_SETTING_BLE_SEND_DELAY:
							{
								uint16_t delay = ble_get_delay_send();
								ESP_LOGI(BRIDGE_TAG, "Sending BLE send delay [%04X]", delay);
								send_packet(0, 0, BLE_COMMAND_FLAG_SETTINGS | BRG_SETTING_BLE_SEND_DELAY, &delay, sizeof(uint16_t));
							}
							break;
						case BRG_SETTING_BLE_MULTI_DELAY:
							{
								uint16_t delay = ble_get_delay_multi();
								ESP_LOGI(BRIDGE_TAG, "Sending BLE send delay [%04X]", delay);
								send_packet(0, 0, BLE_COMMAND_FLAG_SETTINGS | BRG_SETTING_BLE_MULTI_DELAY, &delay, sizeof(uint16_t));
							}
							break;
						case BRG_SETTING_GAP:
							{
								char str[MAX_GAP_LENGTH+1];
								ble_get_gap_name(str);
								uint8_t len = strlen(str);
								send_packet(0, 0, BLE_COMMAND_FLAG_SETTINGS | BRG_SETTING_GAP, &str, len);
								ESP_LOGI(BRIDGE_TAG, "Set GAP [%s]", str);
							}
							break;
					}
					return true;
				}
			} else { //Setting
				switch(header->cmdFlags ^ BLE_COMMAND_FLAG_SETTINGS)
				{
					case BRG_SETTING_ISOTP_STMIN:
						//check size
						if(header->cmdSize == sizeof(uint16_t))
						{   //match rx/tx
							for(uint16_t i = 0; i < NUM_ISOTP_LINK_CONTAINERS; i++)
							{
								IsoTpLinkContainer *isotp_link_container = &isotp_link_containers[i];
								if(header->rxID == isotp_link_container->link.receive_arbitration_id &&
									header->txID == isotp_link_container->link.send_arbitration_id)
								{
									uint16_t* stmin = (uint16_t*)data;
									isotp_link_container->link.stmin_override = *stmin;
									ESP_LOGI(BRIDGE_TAG, "Set stmin [%04X] on container [%02X]", *stmin, i);
									return true;
								}
							}
						}
						break;
					case BRG_SETTING_LED_COLOR:
						//check size
						if(header->cmdSize == sizeof(uint32_t))
						{
							uint32_t* color = (uint32_t*)data;
							led_setcolor(*color);
							ESP_LOGI(BRIDGE_TAG, "Set led color [%08X]", *color);
							return true;
						}
						break;
					case BRG_SETTING_PERSIST_DELAY:
						//check size
						if(header->cmdSize == sizeof(uint16_t))
						{   //confirm correct command size
							uint16_t* delay = (uint16_t*)data;
							persist_set_delay(*delay);
							ESP_LOGI(BRIDGE_TAG, "Set persist delay [%08X]", *delay);
							return true;
						}
						break;
					case BRG_SETTING_PERSIST_Q_DELAY:
						//check size
						if(header->cmdSize == sizeof(uint16_t))
						{   //confirm correct command size
							uint16_t* delay = (uint16_t*)data;
							persist_set_q_delay(*delay);
							ESP_LOGI(BRIDGE_TAG, "Set persist queue delay [%08X]", *delay);
							return true;
						}
						break;
					case BRG_SETTING_BLE_SEND_DELAY:
						//check size
						if(header->cmdSize == sizeof(uint16_t))
						{   //confirm correct command size
							uint16_t* delay = (uint16_t*)data;
							ble_set_delay_send(*delay);
							ESP_LOGI(BRIDGE_TAG, "Set BLE send delay [%08X]", *delay);
							return true;
						}
						break;
					case BRG_SETTING_BLE_MULTI_DELAY:
						//check size
						if(header->cmdSize == sizeof(uint16_t))
						{   //confirm correct command size
							uint16_t* delay = (uint16_t*)data;
							ble_set_delay_multi(*delay);
							ESP_LOGI(BRIDGE_TAG, "Set BLE wait for queue item [%08X]", *delay);
							return true;
						}
						break;
					case BRG_SETTING_PASSWORD:
						//check size
						if(header->cmdSize <= MAX_PASSWORD_LENGTH)
						{   //confirm correct command size
							char* str = malloc(header->cmdSize+1);
							if(str) {
								str[header->cmdSize] = 0;
								memcpy(str, data, header->cmdSize);
								write_password((char*)str);
								free(str);
								return true;
							}
						}
						break;
					case BRG_SETTING_GAP:
						if(header->cmdSize <= MAX_GAP_LENGTH)
						{   //confirm correct command size
							char str[MAX_GAP_LENGTH+1];
                            memcpy(str, (char*)data, header->cmdSize);
							str[header->cmdSize] = 0;
							ble_set_gap_name(str, true);
							eeprom_write_str(BLE_GAP_KEY, str);
							eeprom_commit();
							ch_give_sleep_sem();
							return true;
						}
						break;
				}
			}
		} else {
			if (persist_enabled()) {
				//We are in persistent mode
				//Should we clear the persist messages in memory?
				if (header->cmdFlags & BLE_COMMAND_FLAG_PER_CLEAR)
				{
					persist_clear();
				}

				//Should we disable persist mode?
				if ((header->cmdFlags & BLE_COMMAND_FLAG_PER_ENABLE) == 0)
				{
					persist_set(false);
				} else {
					//If we are still in persist mode only accept setting changes
					return false;
				}
			} else {
				//Not in persistent mode
				if (header->cmdFlags & BLE_COMMAND_FLAG_PER_CLEAR)
				{
					persist_clear();
				}

				if (header->cmdFlags & BLE_COMMAND_FLAG_PER_ADD)
				{
					persist_add(header->txID, header->rxID, data, header->cmdSize);
				}

				if (header->cmdFlags & BLE_COMMAND_FLAG_PER_ENABLE)
				{
					persist_set(true);
					return false;
				}
			}

			if (header->cmdSize)
			{
				if (!persist_enabled())
				{
					ESP_LOGI(BRIDGE_TAG, "Received message [%04X]", header->cmdSize);

					send_message_t msg;
					msg.msg_length = header->cmdSize;
					msg.rxID = header->txID;
					msg.txID = header->rxID;
					msg.buffer = malloc(header->cmdSize);
					if (msg.buffer) {
						memcpy(msg.buffer, data, header->cmdSize);

						if (xQueueSend(isotp_send_message_queue, &msg, pdMS_TO_TICKS(TIMEOUT_NORMAL)) != pdTRUE) {
							free(msg.buffer);
						}
					}
					else {
						ESP_LOGD(BRIDGE_TAG, "parse_packet: malloc fail size(%d)", header->cmdSize);
						return false;
					}
				}

				return true;
			}
		}
	} else {
		//password has not be accepted yet, check for password
		if(header->cmdFlags & BRG_SETTING_PASSWORD & BLE_COMMAND_FLAG_SETTINGS & BLE_COMMAND_FLAG_SETTINGS_GET) {
			//check size
			if(header->cmdSize <= MAX_PASSWORD_LENGTH)
			{
				char* str = malloc(header->cmdSize+1);
				if(str) {
					str[header->cmdSize] = 0;
					memcpy(str, data, header->cmdSize);

					char c = 0;
					if(check_password((char*)data)) {
						ESP_LOGI(BRIDGE_TAG, "Password accepted [%s]", data);
						c = 0xFF;
					}

					send_packet(0xFF, 0xFF, 0xFF, &c, 1);
					free(str);
				}
				return false;
			}
		} else {
			ESP_LOGI(BRIDGE_TAG, "No access");
		}
	}

	return false;
}

void packet_received(const void* src, size_t size)
{
	tMUTEX(isotp_receive_mutex);
		//store current data pointer
		uint8_t* data = (uint8_t*)src;

		//Are we in Split packet mode?
		if(split_enabled && data[0] == BLE_PARTIAL_ID) {
			//check packet count
			if(data[1] == split_count) {
				if (split_data && split_length) {
					ESP_LOGI(BRIDGE_TAG, "Split packet [%02X] adding [%02X]", split_count, size - 2);

					uint8_t* new_data = malloc(split_length + size - 2);
					if (new_data == NULL) {
						ESP_LOGI(BRIDGE_TAG, "malloc error %s %d", __func__, __LINE__);
						split_clear();
						goto release_mutex;
					}
					memcpy(new_data, split_data, split_length);
					memcpy(new_data + split_length, data + 2, size - 2);
					free(split_data);
					split_data = new_data;
					split_length += size - 2;
					split_count++;

					//got complete message
					if (split_length == split_header.cmdSize)
					{   //Messsage size matches
						ESP_LOGI(BRIDGE_TAG, "Split packet size matches [%02X]", split_length);
						parse_packet(&split_header, split_data);
						split_clear();
					}
					else if (split_length > split_header.cmdSize)
					{   //Message size does not match
						ESP_LOGI(BRIDGE_TAG, "Command size is larger than packet size [%02X, %02X]", split_header.cmdSize, split_length);
						split_clear();
					}
				} else {
					//error delete and forget
					ESP_LOGI(BRIDGE_TAG, "Splitpacket data is invalid");
					split_clear();
				}
			} else {
				//error delete and forget
				ESP_LOGI(BRIDGE_TAG, "Splitpacket out of order [%02X, %02X]", data[1], split_count);
				split_clear();
			}
		} else {
			//disable splitpacket
			split_enabled = false;

			//If the packet does not contain header abort
			while(size >= sizeof(ble_header_t))
			{
				//Confirm the header is valid
				ble_header_t* header = (ble_header_t*)data;
				if(header->hdID != BLE_HEADER_ID)
				{
					ESP_LOGI(BRIDGE_TAG, "Packet header does not match [%02X, %02X]", header->hdID, BLE_HEADER_ID);
					goto release_mutex;
				}

				//header has pointer, skip past header in data
				data += sizeof(ble_header_t);
				size -= sizeof(ble_header_t);

				//Confirm data size is legit
				if(header->cmdSize > size)
				{
					//Are they requesting splitpacket?
					if(header->cmdFlags & BLE_COMMAND_FLAG_SPLIT_PK)
					{
						ESP_LOGI(BRIDGE_TAG, "Starting split packet [%02X]", header->cmdSize);
						split_clear();
						split_enabled = true;
						split_data = malloc(size);
						if(split_data == NULL){
							ESP_LOGI(BRIDGE_TAG, "malloc error %s %d", __func__, __LINE__);
							split_clear();
							goto release_mutex;
						}
						memcpy(split_data, data, size);
						memcpy(&split_header, header, sizeof(ble_header_t));
						split_count = 1;
						split_length = size;
						goto release_mutex;
					} else {
						ESP_LOGI(BRIDGE_TAG, "Command size is larger than packet size [%02X, %02X]", header->cmdSize, size);
						goto release_mutex;
					}
				}

				//looks good, parse the packet
				if(parse_packet(header, data))
				{
					data += header->cmdSize;
					size -= header->cmdSize;
				} else {
					goto release_mutex;
				}
			}
		}
release_mutex:
	rMUTEX(isotp_receive_mutex);
}

void received_from_ble(const void* src, size_t size)
{
	packet_received(src, size);
}

void uart_data_received(const void* src, size_t size)
{
	if(!ble_connected()) {
		ch_reset_uart_timer();
		packet_received(src, size);
#ifdef UART_ECHO
		send_packet(0xFF, 0xFF, 0, src+sizeof(ble_header_t), size-sizeof(ble_header_t));
#endif
	}
}

/* ----------- BLE/UART callbacks ---------------- */

void bridge_connect()
{
	//clear persist
	persist_clear();

	//set to green
	led_setcolor(LED_GREEN_QRT);

	//disable password support
	set_password_checked(false);
}

void bridge_disconnect()
{
	//clear persist
	persist_clear();

	//set led to low red
	led_setcolor(LED_RED_EHT);

	//disable password support
	set_password_checked(false);

#ifdef RESET_ON_DISCONNECT
	ch_give_sleep_sem();
#endif
}

void ch_on_uart_connect()
{
	bridge_connect();
}

void ch_on_uart_disconnect()
{
	bridge_disconnect();
}