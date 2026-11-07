/*
 * Candidate B: the bridge transmits on demand (PTX); receivers listen (PRX)
 * on their own pipe. With nothing to send, the bridge listens on pipe 0
 * for HELLOs with a WELCOME preloaded. One packet at a time goes to ESB:
 * the active receiver's reports, then its refresh, then pings.
 */
#include <string.h>
#include <zephyr/random/random.h>

#include "cmd.h"
#include "radio.h"
#include "rec.h"
#include "stamp.h"
#include "traffic.h"

#define BUSY_WAIT_US 20000

/* Bridge ------------------------------------------------------------- */

static struct {
	bool busy, prx;
	uint32_t id;
	uint8_t pipe, kind, len;
	int64_t last_active_tx_us;
} br;

static void bridge_listen(void)
{
	if (br.prx) {
		return;
	}
	radio_mode(RADIO_PRX, BIT(0));
	(void)disco_bridge_offer();
	radio_listen();
	br.prx = true;
}

static bool bridge_send(uint8_t pipe, uint8_t kind, uint32_t id, const uint8_t *data, uint8_t dlen,
			uint8_t retries)
{
	uint8_t out[RADIO_MAX_PAYLOAD];
	struct peer *pe = &peers[pipe];

	if (br.prx) {
		radio_mode(RADIO_PTX, 0xFF);
		br.prx = false;
	}
	int n = session_seal(&pe->sk, &pe->tx_counter, LINK_DIR_TO_RECEIVER, kind,
			     pipe == active_pipe ? PKT_FLAG_ACTIVE : 0, id, data, dlen, out);

	if (n < 0 || radio_send(pipe, out, n, retries) != 0) {
		return false;
	}
	br.busy = true;
	br.id = id;
	br.pipe = pipe;
	br.kind = kind;
	br.len = n;
	if (pipe == active_pipe) {
		br.last_active_tx_us = radio_now_us();
	}
	return true;
}

static void bridge_status(const struct radio_evt *e)
{
	struct peer *pe = &peers[e->pipe];
	struct pkt_body b;

	if (e->pipe == 0 || (!pe->used && pe->hold_until_us == 0)) {
		return;
	}
	int n = link_open(&pe->sk, LINK_DIR_TO_BRIDGE, &pe->rp, e->data, e->len, (uint8_t *)&b);

	if (n == -EALREADY) {
		atomic_inc(&counters.replay_reject);
		return;
	}
	if (n < 0) {
		atomic_inc(&counters.mic_fail);
		return;
	}
	pe->last_valid_us = radio_now_us();
	rec_rxpkt(e, stamp_now(), b.id, b.kind);
	if (!pe->established) {
		disco_bridge_established(e->pipe);
	}
}

static void bridge_event(const struct radio_evt *e)
{
	struct radio_evt ack;

	switch (e->type) {
	case RADIO_EVT_TX_OK:
	case RADIO_EVT_TX_FAIL: {
		if (!br.busy) {
			return;
		}
		bool ok = e->type == RADIO_EVT_TX_OK;
		bool has_ack = ok && radio_take_ack(br.pipe, &ack);

		rec_txdone(e, br.id, br.pipe, br.kind, ok, br.len, has_ack ? ack.len : 0);
		br.busy = false;
		if (has_ack) {
			bridge_status(&ack);
		}
		break;
	}
	case RADIO_EVT_RX:
		if (br.prx && e->pipe == 0) {
			rec_rxpkt(e, stamp_now(), 0, PKT_HELLO);
			(void)disco_bridge_hello(e);
			/* ESB queues it behind the offer the HELLO just got. */
			(void)disco_bridge_offer();
		}
		break;
	default:
		break;
	}
}

