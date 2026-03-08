#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>

#include "bluetooth.h"

K_THREAD_DEFINE(process_data_tid, BASE_PROCESS_STACK_SIZE, process_data_thread, NULL, NULL, NULL, BASE_PROCESS_PRIORITY, 0, 0);
K_THREAD_DEFINE(base_tid, BASE_CONTROL_STACK_SIZE, base_thread, NULL, NULL, NULL, BASE_CONTROL_PRIORITY, 0, 0);