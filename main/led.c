#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "isotp.h"
#include "constants.h"
#include "ws2812_control.h"
#include "led.h"

#define LED_TAG 			"LED"

uint32_t led_color = 0;

void led_init()
{
	// Configure LED enable pin (switches transistor to push LED)
	gpio_config_t io_conf_led;
	io_conf_led.intr_type = GPIO_INTR_DISABLE;
	io_conf_led.mode = GPIO_MODE_OUTPUT;
	io_conf_led.pin_bit_mask = GPIO_OUTPUT_PIN_SEL(LED_ENABLE_GPIO_NUM);
	io_conf_led.pull_down_en = 0;
	io_conf_led.pull_up_en = 0;
	gpio_config(&io_conf_led);
	gpio_set_level(LED_ENABLE_GPIO_NUM, 0);

	ws2812_control_init(LED_GPIO_NUM);
	led_setcolor(LED_RED_QRT);
}

void led_deinit()
{
	led_setcolor(LED_OFF);
}

void led_setcolor(uint32_t color)
{
	led_color = color;
	ws2812_write_leds(led_color & 0xFFFFFF);
}

uint32_t led_getcolor()
{
	return led_color;
}
