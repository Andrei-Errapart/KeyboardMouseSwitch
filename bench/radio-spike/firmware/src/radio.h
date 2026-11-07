/* ESB per mode, the radio thread, and what both candidates share: the
 * peer table and the discovery handshake. See "Packets" in the plan. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "link.h"
#include "pkt.h"

#define NUM_PIPES 8 /* 0: discovery; 1..7: receivers */
#define RADIO_MAX_PAYLOAD 32

struct bench_params {
	uint8_t channel;  /* 2400 + channel MHz */
	int8_t tx_power;  /* dBm */
	uint16_t ard_us;  /* ESB retransmit delay; ESB rejects < 435 */
	uint8_t retries_report, retries_poll, retries_ping, retries_hello;
	uint16_t ping_ms, refresh_ms, hello_ms, hello_jitter_ms;
	uint16_t poll_active_us, poll_idle_ms;
	uint16_t loss_ms, hold_ms;
	uint8_t fast_ramp; /* ESB use_fast_ramp_up */
};
extern struct bench_params params;

enum radio_mode { RADIO_OFF, RADIO_PTX, RADIO_PRX };

enum radio_evt_type {
	RADIO_EVT_TX_OK, RADIO_EVT_TX_FAIL, RADIO_EVT_RX, RADIO_EVT_WAKE, RADIO_EVT_CMD,
};

enum radio_cmd { CMD_ROLE, CMD_ACTIVE, CMD_PHANTOM };

struct radio_evt {
	uint8_t type;
	uint8_t attempts;
	uint8_t pipe;
	uint8_t len;
	int8_t rssi;
	uint8_t cmd, arg1, arg2;
	uint32_t t;         /* stamp at the ESB callback */
	uint32_t t_tx_addr; /* latest ADDRESS sent / received at that moment */
	uint32_t t_rx_addr; /* 0 when it can't be attributed to this payload */
	uint8_t data[RADIO_MAX_PAYLOAD];
};

/* One candidate, bridge and receiver sides. Runs in the radio thread. */
struct cand {
	void (*start)(void); /* g_role says which side */
	void (*stop)(void);
	void (*event)(const struct radio_evt *e);
	/* Do what is due; return µs until the next deadline (UINT32_MAX: none). */
	uint32_t (*tick)(void);
	void (*active)(uint8_t pipe); /* bridge: 0 = none */
	void (*phantom)(uint8_t n);   /* bridge */
};
extern const struct cand cand_b;
extern const struct cand cand_a;

int radio_init(void);
void radio_command(enum radio_cmd cmd, uint8_t arg1, uint8_t arg2);
void radio_wake(void); /* from any context */
uint8_t radio_active_pipe(void);
uint32_t pkt_new_id(void); /* per-boot packet numbering, from 1 */

/* For the candidates (radio thread only). */
int radio_mode(enum radio_mode m, uint8_t pipes);
int radio_listen(void);
int radio_send(uint8_t pipe, const uint8_t *buf, uint8_t len, uint8_t retries);
int radio_ack_payload(uint8_t pipe, const uint8_t *buf, uint8_t len);
/* Take the next queued event if it is the ACK payload of the packet just
 * acknowledged (same pipe); ESB posts it right after TX success. */
bool radio_take_ack(uint8_t pipe, struct radio_evt *out);
int64_t radio_now_us(void);

/* Peer table, bridge side. */
struct peer {
	bool used, phantom, established;
	uint8_t addr[6];
	link_key_t sk;
	uint32_t tx_counter;
	struct replay rp;
	int64_t last_valid_us, next_ping_us, hold_until_us;
};
extern struct peer peers[NUM_PIPES];
extern uint8_t active_pipe;

/* Discovery, bridge side: one WELCOME offer is outstanding at a time. */
int disco_bridge_offer(void);   /* seal a WELCOME, load it as pipe 0's ACK payload */
int disco_bridge_hello(const struct radio_evt *e); /* returns the pipe, or < 0 */
void disco_bridge_established(uint8_t pipe);
void peer_free(uint8_t pipe, bool report_lost);

/* Discovery, receiver side. */
struct self {
	uint8_t pipe;
	link_key_t sk;
	uint32_t tx_counter;
	struct replay rp;
	uint8_t salt_r[8];
};
extern struct self self;
int disco_receiver_hello(uint8_t *out); /* returns the length */
int disco_receiver_welcome(const struct radio_evt *e); /* sets self; returns the pipe */

/* Session packets for the candidates. */
int session_seal(link_key_t *sk, uint32_t *counter, uint8_t dir, uint8_t kind, uint8_t flags,
		 uint32_t id, const uint8_t *data, uint8_t dlen, uint8_t *out);

/* Records. */
void rec_txdone(const struct radio_evt *e, uint32_t id, uint8_t pipe, uint8_t kind, uint8_t ok,
		uint8_t len, uint8_t ack_len);
void rec_rxpkt(const struct radio_evt *e, uint32_t t, uint32_t id, uint8_t kind);
void rec_link(uint8_t event, uint8_t pipe, const uint8_t addr[6]);
void self_addr(uint8_t out[6]);
