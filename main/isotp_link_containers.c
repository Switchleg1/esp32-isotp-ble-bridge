#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "isotp.h"
#include "isotp_link_containers.h"
#include "constants.h"

#define LINKS_TAG 		"ISOTP_LINKS"

void configure_isotp_links()
{
	disable_isotp_links();

	// RX_ID + TX_ID are flipped because this device acts as a "tester" listening for responses from ECUs. the ECU's TX is our RX
	// TODO: make these configurable via j2534 filters
	IsoTpLinkContainer *ecu_isotp_link_container	= &isotp_link_containers[0];
	IsoTpLinkContainer *tcu_isotp_link_container	= &isotp_link_containers[1];
	IsoTpLinkContainer *haldex_isotp_link_container = &isotp_link_containers[2];
	IsoTpLinkContainer *dtc_isotp_link_container	= &isotp_link_containers[3];

	// ECU
	ecu_isotp_link_container->number = 0;
	strcpy(ecu_isotp_link_container->name, "isotp_container_ecu");
	ecu_isotp_link_container->buffer_size = ISOTP_BUFFER_SIZE;
	ecu_isotp_link_container->recv_buf = calloc(1, ecu_isotp_link_container->buffer_size);
	ecu_isotp_link_container->send_buf = calloc(1, ecu_isotp_link_container->buffer_size);
	ecu_isotp_link_container->payload_buf = calloc(1, ecu_isotp_link_container->buffer_size);
	assert(ecu_isotp_link_container->recv_buf != NULL);
	assert(ecu_isotp_link_container->send_buf != NULL);
	assert(ecu_isotp_link_container->payload_buf != NULL);
	isotp_init_link(
		&ecu_isotp_link_container->link,
		0x7E0, 0x7E8,
		ecu_isotp_link_container->send_buf, ecu_isotp_link_container->buffer_size,
		ecu_isotp_link_container->recv_buf, ecu_isotp_link_container->buffer_size
	);

	// TCU
	tcu_isotp_link_container->number = 1;
	strcpy(tcu_isotp_link_container->name, "isotp_container_tcu");
	tcu_isotp_link_container->buffer_size = ISOTP_BUFFER_SIZE;
	tcu_isotp_link_container->recv_buf = calloc(1, tcu_isotp_link_container->buffer_size);
	tcu_isotp_link_container->send_buf = calloc(1, tcu_isotp_link_container->buffer_size);
	tcu_isotp_link_container->payload_buf = calloc(1, tcu_isotp_link_container->buffer_size);
	assert(tcu_isotp_link_container->recv_buf != NULL);
	assert(tcu_isotp_link_container->send_buf != NULL);
	assert(tcu_isotp_link_container->payload_buf != NULL);
	isotp_init_link(
		&tcu_isotp_link_container->link,
		0x7E1, 0x7E9,
		tcu_isotp_link_container->send_buf, tcu_isotp_link_container->buffer_size,
		tcu_isotp_link_container->recv_buf, tcu_isotp_link_container->buffer_size
	);

	// HALDEX
	haldex_isotp_link_container->number = 2;
	strcpy(haldex_isotp_link_container->name, "isotp_container_haldex");
	haldex_isotp_link_container->buffer_size = ISOTP_BUFFER_SIZE;
	haldex_isotp_link_container->recv_buf = calloc(1, haldex_isotp_link_container->buffer_size);
	haldex_isotp_link_container->send_buf = calloc(1, haldex_isotp_link_container->buffer_size);
	haldex_isotp_link_container->payload_buf = calloc(1, haldex_isotp_link_container->buffer_size);
	assert(haldex_isotp_link_container->recv_buf != NULL);
	assert(haldex_isotp_link_container->send_buf != NULL);
	assert(haldex_isotp_link_container->payload_buf != NULL);
	isotp_init_link(
		&haldex_isotp_link_container->link,
		0x7E5, 0x7ED,
		haldex_isotp_link_container->send_buf, haldex_isotp_link_container->buffer_size,
		haldex_isotp_link_container->recv_buf, haldex_isotp_link_container->buffer_size
	);

	// DTC
	dtc_isotp_link_container->number = 3;
	strcpy(dtc_isotp_link_container->name, "isotp_container_dtc");
	dtc_isotp_link_container->buffer_size = ISOTP_BUFFER_SIZE_SMALL;
	dtc_isotp_link_container->recv_buf = calloc(1, dtc_isotp_link_container->buffer_size);
	dtc_isotp_link_container->send_buf = calloc(1, dtc_isotp_link_container->buffer_size);
	dtc_isotp_link_container->payload_buf = calloc(1, dtc_isotp_link_container->buffer_size);
	assert(dtc_isotp_link_container->recv_buf != NULL);
	assert(dtc_isotp_link_container->send_buf != NULL);
	assert(dtc_isotp_link_container->payload_buf != NULL);
	isotp_init_link(
		&dtc_isotp_link_container->link,
		0x700, 0x7E8,
		dtc_isotp_link_container->send_buf, dtc_isotp_link_container->buffer_size,
		dtc_isotp_link_container->recv_buf, dtc_isotp_link_container->buffer_size
	);

	//create semaphores/mutexs for each link
	for(uint16_t i = 0; i < NUM_ISOTP_LINK_CONTAINERS; i++)
	{
		IsoTpLinkContainer *isotp_link_container = &isotp_link_containers[i];
		isotp_link_container->wait_for_isotp_data_sem	= xSemaphoreCreateBinary();
		isotp_link_container->task_mutex				= xSemaphoreCreateMutex();
		isotp_link_container->data_mutex				= xSemaphoreCreateMutex();
	}

	//reset default link container id
	isotp_link_container_id = 0;

	ESP_LOGI(LINKS_TAG, "Init");
}

void disable_isotp_links()
{
	bool16 didDeinit = false;

	//delete semaphores/mutexs for each link
	for(uint16_t i = 0; i < NUM_ISOTP_LINK_CONTAINERS; i++)
	{
		IsoTpLinkContainer *isotp_link_container = &isotp_link_containers[i];
		if (isotp_link_container->wait_for_isotp_data_sem) {
			vSemaphoreDelete(isotp_link_container->wait_for_isotp_data_sem);
			isotp_link_container->wait_for_isotp_data_sem = NULL;
			didDeinit = true;
		}
		
		if (isotp_link_container->task_mutex) {
			vSemaphoreDelete(isotp_link_container->task_mutex);
			isotp_link_container->task_mutex = NULL;
			didDeinit = true;
		}

		if (isotp_link_container->data_mutex) {
			vSemaphoreDelete(isotp_link_container->data_mutex);
			isotp_link_container->data_mutex = NULL;
			didDeinit = true;
		}

		if (isotp_link_container->recv_buf) {
			free(isotp_link_container->recv_buf);
			isotp_link_container->recv_buf = NULL;
			didDeinit = true;
		}

		if (isotp_link_container->send_buf) {
			free(isotp_link_container->send_buf);
			isotp_link_container->send_buf = NULL;
			didDeinit = true;
		}

		if (isotp_link_container->payload_buf) {
			free(isotp_link_container->payload_buf);
			isotp_link_container->payload_buf = NULL;
			didDeinit = true;
		}
	}

	if(didDeinit)
		ESP_LOGI(LINKS_TAG, "Deinit");
}
