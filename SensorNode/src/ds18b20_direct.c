/*
 * ds18b20_direct.c
 *
 * Direct DS18B20 1-Wire driver for ESP32 on Zephyr RTOS.
 * Bypasses Zephyr's w1_serial driver entirely, implementing
 * the UART-based 1-Wire protocol directly using ESP32 HAL APIs.
 *
 * Modelled on ESP-IDF one_wire.c approach:
 * - GPIO set to open-drain input/output via GPIO matrix
 * - TX and RX connected to same GPIO pin
 * - Baud rate switched between 9600 (reset) and 115200 (data)
 * - Explicit TX completion wait before baud switch
 * - Explicit RX FIFO flush before each transaction
 *
 * Wiring:
 *   GPIO14 (TX) ──|BAT46|──┬── DS18B20 S pin (DQ)
 *   GPIO16 (RX) ───────────┘
 *   3.3V ──── 4.7kΩ ────── DQ
 *   GND  ──── DS18B20 GND
 *   3.3V ──── DS18B20 VCC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include <soc/gpio_struct.h>
#include <soc/gpio_sig_map.h>
#include <esp_rom_gpio.h>

#include <zephyr/drivers/sensor.h>

LOG_MODULE_REGISTER(ds18b20_direct, LOG_LEVEL_DBG);

/* ── Pin assignments ─────────────────────────────────────────────────────── */
#define W1_TX_GPIO          17      /* UART2 TX — through BAT46 diode to DQ  */
#define W1_RX_GPIO          16      /* UART2 RX — direct to DQ               */

/* ── 1-Wire UART byte encodings ──────────────────────────────────────────── */
#define OW_RESET_BYTE       0xF0U   /* Reset pulse at 9600 baud              */
#define OW_BIT_1            0xFFU   /* Logic 1 / read slot at 115200 baud    */
#define OW_BIT_0            0x00U   /* Logic 0 at 115200 baud                */

/* ── Baud rates ──────────────────────────────────────────────────────────── */
#define OW_BAUD_RESET       9600U
#define OW_BAUD_DATA        115200U

/* ── Timing ──────────────────────────────────────────────────────────────── */
/* One byte at 9600 baud = 10 bits × (1/9600) = 1041µs — wait 1.5× */
#define OW_TX_WAIT_RESET_US     1600U
/* One byte at 115200 baud = 10 bits × (1/115200) = 87µs — wait 1.5× */
#define OW_TX_WAIT_DATA_US      500U

/* RX poll timeout for reset (one full reset slot ~1041µs × 5 margin) */
#define OW_RX_TIMEOUT_RESET_US  5000U
/* RX poll timeout for data bit (one slot ~87µs × 5 margin) */
#define OW_RX_TIMEOUT_DATA_US   5000U

/* ── DS18B20 commands ─────────────────────────────────────────────────────── */
#define DS18B20_CMD_SKIP_ROM        0xCCU
#define DS18B20_CMD_CONVERT_T       0x44U
#define DS18B20_CMD_READ_SCRATCHPAD 0xBEU
#define DS18B20_SCRATCHPAD_BYTES    9U
#define DS18B20_RESOLUTION_12BIT_MS 750U

/* ── UART device ─────────────────────────────────────────────────────────── */
#define W1_UART_NODE DT_NODELABEL(uart2)
static const struct device *w1_uart;

/* ── Stored UART config ──────────────────────────────────────────────────── */
static struct uart_config w1_uart_cfg = {
    .parity    = UART_CFG_PARITY_NONE,
    .data_bits = UART_CFG_DATA_BITS_8,
    .stop_bits = UART_CFG_STOP_BITS_1,
    .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
};

