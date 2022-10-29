#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "mutexes.h"
#include "queues.h"
#include "ble_server.h"
#include "constants.h"
#include "led.h"
#include "persist.h"
#include "isotp.h"
#include "isotp_link_containers.h"
#include "connection_handler.h"

#define PERSIST_TAG	"Persist"

typedef struct {
	send_message_t messages[PERSIST_MAX_MESSAGE];
	uint16_t			number;
	uint16_t			count;
	uint16_t			position;
	uint16_t			rxID;
	uint16_t			txID;
	int64_t				last_packet;
	SemaphoreHandle_t	data_mutex;
	SemaphoreHandle_t	task_mutex;
	SemaphoreHandle_t	send_sema;
} persist_t;

static SemaphoreHandle_t    persist_settings_mutex	= NULL;
static bool16				persist_run_task		= false;
static uint16_t				persist_msg_delay		= PERSIST_DEFAULT_MESSAGE_DELAY;
static uint16_t				persist_msg_qdelay		= PERSIST_DEFAULT_QUEUE_DELAY;
static bool16				persist_msg_enabled		= false;
static persist_t			persist_msgs[PERSIST_COUNT];

void persist_task(void *arg);

void persist_set_run_task(bool16 allow)
{
	tMUTEX(persist_settings_mutex);
		persist_run_task = allow;
	rMUTEX(persist_settings_mutex);
}

bool16 persist_allow_run_task()
{
	tMUTEX(persist_settings_mutex);
		bool16 run_task = persist_run_task;
	rMUTEX(persist_settings_mutex);

	return run_task;
}

void persist_init()
{
	persist_deinit();

	persist_settings_mutex = xSemaphoreCreateMutex();

	for (uint16_t i = 0; i < PERSIST_COUNT; i++) {
		persist_t* pPersist			= &persist_msgs[i];
		pPersist->number			= i;
		pPersist->count				= 0;
		pPersist->position			= 0;
		pPersist->last_packet		= 0;
		pPersist->rxID				= isotp_link_containers[i].link.send_arbitration_id;
		pPersist->txID				= isotp_link_containers[i].link.receive_arbitration_id;
		pPersist->data_mutex		= xSemaphoreCreateMutex();
		pPersist->task_mutex		= xSemaphoreCreateMutex();
		pPersist->send_sema			= xSemaphoreCreateBinary();
	}

	ESP_LOGI(PERSIST_TAG, "Init");
}

void persist_deinit()
{
	bool16 didDeInit = false;

	for (uint16_t i = 0; i < PERSIST_COUNT; i++) {
		if (persist_msgs[i].data_mutex) {
			vSemaphoreDelete(persist_msgs[i].data_mutex);
			persist_msgs[i].data_mutex = NULL;
			didDeInit = true;
		}

		if (persist_msgs[i].task_mutex) {
			vSemaphoreDelete(persist_msgs[i].task_mutex);
			persist_msgs[i].task_mutex = NULL;
			didDeInit = true;
		}

		if (persist_msgs[i].send_sema) {
			vSemaphoreDelete(persist_msgs[i].send_sema);
			persist_msgs[i].send_sema = NULL;
			didDeInit = true;
		}
	}

	if (persist_settings_mutex) {
		vSemaphoreDelete(persist_settings_mutex);
		persist_settings_mutex = NULL;
		didDeInit = true;
	}

	if(didDeInit)
		ESP_LOGI(PERSIST_TAG, "Deinit");
}

void persist_start_task()
{
	persist_stop_task();
	persist_set_run_task(true);

	ESP_LOGI(PERSIST_TAG, "Tasks starting");
	xSemaphoreTake(sync_task_sem, 0);
	for (uint16_t i = 0; i < PERSIST_COUNT; i++) {
		persist_t* pPersist = &persist_msgs[i];
		xTaskCreate(persist_task, "PERSIST_process", TASK_STACK_SIZE, pPersist, PERSIST_TSK_PRIO, NULL);
		xSemaphoreTake(sync_task_sem, portMAX_DELAY);
	}
	ESP_LOGI(PERSIST_TAG, "Tasks started");
}

