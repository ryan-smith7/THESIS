/* current_thread.c — with dummy implementation
 *
 * Replace dummy_read_current_uA() and dummy_read_voltage_mV()
 * with their actual reads. Everything else
 * (queue, thread structure) stays identical
 */
 
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "sensor.h"
#include "time_sync.h"
 
LOG_MODULE_REGISTER(current_sensor, LOG_LEVEL_INF);
 
/* ── Queue ───────────────────────────────────────────────────────────────── */
#define CURRENT_Q_DEPTH     8
#define CURRENT_STACK_SIZE  2048
#define CURRENT_PRIORITY    6
#define SAMPLE_PERIOD_CURRENT_MS        5000U
 
K_MSGQ_DEFINE(current_q, sizeof(struct current_msg), CURRENT_Q_DEPTH, 4);
 
/*
 * Replace these two functions with your actual sensor reads.
 * Return values:
 *   current_uA
 *   voltage_mV
 */
static int16_t dummy_read_current_uA(void) {
    /* Placeholder: simple triangle wave between -5000 uA and +5000 uA */
    static int32_t t = 0;
    t += 100;
    int32_t phase = t % 20000;
    if (phase < 10000) {
        return (int16_t)(phase - 5000);
    } else {
        return (int16_t)(15000 - phase);
    }
}
 
static uint16_t dummy_read_voltage_mV(void) {
    /* Placeholder: returns 3300 mV (3.3V rail) */
    return 3300;
}
 
void current_thread(void) {
    LOG_INF("Current sensor thread ready");
 
    while (1) {
        // READ SENSOR — replace dummy calls with real reads
        int16_t  current_uA = dummy_read_current_uA();
        uint16_t voltage_mV = dummy_read_voltage_mV();
 
        // STAMP UTC AT MEASUREMENT MOMENT
        uint16_t utc_ms;
        uint32_t utc_sec = time_sync_get_utc_ms(&utc_ms);
 
        struct current_msg m = {
            .current_uA = current_uA,
            .voltage_mV = voltage_mV,
            .utc_sec    = utc_sec,
            .utc_ms     = utc_ms,
        };
 
        printk("CURR put: current=%d uA  (%.3f mA)  voltage=%u mV  UTC=%u.%03u\n",
               m.current_uA,
               (double)m.current_uA / 1000.0,
               m.voltage_mV,
               m.utc_sec,
               m.utc_ms);
 
        if (k_msgq_put(&current_q, &m, K_NO_WAIT) != 0) {
            // if queue is full
            struct current_msg dump;
            (void)k_msgq_get(&current_q, &dump, K_NO_WAIT); //dequeue oldest message
            (void)k_msgq_put(&current_q, &m, K_NO_WAIT); // enqueue new message
        }
 
        k_msleep(SAMPLE_PERIOD_CURRENT_MS);
    }
}