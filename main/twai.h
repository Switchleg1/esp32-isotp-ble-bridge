#ifndef TWAI_H
#define TWAI_H

void twai_init();
void twai_deinit();
void twai_start_task();
void twai_stop_task();
void twai_send(twai_message_t *twai_tx_msg);

#endif