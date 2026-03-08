#ifndef SOUND_BLE_H
#define SOUND_BLE_H

/**
 * @file sound_ble.h
 * @brief Dedicated BLE GATT characteristic for SPH0645 FFT spectrum.
 *
 * Exposes a separate GATT service with one notify characteristic.
 * The full 348-bin spectrum (696 bytes) is split into 3 sequential
 * BLE notifications, each prefixed with a 4-byte header so the
 * gateway can reassemble them.
 *
 * Packet layout (each notification, max 244 bytes):
 * ┌─────────────┬────────────┬──────────────┬──────────────────────────┐
 * │ pkt_id (1B) │ total (1B) │ timestamp_ms │ payload                  │
 * │             │            │ low16 (2B)   │ (bin magnitudes ×10)     │
 * └─────────────┴────────────┴──────────────┴──────────────────────────┘
 *
 * Packet 0: header(4) + bins   1–116  = 4 + 232 = 236 bytes
 * Packet 1: header(4) + bins 117–232  = 4 + 232 = 236 bytes
 * Packet 2: header(4) + bins 233–348  + rms_dbfs_x100(2)
 *                                      = 4 + 232 + 2 = 238 bytes
 *
 * Total bins sent: 348 × uint16_t = 696 bytes across 3 packets.
 *
 * Gateway reassembly: match packets by timestamp_ms low16, ordered by pkt_id.
 *
 * Service UUID:  A1B2C3D4-E5F6-7890-ABCD-EF1234567890
 * Char UUID:     A1B2C3D4-E5F6-7890-ABCD-EF1234567891
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>

/* ── Chunking constants ─────────────────────────────────── */
#define SOUND_BLE_BINS_PER_PKT   116U   /* bins per notification         */
#define SOUND_BLE_NUM_PKTS       3U     /* ceil(348 / 116) = 3           */
#define SOUND_BLE_HDR_SIZE       4U     /* pkt_id + total + timestamp16  */

/*
 * Max notification payload:
 *   header(4) + 116 bins × 2 bytes = 236 bytes — well within 244 byte MTU
 */
#define SOUND_BLE_PKT_SIZE  (SOUND_BLE_HDR_SIZE + SOUND_BLE_BINS_PER_PKT * 2U)

/* ── Thread config ──────────────────────────────────────── */
#define SOUND_BLE_STACK_SIZE  2048
#define SOUND_BLE_PRIORITY    6     /* lower than sound_thread (4) */

/* ── Public API ─────────────────────────────────────────── */

/**
 * @brief Thread that consumes sound_spec_q and sends 3 BLE notifications.
 *        Register with K_THREAD_DEFINE in main.c.
 */
extern void sound_ble_thread(void);

#endif /* SOUND_BLE_H */