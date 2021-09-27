#ifndef CONSTANTS_H
#define CONSTANTS_H

/* --------------------- Definitions and static variables ------------------ */

#define RX_TASK_PRIO 			 	3 // Ensure we drain the RX queue as quickly as we reasonably can to prevent overflow and ensure the message pump has fresh data.
#define TX_TASK_PRIO 			 	3 // Ensure we TX messages as quickly as we reasonably can to meet ISO15765-2 timing constraints
#define ISOTP_TSK_PRIO 				2 // Run the message pump at a higher priority than the main queue/dequeue task when messages are available
#define MAIN_TSK_PRIO 				1 // Run the main task at the same priority as the BLE queue/dequeue tasks to help in delivery ordering.
#define PERSIST_TSK_PRIO 			0

#define TX_GPIO_NUM 				5 // For A0
#define RX_GPIO_NUM 				4 // For A0
#define SILENT_GPIO_NUM 			21 // For A0
#define LED_ENABLE_GPIO_NUM 		13 // For A0
#define LED_GPIO_NUM 				2 // For A0
#define GPIO_OUTPUT_PIN_SEL(X)  	((1ULL<<X))


#define ISOTP_BUFSIZE 				4096

//Queue sizes
#define TASK_QUEUE_SIZE     		32
#define MESSAGE_QUEUE_SIZE			32

#define TIMEOUT_SHORT				50
#define TIMEOUT_NORMAL				100
#define TIMEOUT_LONG				1000
#define TIMEOUT_CONGESTION			5000

#endif