static void w1_flush_rx(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * Low-level UART helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Set UART baud rate and settle.
 *        Re-applies GPIO matrix routing after every uart_configure call
 *        because uart_esp32_configure resets the GPIO matrix state.
 */
static int w1_set_baud(uint32_t baud)
{
    w1_uart_cfg.baudrate = baud;
    int ret = uart_configure(w1_uart, &w1_uart_cfg);
    if (ret != 0) {
        LOG_ERR("uart_configure(%u) failed: %d", baud, ret);
        return ret;
    }

    /* Re-apply open-drain and GPIO matrix routing */
    GPIO.pin[W1_TX_GPIO].pad_driver = 1;
    esp_rom_gpio_connect_out_signal(W1_TX_GPIO, U2TXD_OUT_IDX, false, false);
    esp_rom_gpio_connect_in_signal(W1_RX_GPIO, U2RXD_IN_IDX, false);

    /* Settle and flush any garbage from baud rate transition glitch */
    k_sleep(K_MSEC(2));
    w1_flush_rx();
    k_sleep(K_MSEC(1));
    w1_flush_rx();

    return 0;
}
/**
 * @brief Flush any stale bytes from the UART RX FIFO.
 */
static void w1_flush_rx(void)
{
    uint8_t dummy;
    while (uart_poll_in(w1_uart, &dummy) == 0) {
        /* discard */
    }
}

/**
 * @brief Transmit one byte and receive the loopback echo.
 *        TX and RX share the same wire — the received byte reflects
 *        what was actually on the bus (DS18B20 may pull it low).
 *
 * @param tx_byte   Byte to transmit
 * @param rx_byte   Pointer to store received byte
 * @param tx_wait   Microseconds to wait for TX completion before polling RX
 * @param rx_timeout Microseconds to poll RX before timing out
 * @return 0 on success, -EIO on timeout
 */
static int w1_tx_rx(uint8_t tx_byte, uint8_t *rx_byte,
                    uint32_t tx_wait, uint32_t rx_timeout)
{
    w1_flush_rx();

    uart_poll_out(w1_uart, tx_byte);

    /* Wait for TX to fully shift out before reading loopback.
     * This is the Zephyr equivalent of ESP-IDF uart_wait_tx_done(). */
    k_busy_wait(tx_wait);

    /* Poll for loopback byte */
    k_timepoint_t end = sys_timepoint_calc(K_USEC(rx_timeout));
    int ret;
    do {
        ret = uart_poll_in(w1_uart, rx_byte);
    } while (ret != 0 && !sys_timepoint_expired(end));

    if (ret != 0) {
        LOG_ERR("RX timeout: tx=0x%02x", tx_byte);
        return -EIO;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 1-Wire protocol primitives
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Issue a 1-Wire reset pulse and detect presence.
 *
 * Sends 0xF0 at 9600 baud. At this baud rate the start bit + 4 zero bits
 * create a ~416µs low pulse (1-Wire reset). The DS18B20 responds with a
 * presence pulse that pulls further bits low, so the received byte differs
 * from 0xF0.
 *
 * @return true if device present, false if no device or bus error
 */
static bool w1_reset(void)
{
    uint8_t rx = 0;

    if (w1_set_baud(OW_BAUD_RESET) != 0) {
        return false;
    }

    if (w1_tx_rx(OW_RESET_BYTE, &rx,
                 OW_TX_WAIT_RESET_US, OW_RX_TIMEOUT_RESET_US) != 0) {
        LOG_ERR("Reset: no loopback received");
        /* Switch back to data baud before returning */
        w1_set_baud(OW_BAUD_DATA);
        return false;
    }

    LOG_DBG("Reset: tx=0x%02x rx=0x%02x", OW_RESET_BYTE, rx);

    /* Switch back to data baud for subsequent transactions */
    w1_set_baud(OW_BAUD_DATA);
    w1_flush_rx();
    k_sleep(K_MSEC(1));  /* ← yields to BLE stack */
    w1_flush_rx();
    
    /* Device present if received byte differs from sent and isn't 0x00 (shorted) */
    bool present = (rx != OW_RESET_BYTE) && (rx != 0x00);
    if (!present) {
        LOG_ERR("No 1-Wire device detected (rx=0x%02x)", rx);
    }
    return present;
}

/**
 * @brief Write one bit onto the 1-Wire bus.
 *        Must be called at 115200 baud.
 */
static void w1_write_bit(bool bit)
{
    uint8_t tx = bit ? OW_BIT_1 : OW_BIT_0;
    uint8_t rx = 0;
    w1_tx_rx(tx, &rx, OW_TX_WAIT_DATA_US, OW_RX_TIMEOUT_DATA_US);
}

/**
 * @brief Read one bit from the 1-Wire bus.
 *        Sends 0xFF (read time slot). If the slave pulls the line low,
 *        the received byte will differ from 0xFF → bit is 0.
 *        Must be called at 115200 baud.
 *
 * @return 1 or 0
 */
static int w1_read_bit(void)
{
    uint8_t rx = 0;
    if (w1_tx_rx(OW_BIT_1, &rx,
                 OW_TX_WAIT_DATA_US, OW_RX_TIMEOUT_DATA_US) != 0) {
        return 0;
    }
    return (rx == OW_BIT_1) ? 1 : 0;
}

/**
 * @brief Write one byte onto the 1-Wire bus, LSB first.
 */
static void w1_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        w1_write_bit((byte >> i) & 0x01);
    }
}

/**
 * @brief Read one byte from the 1-Wire bus, LSB first.
 */
static uint8_t w1_read_byte(void)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        if (w1_read_bit()) {
            byte |= (1U << i);
        }
    }
    return byte;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CRC
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Dallas/Maxim 1-Wire CRC8 (polynomial x^8 + x^5 + x^4 + 1).
 */
