#ifndef WS2812_CONTROL_H
#define WS2812_CONTROL
#include <stdint.h>
#include "hal/gpio_types.h"
#define NUM_LEDS 1

// This structure is used for indicating what the colors of each LED should be set to.
// There is a 32bit value for each LED. Only the lower 3 bytes are used and they hold the
// Red (byte 2), Green (byte 1), and Blue (byte 0) values to be set.
struct led_state {
    uint32_t leds[NUM_LEDS];
};

// Setup the hardware peripheral. Only call this once.
void ws2812_control_init(gpio_num_t led_gpio);

// Update the LEDs to the new state. Call as needed.
// This function will block the current task until the RMT peripheral is finished sending 
// the entire sequence.
void ws2812_write_leds(uint32_t led_state);

#endif