void persist_stop_task()
{
	if (persist_allow_run_task()) {
		persist_set_run_task(false);

		for (uint16_t i = 0; i < PERSIST_COUNT; i++) {
			persist_t* pPersist = &persist_msgs[i];
			tMUTEX(pPersist->task_mutex);
			rMUTEX(pPersist->task_mutex);
		}

		persist_clear();

		ESP_LOGI(PERSIST_TAG, "Tasks stopped");
	}
}

void persist_allow_send(uint16_t persist)
{
	xSemaphoreGive(persist_msgs[persist].send_sema);
}

uint16_t persist_enabled()
{
	tMUTEX(persist_settings_mutex);
		uint16_t msg_enabled = persist_msg_enabled;
	rMUTEX(persist_settings_mutex);

	return msg_enabled;
}

void persist_set(uint16_t enable)
{
	tMUTEX(persist_settings_mutex);
		persist_msg_enabled = enable;
	rMUTEX(persist_settings_mutex);

	ESP_LOGI(PERSIST_TAG, "Enabled: %d", enable);
}

int16_t persist_add(uint16_t rx, uint16_t tx, const void* src, size_t size)
{
	//check for valid message
	if (!src || size == 0)
		return false;

	//set persist pointer
	persist_t* pPersist = NULL;
	uint16_t pCount = 0;
	for (uint16_t i = 0; i < PERSIST_COUNT; i++)
	{
		persist_t* persist = &persist_msgs[i];
		tMUTEX(persist->data_mutex);
			if (persist->rxID == rx && persist->txID == tx) {
				pPersist	= persist;
				pCount		= persist->count;
			}
		rMUTEX(persist->data_mutex);
		if (pPersist)
			break;
	}
	if (pPersist == NULL || pCount >= PERSIST_MAX_MESSAGE)
		return false;

	//add new persist message
	bool16 msg_added = false;
	tMUTEX(pPersist->data_mutex);
		send_message_t* pMsg	= &pPersist->messages[pPersist->count++];
		pMsg->rxID				= rx;
		pMsg->txID				= tx;
		pMsg->msg_length		= 0;
		pMsg->buffer			= malloc(size);
		if(pMsg->buffer){
			memcpy(pMsg->buffer, src, size);
			pMsg->msg_length = size;
			msg_added = true;
		}
	rMUTEX(pPersist->data_mutex);

	if (msg_added) {
		ESP_LOGI(PERSIST_TAG, "Message added with size: %04X", size);
	}
	else {
		ESP_LOGI(PERSIST_TAG, "Error adding message: malloc error %s %d", __func__, __LINE__);
	}

	return msg_added;
}

void persist_clear()
{
	persist_set(false);
	for (uint16_t d = 0; d < PERSIST_COUNT; d++) {
		//clear all messages
		persist_t* pPersist = &persist_msgs[d];
		tMUTEX(pPersist->data_mutex);
			for (uint16_t i = 0; i < pPersist->count; i++)
			{
				send_message_t* pMsg = &pPersist->messages[i];
				if (pMsg->buffer)
				{
					free(pMsg->buffer);
					pMsg->buffer = NULL;
				}
				pMsg->msg_length = 0;
			}
			pPersist->position = 0;
			pPersist->count = 0;
		rMUTEX(pPersist->data_mutex);
	}

	ESP_LOGI(PERSIST_TAG, "Messages cleared");
}

