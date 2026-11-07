#include "radio.h"

#include <esb.h>
#include <string.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>

#include "cmd.h"
#include "rec.h"
#include "stamp.h"
#include "traffic.h"

#define RADIO_PRIO K_PRIO_COOP(2)
#define RADIO_STACK 3072

#define PARAMS_DEFAULT {                                                                    \
	/* 2480 MHz: the quietest of 5, 25, 50, 77 and 80 on 2026-11-07 (50,       \
	 * the plan's default, barely linked). T8 picks the channel. */            \
	.channel = 80, .tx_power = 0, .ard_us = 435,                                \
	.retries_report = 3, .retries_poll = 3, .retries_ping = 1, .retries_hello = 0, \
	.ping_ms = 100, .refresh_ms = 25, .hello_ms = 20, .hello_jitter_ms = 10,    \
	.poll_active_us = 500, .poll_idle_ms = 100, .loss_ms = 1000, .hold_ms = 100, \
	.fast_ramp = 1,                                                             \
}
struct bench_params params = PARAMS_DEFAULT;
const struct bench_params params_default = PARAMS_DEFAULT;

static const uint8_t base0[4] = {0x4B, 0x4D, 0x53, 0x44}; /* "KMSD": discovery */
static const uint8_t base1[4] = {0x4B, 0x4D, 0x53, 0x52}; /* "KMSR": receivers */
static const uint8_t prefixes[NUM_PIPES] = {0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8};

K_MSGQ_DEFINE(radio_q, sizeof(struct radio_evt), 32, 4);
K_THREAD_STACK_DEFINE(radio_stack, RADIO_STACK);
static struct k_thread radio_thread;
static atomic_t next_id;
static atomic_t evt_dropped;
static const struct cand *cand;
static enum radio_mode mode;

struct peer peers[NUM_PIPES];
uint8_t active_pipe;
struct self self;

uint32_t pkt_new_id(void)
{
	return (uint32_t)atomic_inc(&next_id) + 1;
}

int64_t radio_now_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

uint8_t radio_active_pipe(void)
{
	return active_pipe;
}

static void post(const struct radio_evt *e)
{
	if (k_msgq_put(&radio_q, e, K_NO_WAIT) != 0) {
		atomic_inc(&evt_dropped);
	}
}

void radio_wake(void)
{
	struct radio_evt e = {.type = RADIO_EVT_WAKE};

	post(&e);
}

void radio_command(enum radio_cmd cmd, uint8_t arg1, uint8_t arg2)
{
	struct radio_evt e = {.type = RADIO_EVT_CMD, .cmd = cmd, .arg1 = arg1, .arg2 = arg2};

	post(&e);
}

/* ESB's event interrupt: stamp, read, post. Nothing slow here. */
static void esb_handler(const struct esb_evt *event)
{
	struct radio_evt e = {
		.t = stamp_now(),
		.t_tx_addr = stamp_last_tx_addr(),
		.t_rx_addr = stamp_last_rx_addr(),
		.attempts = event->tx_attempts,
	};

	switch (event->evt_id) {
	case ESB_EVENT_TX_SUCCESS:
		e.type = RADIO_EVT_TX_OK;
		post(&e);
		break;
	case ESB_EVENT_TX_FAILED:
		/* A failed packet stays at the head of ESB's queue: drop it. */
		(void)esb_pop_tx();
		e.type = RADIO_EVT_TX_FAIL;
		post(&e);
		break;
	case ESB_EVENT_RX_RECEIVED: {
		static struct esb_payload p[4];
		int n = 0;

		while (n < ARRAY_SIZE(p) && esb_read_rx_payload(&p[n]) == 0) {
			n++;
		}
		for (int i = 0; i < n; i++) {
			e.type = RADIO_EVT_RX;
			e.pipe = p[i].pipe;
			e.len = MIN(p[i].length, RADIO_MAX_PAYLOAD);
			e.rssi = p[i].rssi;
			/* Only the newest payload owns the latest RX stamp. */
			e.t_rx_addr = i == n - 1 ? stamp_last_rx_addr() : 0;
			memcpy(e.data, p[i].data, e.len);
			post(&e);
		}
		break;
	}
	default:
		break;
	}
}