static uint8_t w1_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x01) {
                crc = (crc >> 1) ^ 0x8C;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DS18B20 public API
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialise the DS18B20 driver.
 *        Call once at startup before ds18b20_read_temp().
 *
 * @return 0 on success, negative errno on failure
 */
int ds18b20_direct_init(void)
{
    w1_uart = DEVICE_DT_GET(W1_UART_NODE);
    if (!device_is_ready(w1_uart)) {
        LOG_ERR("UART2 not ready");
        return -ENODEV;
    }

    /* Initial UART configuration at data baud */
    if (w1_set_baud(OW_BAUD_DATA) != 0) {
        return -EIO;
    }

    LOG_INF("DS18B20 direct driver initialised (TX=GPIO%d, RX=GPIO%d)",
            W1_TX_GPIO, W1_RX_GPIO);
    return 0;
}

/**
 * @brief Read temperature from DS18B20.
 *
 * Performs: Reset → Skip ROM → Convert T → wait 750ms → 
 *           Reset → Skip ROM → Read Scratchpad → CRC check → parse
 *
 * @param temp_c  Pointer to store temperature in degrees Celsius
 * @return 0 on success, negative errno on failure
 *         -ENODEV  no device on bus
 *         -EIO     CRC error or bus error
 */
int ds18b20_direct_read(float *temp_c)
{
    uint8_t scratchpad[DS18B20_SCRATCHPAD_BYTES];

    /* ── Step 1: Reset + Skip ROM + Convert T ───────────────────────── */
    if (!w1_reset()) {
        return -ENODEV;
    }
    w1_write_byte(DS18B20_CMD_SKIP_ROM);
    w1_write_byte(DS18B20_CMD_CONVERT_T);

    /* ── Step 2: Wait for 12-bit conversion (750ms) ─────────────────── */
    k_sleep(K_MSEC(DS18B20_RESOLUTION_12BIT_MS));

    /* ── Step 3: Reset + Skip ROM + Read Scratchpad ─────────────────── */
    if (!w1_reset()) {
        return -ENODEV;
    }
    w1_write_byte(DS18B20_CMD_SKIP_ROM);
    w1_write_byte(DS18B20_CMD_READ_SCRATCHPAD);

    for (int i = 0; i < DS18B20_SCRATCHPAD_BYTES; i++) {
        scratchpad[i] = w1_read_byte();
    }

    /* ── Step 4: CRC check ───────────────────────────────────────────── */
    uint8_t crc_calc = w1_crc8(scratchpad, DS18B20_SCRATCHPAD_BYTES - 1);
    if (crc_calc != scratchpad[DS18B20_SCRATCHPAD_BYTES - 1]) {
        LOG_ERR("CRC mismatch: calc=0x%02x got=0x%02x",
                crc_calc, scratchpad[DS18B20_SCRATCHPAD_BYTES - 1]);
        return -EIO;
    }

    /* ── Step 5: Parse temperature ───────────────────────────────────── */
    /* Scratchpad bytes 0-1 are the 16-bit raw temperature, LSB first.
     * DS18B20 12-bit resolution: LSB = 0.0625°C */
    int16_t raw = (int16_t)((scratchpad[1] << 8) | scratchpad[0]);
    *temp_c = (float)raw / 16.0f;

    LOG_DBG("Raw=0x%04x Temp=%.4f°C", (uint16_t)raw, (double)*temp_c);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Optional: sensor_value output (matches Zephyr sensor API convention)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Read temperature into a Zephyr sensor_value struct.
 *        val1 = integer degrees C, val2 = fractional part in millionths.
 *
 * @param val  Pointer to sensor_value to populate
 * @return 0 on success, negative errno on failure
 */
int ds18b20_direct_read_sensor_value(struct sensor_value *val)
{
    float temp;
    int ret = ds18b20_direct_read(&temp);
    if (ret != 0) {
        return ret;
    }

    val->val1 = (int32_t)temp;
    val->val2 = (int32_t)((temp - val->val1) * 1000000.0f);
    return 0;
}
