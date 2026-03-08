/**
 * @file uart.h
 * @brief UART TX/RX interface for the gateway board.
 *
 * Handles all UART2 communication between the gateway (ESP32 #1) and
 * the WiFi board (ESP32 #2):
 *
 * Outbound (TX — GPIO17):
 *   Sensor frames  [ 0xAA ] [ len_hi ] [ len_lo ] [ 57 bytes  ]
 *   Sound frames   [ 0xBB ] [ len_hi ] [ len_lo ] [ 698 bytes ]
 *
 * Inbound (RX — GPIO16):
 *   UTC sync frame [ 0xCC ] [ sec_b3..sec_b0 ] [ ms_hi ] [ ms_lo ]
 *   Parsed by interrupt-driven state machine; calls
 *   time_sync_writer_set_utc() on receipt of a complete frame.
 */

#ifndef GATEWAY_UART_H
#define GATEWAY_UART_H

#include <zephyr/kernel.h>
#include <zephyr/types.h>

/* ── Frame magic bytes ──────────────────────────────────── */
#define FRAME_MAGIC_SENSOR  0xAA   /* outbound: main sensor packet  */
#define FRAME_MAGIC_SOUND   0xBB   /* outbound: sound spectrum      */
#define FRAME_MAGIC_UTC     0xCC   /* inbound:  UTC time sync       */

/* ── Frame lengths ──────────────────────────────────────── */
#define FRAME_UTC_RX_LEN    7      /* magic(1) + utc_sec(4) + utc_ms(2) */

/**
 * @brief Initialise UART2 for TX and RX.
 *
 * Acquires the uart2 device, enables TX polling, and registers the
 * interrupt-driven RX handler. Must be called once before any other
 * uart_ functions, typically at the start of process_data_thread().
 */
void uart_gateway_init(void);

/**
 * @brief Transmit a framed packet over UART2.
 *
 * Frame layout: [ magic(1) ] [ len_hi(1) ] [ len_lo(1) ] [ data(len) ]
 *
 * Uses uart_poll_out() — blocking per byte, safe to call from any
 * thread context. Not safe to call concurrently from multiple threads
 * without external locking.
 *
 * @param magic  Frame type identifier (FRAME_MAGIC_SENSOR or FRAME_MAGIC_SOUND).
 * @param data   Pointer to payload buffer.
 * @param len    Number of payload bytes to transmit.
 */
void uart_tx_frame(uint8_t magic, const uint8_t *data, uint16_t len);

/**
 * @brief Check whether UART2 initialised successfully.
 *
 * @return true if the device is ready and TX/RX are active.
 */
bool uart_gateway_is_ready(void);

#endif /* GATEWAY_UART_H */