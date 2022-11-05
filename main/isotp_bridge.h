#ifndef BRIDGE_H
#define BRIDGE_H

void isotp_init();
void isotp_deinit();
void isotp_start_task();
void isotp_stop_task();

void received_from_ble(const void* src, size_t size);
void bridge_connect();
void bridge_disconnect();

#endif