static void count_switch(uint32_t dt)
{
	atomic_inc(&counters.mode_switches);
	atomic_add(&counters.mode_switch_us_total, dt);
	atomic_val_t old;

	do {
		old = atomic_get(&counters.mode_switch_us_max);
	} while ((uint32_t)old < dt && !atomic_cas(&counters.mode_switch_us_max, old, dt));
}

int radio_mode(enum radio_mode m, uint8_t pipes)
{
	uint32_t t0 = stamp_now();
	struct esb_config c = ESB_DEFAULT_CONFIG;
	int err;

	esb_disable();
	mode = m;
	if (m == RADIO_OFF) {
		return 0;
	}
	c.protocol = ESB_PROTOCOL_ESB_DPL;
	c.mode = m == RADIO_PTX ? ESB_MODE_PTX : ESB_MODE_PRX;
	c.event_handler = esb_handler;
	c.bitrate = ESB_BITRATE_2MBPS;
	c.crc = ESB_CRC_16BIT;
	c.tx_output_power = params.tx_power;
	c.retransmit_delay = params.ard_us;
	c.retransmit_count = params.retries_report;
	c.tx_mode = ESB_TXMODE_AUTO;
	c.payload_length = RADIO_MAX_PAYLOAD;
	c.selective_auto_ack = false;
	c.use_fast_ramp_up = params.fast_ramp;

	err = esb_init(&c);
	err = err ?: esb_set_base_address_0(base0);
	err = err ?: esb_set_base_address_1(base1);
	err = err ?: esb_set_prefixes(prefixes, NUM_PIPES);
	err = err ?: esb_enable_pipes(pipes);
	err = err ?: esb_set_rf_channel(params.channel);
	count_switch(stamp_now() - t0);
	if (err) {
		rec_text("error radio_mode %d: %d", m, err);
	}
	return err;
}

int radio_listen(void)
{
	return esb_start_rx();
}

int radio_send(uint8_t pipe, const uint8_t *buf, uint8_t len, uint8_t retries)
{
	struct esb_payload p = {.length = len, .pipe = pipe};
	int err = esb_set_retransmit_count(retries);

	memcpy(p.data, buf, len);
	return err ?: esb_write_payload(&p);
}

int radio_ack_payload(uint8_t pipe, const uint8_t *buf, uint8_t len)
{
	struct esb_payload p = {.length = len, .pipe = pipe};

	memcpy(p.data, buf, len);
	return esb_write_payload(&p);
}

bool radio_take_ack(uint8_t pipe, struct radio_evt *out)
{
	if (k_msgq_peek(&radio_q, out) == 0 && out->type == RADIO_EVT_RX && out->pipe == pipe) {
		(void)k_msgq_get(&radio_q, out, K_NO_WAIT);
		return true;
	}
	return false;
}

bool radio_peek(struct radio_evt *out)
{
	return k_msgq_peek(&radio_q, out) == 0;
}

void self_addr(uint8_t out[6])
{
	sys_put_be16((uint16_t)NRF_FICR->DEVICEADDR[1] | 0xC000, out);
	sys_put_be32(NRF_FICR->DEVICEADDR[0], out + 2);
}

/* Records ----------------------------------------------------------- */

void rec_txdone(const struct radio_evt *e, uint32_t id, uint8_t pipe, uint8_t kind, uint8_t ok,
		uint8_t len, uint8_t ack_len)
{
	struct rec_txdone r = {
		.type = REC_TXDONE, .t = e->t, .id = id, .pipe = pipe, .kind = kind,
		.attempts = e->attempts, .ok = ok, .len = len, .ack_len = ack_len,
		.t_tx_addr = e->t_tx_addr, .t_rx_addr = ok ? e->t_rx_addr : 0,
	};

	rec_put(&r, sizeof(r));
}

