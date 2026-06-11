#ifndef BASE_SERVICE_H
#define BASE_SERVICE_H

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>

/*
 * Stack sizes and connection limit differ by Gateway platform:
 */
#if defined(CONFIG_ETH_GATEWAY)
#  define BASE_CONTROL_STACK_SIZE  1536
#  define BASE_PROCESS_STACK_SIZE  4096   /* headroom for 4 KB sound JSON encode */
#  define MAX_CONN                 3      /* max simultaneous BLE peripheral connections */
#else
#  define BASE_CONTROL_STACK_SIZE  2048
#  define BASE_PROCESS_STACK_SIZE  4096   /* headroom for 4 KB sound JSON encode */
#  define MAX_CONN                 3     /* max simultaneous BLE peripheral connections */
#endif

#define BASE_CONTROL_PRIORITY   5
#define BASE_PROCESS_PRIORITY   5

extern void process_data_thread(void);
extern void base_thread(void);

#endif /* BASE_SERVICE_H */