bool16 persist_send(persist_t* pPersist)
{
	tMUTEX(pPersist->data_mutex);
		//If persist mode is disabled abort
		if (!persist_enabled() || !pPersist->count)
		{
			rMUTEX(pPersist->data_mutex);
			return false;
		}

		//don't flood the queues
		if (uxQueueMessagesWaiting(isotp_send_message_queue) || !ble_queue_spaces())
		{
			ESP_LOGW(PERSIST_TAG, "Unable to send message: queues are full");
			rMUTEX(pPersist->data_mutex);
			xSemaphoreGive(pPersist->send_sema);
			return false;
		}

		//Cycle through messages
		if (pPersist->position >= pPersist->count)
			pPersist->position = 0;

		//Make sure the message has length
		uint16_t mPos = pPersist->position++;
		send_message_t* pMsg = &pPersist->messages[mPos];
		if (pMsg->msg_length == 0)
		{
			ESP_LOGW(PERSIST_TAG, "Unable to send message: message length 0");
			rMUTEX(pPersist->data_mutex);
			xSemaphoreGive(pPersist->send_sema);
			return false;
		}

		//We are good, lets send the message!
		send_message_t msg;
		msg.buffer = malloc(pMsg->msg_length);
		if (msg.buffer == NULL) {
			ESP_LOGW(PERSIST_TAG, "Unable to send message: malloc error %s %d", __func__, __LINE__);
			rMUTEX(pPersist->data_mutex);
			xSemaphoreGive(pPersist->send_sema);
			return false;
		}

		//copy persist message into a new container
		memcpy(msg.buffer, pMsg->buffer, pMsg->msg_length);
		msg.msg_length = pMsg->msg_length;
		msg.rxID = pMsg->rxID;
		msg.txID = pMsg->txID;
	rMUTEX(pPersist->data_mutex);

	//if we fail to place message into the queue free the memory!
	if (xQueueSend(isotp_send_message_queue, &msg, pdMS_TO_TICKS(TIMEOUT_SHORT)) != pdTRUE) {
		free(msg.buffer);
		xSemaphoreGive(pPersist->send_sema);
		return false;
	}
	else {
		ESP_LOGD(PERSIST_TAG, "Message sent with size: %04X", msg.msg_length);
	}

	return true;
}

void persist_task(void *arg)
{
	persist_t *pPersist = ((persist_t*)arg);
	tMUTEX(pPersist->task_mutex);
		tMUTEX(pPersist->data_mutex);
			ESP_LOGI(PERSIST_TAG, "Task started: %d", pPersist->number);
		rMUTEX(pPersist->data_mutex);
		xSemaphoreGive(sync_task_sem);
		while(persist_allow_run_task()) {
			xSemaphoreTake(pPersist->send_sema, pdMS_TO_TICKS(TIMEOUT_NORMAL));
			persist_send(pPersist);
			int32_t required_delay = persist_get_delay() + (ble_queue_waiting() * persist_get_q_delay());
			int64_t current_time = esp_timer_get_time();
			int32_t current_delay = required_delay - ((current_time - pPersist->last_packet) / 1000);
			if (current_delay < 0)
				current_delay = 0;
			pPersist->last_packet = current_time;
			vTaskDelay(pdMS_TO_TICKS(current_delay));
		}
		tMUTEX(pPersist->data_mutex);
			ESP_LOGI(PERSIST_TAG, "Task stopped: %d", pPersist->number);
		rMUTEX(pPersist->data_mutex);
	rMUTEX(pPersist->task_mutex);
	vTaskDelete(NULL);
}

void persist_set_delay(uint16_t delay)
{
	tMUTEX(persist_settings_mutex);
		persist_msg_delay = delay;
	rMUTEX(persist_settings_mutex);

	ESP_LOGI(PERSIST_TAG, "Set delay: %d", delay);
}

void persist_set_q_delay(uint16_t delay)
{
	tMUTEX(persist_settings_mutex);
		persist_msg_qdelay = delay;
	rMUTEX(persist_settings_mutex);

	ESP_LOGI(PERSIST_TAG, "Set q delay: %d", delay);
}

uint16_t persist_get_delay()
{
	tMUTEX(persist_settings_mutex);
		uint16_t msg_delay = persist_msg_delay;
	rMUTEX(persist_settings_mutex);

	return msg_delay;
}

uint16_t persist_get_q_delay()
{
	tMUTEX(persist_settings_mutex);
		uint16_t q_delay = persist_msg_qdelay;
	rMUTEX(persist_settings_mutex);

	return q_delay;
}