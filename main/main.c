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
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
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

#define MAIN_TAG	"Main"

void app_main(void)
{
	ESP_LOGI(MAIN_TAG, "Application starting");

#if !CONFIG_ESP_TASK_WDT_INIT
    // If the WDT was not initialized automatically on startup, manually intialize it now
    ESP_ERROR_CHECK(esp_task_wdt_init(WDT_TIMEOUT_S, true));
    ESP_LOGI(MAIN_TAG, "WDT initialized");
#endif

	//subscribe to WDT
	ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
	ESP_ERROR_CHECK(esp_task_wdt_status(NULL));

	//create our sleep semaphore
	sync_task_sem = xSemaphoreCreateBinary();

	//init
	eeprom_init();
	ble_server_init();
	ch_init();
	led_init();
	uart_init();
	twai_init();
	isotp_init();
	persist_init();

	//start eeprom and read values
	char* gapName = eeprom_read_str(BLE_GAP_KEY);
	if(gapName) {
		ble_set_gap_name(gapName, false);
		free(gapName);
	}

#if SLEEP_MODE == 1
	while(1) {
#endif
		//setup BLE server
		ble_server_callbacks callbacks = {
			.data_received = received_from_ble,
			.notifications_subscribed = bridge_connect,
			.notifications_unsubscribed = bridge_disconnect
		};

		//start tasks
		ble_server_start(callbacks);
		twai_start_task();
		isotp_start_task();
		persist_start_task();
		uart_start_task();
		ch_start_task();

		//wait for sleep command
		while(ch_take_sleep_sem() != pdTRUE)
			esp_task_wdt_reset();

		//stop ble connections
		ble_stop_advertising();
		while (ble_connected()) {
			vTaskDelay(pdMS_TO_TICKS(TIMEOUT_NORMAL));
			esp_task_wdt_reset();
		}
	
		//stop tasks
		ch_stop_task();
		uart_stop_task();
		persist_stop_task();
		isotp_stop_task();
		twai_stop_task();
		ble_server_stop();

		//setup sleep timer
		esp_sleep_enable_timer_wakeup(SLEEP_TIME * US_TO_S);
		ESP_LOGI(MAIN_TAG, "Timeout - Sleeping [%ds]", SLEEP_TIME);

#if SLEEP_MODE == 1
		vTaskDelay(pdMS_TO_TICKS(TIMEOUT_LONG));
		esp_task_wdt_reset();
		esp_light_sleep_start();
		esp_task_wdt_reset();
	}
#endif

	//deinit
	persist_deinit();
	isotp_deinit();
	twai_deinit();
	uart_deinit();
	led_deinit();
	ch_deinit();
	ble_server_deinit();

	//unsubscribe to WDT and deinit
	ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));

	//Go to sleep now
	esp_deep_sleep_start();
}