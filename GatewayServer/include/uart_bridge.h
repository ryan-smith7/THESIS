#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#include <stdint.h>
/*
 * uart_bridge.h — UART JSON receiver for WiFi board (ESP32 #2)
 *
 * Wiring:
 *   BLE board GPIO17 (TX2) --> WiFi board GPIO16 (RX2)
 *   BLE board GPIO16 (RX2) <-- WiFi board GPIO17 (TX2)
 *   BLE board GND          --- WiFi board GND
 *
 * Protocol: newline-terminated JSON strings at 115200 baud.
 * Each complete line is published to Azure IoT Hub via azure_mqtt_publish().
 */

/* Start the UART bridge receive thread */
void uart_bridge_start(void);

/**
 * @brief Send a UTC sync frame to the BLE gateway (7 bytes).
 *
 *   [ 0xCC ][ sec_b3 ][ sec_b2 ][ sec_b1 ][ sec_b0 ][ ms_hi ][ ms_lo ]
 *
 * Safe to call from any thread. Blocks ~70µs for polling TX at 115200.
 *
 * @param utc_time  Unix UTC timestamp (seconds since epoch).
 * @param utc_ms    Sub-second milliseconds (0–999).
 */
void uart_bridge_send_utc(uint32_t utc_time, uint16_t utc_ms);

#endif /* UART_BRIDGE_H */