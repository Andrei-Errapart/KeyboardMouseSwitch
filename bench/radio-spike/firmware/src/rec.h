/* Records streamed to the host, one per SLIP frame. Layouts match the
 * record table in docs/superpowers/plans/2026-11-07-radio-bench-spike.md
 * and bench/radio-spike/host/kmbench/records.py. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/toolchain.h>

enum rec_type {
	REC_BOOT = 0x01, REC_TEXT = 0x02, REC_STATS = 0x03, REC_QUEUED = 0x10,
	REC_TXDONE = 0x11, REC_RXPKT = 0x12, REC_LINK = 0x13, REC_SCAN = 0x14,
	REC_SELFTEST = 0x15, REC_MERGED = 0x16,
};

struct __packed rec_boot {
	uint8_t type; uint32_t t; uint32_t boot_id; uint8_t role; uint8_t cand;
	char serial[12]; char fw[16];
};
struct __packed rec_stats {
	uint8_t type; uint32_t t; uint32_t addr_tx; uint32_t addr_rx;
	uint32_t rec_dropped; uint32_t stamp_lost; uint32_t mic_fail;
	uint32_t replay_reject; uint32_t merged; uint32_t queue_dropped;
	uint32_t mode_switches; uint32_t mode_switch_us_total;
	uint16_t mode_switch_us_max;
};
struct __packed rec_queued { uint8_t type; uint32_t t; uint32_t id; uint8_t pipe; };
struct __packed rec_txdone {
	uint8_t type; uint32_t t; uint32_t id; uint8_t pipe; uint8_t kind;
	uint8_t attempts; uint8_t ok; uint8_t len; uint8_t ack_len;
	uint32_t t_tx_addr; uint32_t t_rx_addr;
};
struct __packed rec_rxpkt {
	uint8_t type; uint32_t t; uint32_t id; uint8_t pipe; uint8_t kind;
	uint8_t len; int8_t rssi; uint32_t t_rx_addr;
};
struct __packed rec_link { uint8_t type; uint32_t t; uint8_t event; uint8_t pipe; uint8_t peer[6]; };
struct __packed rec_scan {
	uint8_t type; uint8_t channel; int8_t rssi_min; int8_t rssi_median;
	int8_t rssi_max; uint16_t busy_permille;
};
struct __packed rec_selftest {
	uint8_t type; uint8_t nist_ok; uint8_t layout_ok; uint8_t replay_ok;
	uint8_t tamper_ok; uint16_t enc_us; uint16_t dec_us; uint16_t sk_us;
};
struct __packed rec_merged { uint8_t type; uint32_t t; uint32_t id; uint32_t into; uint8_t pipe; };

/* Sizes from the plan's record table, type byte included. */
BUILD_ASSERT(sizeof(struct rec_boot) == 39);
BUILD_ASSERT(sizeof(struct rec_stats) == 47);
BUILD_ASSERT(sizeof(struct rec_queued) == 10);
BUILD_ASSERT(sizeof(struct rec_txdone) == 23);
BUILD_ASSERT(sizeof(struct rec_rxpkt) == 17);
BUILD_ASSERT(sizeof(struct rec_link) == 13);
BUILD_ASSERT(sizeof(struct rec_scan) == 7);
BUILD_ASSERT(sizeof(struct rec_selftest) == 11);
BUILD_ASSERT(sizeof(struct rec_merged) == 14);

/* Counters reported in STATS, written from any context. */
struct bench_counters {
	atomic_t mic_fail, replay_reject, merged, queue_dropped;
	atomic_t mode_switches, mode_switch_us_total, mode_switch_us_max;
};
extern struct bench_counters counters;

#define CMD_LINE_MAX 96

int rec_init(void);
/* Queue one record. ISR-safe. False, and counted in rec_dropped, when the
 * ring is full. */
bool rec_put(const void *rec, size_t len);
void rec_text(const char *fmt, ...);
void rec_stats(void);
uint32_t rec_dropped(void);
/* Called on a CDC control-line change; reads DTR. */
void rec_dtr_update(void);
bool rec_dtr(void);
/* The next command line from the host, or false on timeout. */
bool rec_get_line(char line[CMD_LINE_MAX], k_timeout_t timeout);