static uint32_t bridge_tick(void)
{
	int64_t now = radio_now_us(), next = INT64_MAX;
	struct report rep;
	uint8_t data[11] = {0};

	for (uint8_t p = 1; p < NUM_PIPES; p++) {
		struct peer *pe = &peers[p];

		if (!pe->used && pe->hold_until_us && now >= pe->hold_until_us) {
			peer_free(p, false); /* offer taken, no session: free again */
		} else if (pe->used && !pe->phantom &&
			   now - pe->last_valid_us > params.loss_ms * 1000LL) {
			peer_free(p, true);
		}
	}

	if (!br.busy) {
		bool live = active_pipe && peers[active_pipe].used;

		if (live && traffic_next(active_pipe, &rep)) {
			report_data(&rep, data);
			bridge_send(active_pipe, PKT_REPORT, rep.id, data, 11, params.retries_report);
		} else if (live && now - br.last_active_tx_us >= params.refresh_ms * 1000LL) {
			data[0] = REPORT_REFRESH;
			bridge_send(active_pipe, PKT_REPORT, pkt_new_id(), data, 11,
				    params.retries_report);
		} else {
			for (uint8_t p = 1; p < NUM_PIPES; p++) {
				struct peer *pe = &peers[p];
				bool pending = !pe->used && pe->hold_until_us > now;

				if ((pe->used || pending) && p != active_pipe &&
				    now >= pe->next_ping_us) {
					pe->next_ping_us = now + params.ping_ms * 1000LL;
					bridge_send(p, PKT_PING, pkt_new_id(), NULL, 0,
						    params.retries_ping);
					break;
				}
			}
		}
		if (!br.busy) {
			bridge_listen();
		}
	}
	if (br.busy) {
		return BUSY_WAIT_US;
	}

	if (active_pipe && peers[active_pipe].used) {
		next = MIN(next, br.last_active_tx_us + params.refresh_ms * 1000LL);
	}
	for (uint8_t p = 1; p < NUM_PIPES; p++) {
		struct peer *pe = &peers[p];

		if (pe->used || pe->hold_until_us) {
			if (p != active_pipe) {
				next = MIN(next, pe->next_ping_us);
			}
			if (pe->hold_until_us) {
				next = MIN(next, pe->hold_until_us);
			}
			if (pe->used && !pe->phantom) {
				next = MIN(next, pe->last_valid_us + params.loss_ms * 1000LL + 1);
			}
		}
	}
	return next == INT64_MAX ? UINT32_MAX : (uint32_t)CLAMP(next - now, 0, 1000000);
}

static void bridge_active(uint8_t pipe)
{
	if (pipe && (pipe >= NUM_PIPES || !peers[pipe].used)) {
		rec_text("error active: pipe %u not linked", pipe);
		return;
	}
	if (active_pipe) {
		rec_link(LINK_INACTIVE, active_pipe, peers[active_pipe].addr);
	}
	active_pipe = pipe;
	br.last_active_tx_us = 0; /* tell it at once: refresh is due */
	if (pipe) {
		rec_link(LINK_ACTIVE, pipe, peers[pipe].addr);
	}
	rec_text("ok active %u", pipe);
}

static void bridge_phantom(uint8_t n)
{
	for (uint8_t p = NUM_PIPES - 1; p >= 1 && n; p--) {
		struct peer *pe = &peers[p];

		if (!pe->used && !pe->hold_until_us) {
			memset(pe, 0, sizeof(*pe));
			pe->used = pe->phantom = pe->established = true;
			sys_rand_get(pe->sk.bytes, sizeof(pe->sk.bytes));
			rec_link(LINK_ESTABLISHED, p, pe->addr);
			n--;
		}
	}
	rec_text("ok phantom%s", n ? " (no free pipe)" : "");
}

/* Receiver ----------------------------------------------------------- */

enum { UNLINKED, PENDING, LINKED };

static struct {
	uint8_t st;
	bool busy, active;
	uint32_t hello_id;
	uint8_t hello_len;
	int64_t next_hello_us, pending_until_us, last_valid_us;
} rx;

static void rx_unlink(bool lost)
{
	if (lost) {
		rec_link(LINK_LOST, self.pipe, NULL);
	}
	link_key_destroy(&self.sk);
	rx.st = UNLINKED;
	rx.busy = false;
	rx.active = false;
	rx.next_hello_us = 0;
	radio_mode(RADIO_PTX, BIT(0));
}

