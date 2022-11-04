#ifndef CONNECTION_HANDLER_H
#define CONNECTION_HANDLER_H

void 		ch_init();
void 		ch_deinit();
void 		ch_start_task();
void		ch_stop_task();
void		ch_reset_uart_timer();
bool16		ch_uart_connected();
bool16		ch_take_can_timer_sem();
bool16		ch_take_uart_packet_timer_sem();
void		ch_take_sleep_sem();
void		ch_give_sleep_sem();

void 		ch_on_uart_connect();
void 		ch_on_uart_disconnect();

#endif