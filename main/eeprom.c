#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "eeprom.h"
#include "constants.h"
#include "ble_server.h"

#define EEPROM_TAG 			"EEPROM"
#define STORAGE_NAMESPACE	"EEPROM"

nvs_handle_t eeprom_handle = 0;

esp_err_t eeprom_init()
{
	if(!eeprom_handle) {
		ESP_LOGI(EEPROM_TAG, "Initializing EEPROM");

		esp_err_t err = nvs_flash_init();
		if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
			// NVS partition was truncated and needs to be erased
			// Retry nvs_flash_init
			ESP_ERROR_CHECK(nvs_flash_erase());
			err = nvs_flash_init();
			eeprom_defaults();
		}
		ESP_ERROR_CHECK( err );

		// Open
		err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &eeprom_handle);
		if (err != ESP_OK) {
			eeprom_handle = 0;
			ESP_LOGI(EEPROM_TAG, "Unable to initialize EEPROM");
			return err;
		}

		ESP_LOGI(EEPROM_TAG, "Initialized EEPROM");
		return ESP_OK;
	}

	ESP_LOGI(EEPROM_TAG, "Already initialized");
	return ESP_FAIL;
}

void eeprom_deinit()
{
	if(eeprom_handle) {
		// Close
		eeprom_commit();
		nvs_close(eeprom_handle);
		eeprom_handle = 0;

		ESP_LOGI(EEPROM_TAG, "Deitialized EEPROM");
	}
}

esp_err_t eeprom_commit()
{
	if(eeprom_handle)
		return nvs_commit(eeprom_handle);

	ESP_LOGI(EEPROM_TAG, "Commit failed - not initialized");
	return ESP_FAIL;
}

void eeprom_defaults()
{
	ESP_LOGI(EEPROM_TAG, "Write defaults");

	eeprom_write_str(PASSWORD_KEY, PASSWORD_DEFAULT);
	eeprom_write_str(BLE_GAP_KEY, DEFAULT_GAP_NAME);

	eeprom_commit();
}

esp_err_t eeprom_write_str(char* address, char* data)
{
	if(eeprom_handle)
		return nvs_set_str(eeprom_handle, address, data);

	ESP_LOGI(EEPROM_TAG, "Write failed - not initialized");
	return ESP_FAIL;
}

char* eeprom_read_str(char* address)
{
	if(eeprom_handle) {
		size_t requiredSize;
		esp_err_t err = nvs_get_str(eeprom_handle, address, NULL, &requiredSize);
		if(err == ESP_OK) {
			char* strData = malloc(requiredSize);
			if(strData){
				err = nvs_get_str(eeprom_handle, address, strData, &requiredSize);
				if(err == ESP_OK)
					return strData;

				free(strData);
			}
		}
		ESP_LOGI(EEPROM_TAG, "Read failed - not stored");
		return NULL;
	}

	ESP_LOGI(EEPROM_TAG, "Read failed - not initialized");
	return NULL;
}

esp_err_t eeprom_write_int(char* address, int32_t data)
{
	if(eeprom_handle)
		return nvs_set_i32(eeprom_handle, address, data);

	ESP_LOGI(EEPROM_TAG, "Write failed - not initialized");
	return ESP_FAIL;
}

int32_t eeprom_read_int(char* address)
{
	if(eeprom_handle) {
		int32_t i = 0;

		nvs_get_i32(eeprom_handle, address, &i);
		return i;
	}

	ESP_LOGI(EEPROM_TAG, "Read failed - EEPROM not initialized");
	return 0;
}