void rec_rxpkt(const struct radio_evt *e, uint32_t t, uint32_t id, uint8_t kind)
{
	struct rec_rxpkt r = {
		.type = REC_RXPKT, .t = t, .id = id, .pipe = e->pipe, .kind = kind,
		.len = e->len, .rssi = e->rssi, .t_rx_addr = e->t_rx_addr,
	};

	rec_put(&r, sizeof(r));
}

void rec_link(uint8_t event, uint8_t pipe, const uint8_t addr[6])
{
	struct rec_link r = {.type = REC_LINK, .t = stamp_now(), .event = event, .pipe = pipe};

	if (addr) {
		memcpy(r.peer, addr, 6);
	}
	rec_put(&r, sizeof(r));
}

/* Sessions ---------------------------------------------------------- */

int session_seal(link_key_t *sk, uint32_t *counter, uint8_t dir, uint8_t kind, uint8_t flags,
		 uint32_t id, const uint8_t *data, uint8_t dlen, uint8_t *out)
{
	struct pkt_body b = {.kind = kind, .flags = flags, .id = id};
	uint8_t blen = kind == PKT_REPORT ? PKT_BODY_REPORT : PKT_BODY_SHORT;

	memcpy(b.data, data, MIN(dlen, blen - 6));
	return link_seal(sk, (*counter)++, dir, (const uint8_t *)&b, blen, out);
}

/* Discovery: bridge ------------------------------------------------- */

static struct {
	bool valid;
	uint8_t pipe;
	uint8_t salt_b[8];
} offer;

static int free_pipe(void)
{
	int64_t now = radio_now_us();

	for (uint8_t p = 1; p < NUM_PIPES; p++) {
		if (!peers[p].used && peers[p].hold_until_us <= now) {
			return p;
		}
	}
	return -1;
}

int disco_bridge_offer(void)
{
	struct pkt_welcome w = {.kind = PKT_WELCOME};
	uint8_t out[PKT_ONAIR_WELCOME];
	int p = free_pipe();

	if (p < 0) {
		offer.valid = false;
		return -ENOSPC;
	}
	w.pipe = p;
	sys_rand_get(offer.salt_b, sizeof(offer.salt_b));
	offer.pipe = p;
	offer.valid = true;
	int n = link_seal_discovery(LINK_DIR_TO_RECEIVER, offer.salt_b, (uint8_t *)&w, sizeof(w),
				    out);
	return radio_ack_payload(0, out, n);
}

int disco_bridge_hello(const struct radio_evt *e)
{
	struct pkt_hello h;
	uint8_t salt_r[8];

	if (link_open_discovery(LINK_DIR_TO_BRIDGE, e->data, e->len, salt_r, (uint8_t *)&h) !=
		    sizeof(h) ||
	    h.kind != PKT_HELLO) {
		atomic_inc(&counters.mic_fail);
		return -EBADMSG;
	}
	rec_link(LINK_HELLO_HEARD, offer.valid ? offer.pipe : 0, h.rx_addr);
	if (!offer.valid || h.version != PKT_VERSION) {
		return -EAGAIN;
	}
	/* The HELLO got the outstanding offer in its ACK. Hold the pipe for
	 * it; a session replaces an older one only once it is established. */
	struct peer *pe = &peers[offer.pipe];

	memset(pe, 0, sizeof(*pe));
	memcpy(pe->addr, h.rx_addr, 6);
	link_session_key(offer.salt_b, salt_r, &pe->sk);
	pe->hold_until_us = radio_now_us() + params.hold_ms * 1000LL;
	pe->next_ping_us = 0; /* first session packet at once */
	offer.valid = false;
	return offer.pipe;
}

void peer_free(uint8_t pipe, bool report_lost)
{
	if (report_lost) {
		rec_link(LINK_LOST, pipe, peers[pipe].addr);
	}
	link_key_destroy(&peers[pipe].sk);
	memset(&peers[pipe], 0, sizeof(peers[pipe]));
	if (active_pipe == pipe) {
		active_pipe = 0;
	}
}

