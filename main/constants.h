#ifndef CONSTANTS_H
#define CONSTANTS_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

//global semaphore to sync start/stop of tasks
SemaphoreHandle_t						sync_task_sem;

//defines
typedef int16_t							bool16;
#define tMUTEX(x)						xSemaphoreTake(x, portMAX_DELAY)
#define rMUTEX(x)						xSemaphoreGive(x)

//Settings
#define BRG_SETTING_ISOTP_STMIN			1
#define BRG_SETTING_LED_COLOR			2
#define BRG_SETTING_PERSIST_DELAY		3
#define BRG_SETTING_PERSIST_Q_DELAY		4
#define BRG_SETTING_BLE_SEND_DELAY		5
#define BRG_SETTING_BLE_MULTI_DELAY		6
#define BRG_SETTING_PASSWORD			7
#define BRG_SETTING_GAP					8

#define TASK_STACK_SIZE					3072 //2048

#define TWAI_TASK_PRIO 			 		3 // Ensure we TX messages as quickly as we reasonably can to meet ISO15765-2 timing constraints
#define ISOTP_TSK_PRIO 					2 // Run the message pump at a higher priority than the main queue/dequeue task when messages are available
#define MAIN_TSK_PRIO 					1 // Run the main task at the same priority as the BLE queue/dequeue tasks to help in delivery ordering.
#define PERSIST_TSK_PRIO 				0
#define HANDLER_TSK_PRIO 				0
#define UART_TSK_PRIO 					1

#define SILENT_GPIO_NUM 				21 // For A0
#define LED_ENABLE_GPIO_NUM 			13 // For A0
#define LED_GPIO_NUM 					2 // For A0
#define GPIO_OUTPUT_PIN_SEL(X)  		((1ULL<<X))

#define ISOTP_BUFFER_SIZE 				4096
#define ISOTP_BUFFER_SIZE_SMALL 		512

#define TIMEOUT_SHORT					50
#define TIMEOUT_NORMAL					100
#define TIMEOUT_LONG					1000

#define TIMEOUT_CANCONNECTION			1
#define TIMEOUT_FIRSTBOOT				30
#define TIMEOUT_UARTCONNECTION			120
#define TIMEOUT_UARTPACKET				1

#define ALLOW_SLEEP
#define RESET_ON_DISCONNECT
#define SLEEP_TIME						5
#define WDT_TIMEOUT_S					5
#define US_TO_S							1000000

//#define PASSWORD_CHECK
#define MAX_PASSWORD_LENGTH				64
#define PASSWORD_KEY					"Password"
#define PASSWORD_DEFAULT			   	"BLE2"
#define BLE_GAP_KEY		  				"GAP"

#define CAN_INTERNAL_BUFFER_SIZE		1024
#define CAN_TX_PORT						5
#define CAN_RX_PORT						4
#define CAN_CLK_IO						TWAI_IO_UNUSED
#define CAN_BUS_IO						TWAI_IO_UNUSED
#define CAN_MODE						TWAI_MODE_NORMAL
#define CAN_ALERTS						TWAI_ALERT_ABOVE_ERR_WARN | TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED
#define CAN_FLAGS						ESP_INTR_FLAG_LEVEL1
#define CAN_CLK_DIVIDER					0
#define CAN_TIMING						TWAI_TIMING_CONFIG_500KBITS()
#define CAN_FILTER						TWAI_FILTER_CONFIG_ACCEPT_ALL()
#define ISOTP_STMIN						1000

#define UART_TXD 						UART_PIN_NO_CHANGE
#define UART_RXD 						UART_PIN_NO_CHANGE
#define UART_RTS 						UART_PIN_NO_CHANGE
#define UART_CTS 						UART_PIN_NO_CHANGE
#define UART_PORT_NUM      				UART_NUM_0
#define UART_BAUD_RATE     				250000 //250000 //115200
#define UART_BUFFER_SIZE				8192
#define UART_INTERNAL_BUFFER_SIZE		2048
//#define UART_ECHO

#define PERSIST_COUNT					2
#define PERSIST_MAX_MESSAGE				64
#define PERSIST_DEFAULT_MESSAGE_DELAY	20
#define PERSIST_DEFAULT_QUEUE_DELAY		10

#endif