static void rx_load_status(void)
{
	uint8_t out[RADIO_MAX_PAYLOAD], data[2] = {1 /* USB configured */, 0};
	int n = session_seal(&self.sk, &self.tx_counter, LINK_DIR_TO_BRIDGE, PKT_STATUS,
			     rx.active ? PKT_FLAG_ACTIVE : 0, pkt_new_id(), data, 2, out);

	(void)radio_ack_payload(self.pipe, out, n);
}

static void receiver_event(const struct radio_evt *e)
{
	struct radio_evt ack;
	struct pkt_body b;

	if (rx.st == UNLINKED) {
		if ((e->type != RADIO_EVT_TX_OK && e->type != RADIO_EVT_TX_FAIL) || !rx.busy) {
			return;
		}
		bool ok = e->type == RADIO_EVT_TX_OK;
		bool has_ack = ok && radio_take_ack(0, &ack);

		rec_txdone(e, rx.hello_id, 0, PKT_HELLO, ok, rx.hello_len, has_ack ? ack.len : 0);
		rx.busy = false;
		if (has_ack && disco_receiver_welcome(&ack) > 0) {
			rx.st = PENDING;
			rx.pending_until_us = radio_now_us() + 2 * params.hold_ms * 1000LL;
			radio_mode(RADIO_PRX, BIT(self.pipe));
			rx_load_status();
			radio_listen();
		}
		return;
	}
	if (e->type != RADIO_EVT_RX || e->pipe != self.pipe) {
		return;
	}
	int n = link_open(&self.sk, LINK_DIR_TO_RECEIVER, &self.rp, e->data, e->len, (uint8_t *)&b);

	if (n == -EALREADY) {
		atomic_inc(&counters.replay_reject);
		return;
	}
	if (n < 0) {
		atomic_inc(&counters.mic_fail);
		return;
	}
	rec_rxpkt(e, stamp_now(), b.id, b.kind);
	rx.last_valid_us = radio_now_us();
	if (rx.st == PENDING) {
		rx.st = LINKED;
		rec_link(LINK_ESTABLISHED, self.pipe, NULL);
	}
	bool act = b.flags & PKT_FLAG_ACTIVE;

	if (act != rx.active) {
		rx.active = act;
		rec_link(act ? LINK_ACTIVE : LINK_INACTIVE, self.pipe, NULL);
	}
	rx_load_status();
}

static uint32_t receiver_tick(void)
{
	int64_t now = radio_now_us();
	uint8_t out[RADIO_MAX_PAYLOAD];

	switch (rx.st) {
	case UNLINKED:
		if (!rx.busy && now >= rx.next_hello_us) {
			rx.hello_len = disco_receiver_hello(out);
			rx.hello_id = pkt_new_id();
			if (radio_send(0, out, rx.hello_len, params.retries_hello) == 0) {
				rx.busy = true;
			}
			rx.next_hello_us = now + (params.hello_ms +
						  sys_rand32_get() % (params.hello_jitter_ms + 1)) *
							 1000LL;
		}
		return rx.busy ? BUSY_WAIT_US : (uint32_t)MAX(rx.next_hello_us - now, 0);
	case PENDING:
		if (now >= rx.pending_until_us) {
			rx_unlink(false);
			return 0;
		}
		return rx.pending_until_us - now;
	default:
		if (now - rx.last_valid_us > params.loss_ms * 1000LL) {
			rx_unlink(true);
			return 0;
		}
		return rx.last_valid_us + params.loss_ms * 1000LL + 1 - now;
	}
}

/* Both sides ----------------------------------------------------------- */

static void start(void)
{
	memset(&br, 0, sizeof(br));
	memset(&rx, 0, sizeof(rx));
	if (g_role == 1) {
		bridge_listen();
	} else {
		rx_unlink(false);
	}
}

static void stop(void)
{
	radio_mode(RADIO_OFF, 0);
}

static void event(const struct radio_evt *e)
{
	if (g_role == 1) {
		bridge_event(e);
	} else {
		receiver_event(e);
	}
}

static uint32_t tick(void)
{
	return g_role == 1 ? bridge_tick() : receiver_tick();
}

const struct cand cand_b = {
	.start = start, .stop = stop, .event = event, .tick = tick,
	.active = bridge_active, .phantom = bridge_phantom,
};
