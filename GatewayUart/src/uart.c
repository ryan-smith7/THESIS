/**
 * @file uart.c
 * @brief UART TX/RX implementation for the gateway board.
 *
 * Outbound (TX):
 *   uart_tx_frame() sends framed sensor and sound packets to the WiFi board
 *   using uart_poll_out() (blocking, one byte at a time).
 *
 * Inbound (RX):
 *   An interrupt-driven state machine receives UTC sync frames from the
 *   WiFi board. On receipt of a complete 0xCC frame, calls
 *   time_sync_writer_set_utc() to update the gateway's UTC reference,
 *   which is then distributed to all connected sensor nodes.
 */

#include "uart.h"
#include "time_sync_writer.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gateway_uart, LOG_LEVEL_INF);

/* ── Device ─────────────────────────────────────────────── */
#define UART_DEVICE_NODE DT_NODELABEL(uart2)

static const struct device *uart_dev;
static bool uart_ready = false;

/* ── RX state machine ───────────────────────────────────────
 *
 * Parses the inbound UTC frame byte by byte inside the UART ISR:
 *
 *   Byte 0:   0xCC magic — starts a new frame
 *   Bytes 1–4: UTC unix timestamp, big-endian MSB first
 *
 * Any byte that is not 0xCC while idle is silently discarded,
 * making the parser self-synchronising after line noise or resets.
 */
#define UART_RX_FRAME_LEN  7   /* magic(1) + utc_sec(4) + utc_ms(2) */

static uint8_t uart_rx_buf[UART_RX_FRAME_LEN];
static uint8_t uart_rx_pos      = 0;
static bool    uart_rx_in_frame = false;

/**
 * @brief Process a single received byte inside the UART ISR.
 *
 * Implements the state machine:
 *   IDLE        — wait for 0xCC magic byte
 *   IN_FRAME    — accumulate remaining 4 UTC bytes
 *   COMPLETE    — parse and call time_sync_writer_set_utc(), reset to IDLE
 *
 * @param byte  The byte just read from the UART FIFO.
 */
static void uart_rx_handle_byte(uint8_t byte)
{
    // LOG_INF("in uart handler");
    if (!uart_rx_in_frame) {
        /* IDLE — wait for magic */
        if (byte == FRAME_MAGIC_UTC) {
            uart_rx_buf[0]   = byte;
            uart_rx_pos      = 1;
            uart_rx_in_frame = true;
        }
        return;
    }

    /* IN_FRAME — accumulate bytes */
    uart_rx_buf[uart_rx_pos++] = byte;

    if (uart_rx_pos == UART_RX_FRAME_LEN) {
        /* COMPLETE — parse UTC seconds (bytes 1–4) and ms (bytes 5–6) */
        uint32_t utc_sec = ((uint32_t)uart_rx_buf[1] << 24) |
                           ((uint32_t)uart_rx_buf[2] << 16) |
                           ((uint32_t)uart_rx_buf[3] <<  8) |
                            (uint32_t)uart_rx_buf[4];

        uint16_t utc_ms  = ((uint16_t)uart_rx_buf[5] << 8) |
                            (uint16_t)uart_rx_buf[6];

        time_sync_writer_set_utc(utc_sec, utc_ms);

        /* Reset to IDLE */
        uart_rx_pos      = 0;
        uart_rx_in_frame = false;
    }
}

/**
 * @brief UART interrupt handler — drains the RX FIFO byte by byte.
 *
 * Registered via uart_irq_callback_set() during uart_gateway_init().
 * Called by the Zephyr UART driver on each RX FIFO ready event.
 *
 * @param dev        UART device pointer (unused directly).
 * @param user_data  Not used.
 */
static void uart_irq_handler(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        uint8_t byte;
        if (uart_fifo_read(dev, &byte, 1) == 1) {
            uart_rx_handle_byte(byte);
        }
    }
}

/* ── Public API ─────────────────────────────────────────── */

void uart_gateway_init(void)
{
    uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART2 not ready — TX and RX disabled");
        return;
    }

    uart_ready = true;
    LOG_INF("UART2 TX ready (GPIO17)");

    /* Register ISR and enable RX interrupts */
    uart_irq_callback_set(uart_dev, uart_irq_handler);
    uart_irq_rx_enable(uart_dev);
    LOG_INF("UART2 RX interrupt enabled (GPIO16)");
}

bool uart_gateway_is_ready(void)
{
    return uart_ready;
}

void uart_tx_frame(uint8_t magic, const uint8_t *data, uint16_t len)
{
    if (!uart_ready) {
        LOG_WRN("uart_tx_frame: UART2 not ready — dropping frame 0x%02x", magic);
        return;
    }

    /* Header: magic + 2-byte big-endian length */
    uart_poll_out(uart_dev, magic);
    uart_poll_out(uart_dev, (uint8_t)(len >> 8));
    uart_poll_out(uart_dev, (uint8_t)(len & 0xFF));

    /* Payload */
    for (uint16_t i = 0; i < len; i++) {
        uart_poll_out(uart_dev, data[i]);
    }
}