void disco_bridge_established(uint8_t pipe)
{
	struct peer *pe = &peers[pipe];

	pe->used = true;
	pe->established = true;
	pe->hold_until_us = 0;
	rec_link(LINK_ESTABLISHED, pipe, pe->addr);
	for (uint8_t p = 1; p < NUM_PIPES; p++) {
		if (p != pipe && peers[p].used && !peers[p].phantom &&
		    memcmp(peers[p].addr, pe->addr, 6) == 0) {
			peer_free(p, true); /* the receiver rebooted: new session wins */
		}
	}
}

/* Discovery: receiver ----------------------------------------------- */

int disco_receiver_hello(uint8_t *out)
{
	struct pkt_hello h = {.kind = PKT_HELLO, .version = PKT_VERSION};

	self_addr(h.rx_addr);
	sys_rand_get(self.salt_r, sizeof(self.salt_r));
	return link_seal_discovery(LINK_DIR_TO_BRIDGE, self.salt_r, (uint8_t *)&h, sizeof(h), out);
}

int disco_receiver_welcome(const struct radio_evt *e)
{
	struct pkt_welcome w;
	uint8_t salt_b[8];

	if (link_open_discovery(LINK_DIR_TO_RECEIVER, e->data, e->len, salt_b, (uint8_t *)&w) !=
		    sizeof(w) ||
	    w.kind != PKT_WELCOME || w.pipe < 1 || w.pipe >= NUM_PIPES) {
		atomic_inc(&counters.mic_fail);
		return -EBADMSG;
	}
	memset(&self.rp, 0, sizeof(self.rp));
	self.tx_counter = 0;
	self.pipe = w.pipe;
	link_session_key(salt_b, self.salt_r, &self.sk);
	return w.pipe;
}

/* The radio thread ---------------------------------------------------- */

static void do_command(const struct radio_evt *e)
{
	switch (e->cmd) {
	case CMD_ROLE:
		if (cand) {
			cand->stop();
		}
		traffic_stop();
		radio_mode(RADIO_OFF, 0);
		memset(peers, 0, sizeof(peers));
		active_pipe = 0;
		g_role = e->arg1;
		g_cand = e->arg1 ? e->arg2 : 0;
		cand = g_cand == 'B' ? &cand_b : g_cand == 'A' ? &cand_a : NULL;
		if (cand) {
			cand->start();
		}
		rec_text("ok role %u %c", g_role, g_cand ? g_cand : '-');
		break;
	case CMD_ACTIVE:
		if (cand && cand->active) {
			cand->active(e->arg1);
		}
		break;
	case CMD_PHANTOM:
		if (cand && cand->phantom) {
			cand->phantom(e->arg1);
		}
		break;
	case CMD_SCAN:
		if (cand) {
			cand->stop();
		}
		traffic_stop();
		cand = NULL;
		g_role = 0;
		g_cand = 0;
		scan_run(e->arg1 | (e->arg2 << 8));
		break;
	}
}

static void radio_loop(void *a, void *b, void *c)
{
	struct radio_evt e;

	for (;;) {
		uint32_t wait = cand ? cand->tick() : UINT32_MAX;
		k_timeout_t to = wait == UINT32_MAX ? K_FOREVER : K_USEC(wait);

		if (k_msgq_get(&radio_q, &e, to) != 0) {
			continue;
		}
		if (e.type == RADIO_EVT_CMD) {
			do_command(&e);
		} else if (cand && e.type != RADIO_EVT_WAKE) {
			cand->event(&e);
		}
	}
}

int radio_init(void)
{
	k_thread_create(&radio_thread, radio_stack, K_THREAD_STACK_SIZEOF(radio_stack), radio_loop,
			NULL, NULL, NULL, RADIO_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&radio_thread, "radio");
	return 0;
}
