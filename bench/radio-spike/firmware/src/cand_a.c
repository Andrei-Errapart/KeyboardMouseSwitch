/*
 * Candidate A: polling, Gazell-style. The bridge is PRX on all pipes and
 * answers polls with ACK payloads; receivers are PTX and poll it, the
 * active one every 500 µs, idle ones every 100 ms.
 *
 * ESB keeps one ACK-payload queue per pipe and drops the payload at the
 * front only when a new packet (new PID) arrives on that pipe, reporting
 * TX_SUCCESS and the new packet's RX in the same interrupt.
 */
#include <string.h>
#include <zephyr/random/random.h>

#include "cmd.h"
#include "radio.h"
#include "rec.h"
#include "stamp.h"
#include "traffic.h"

#define MAX_IN_ESB 4        /* REPORTs per pipe handed to ESB ahead of polls */
#define PENDING_POLL_US 5000 /* until the session's first payload verifies */
#define BUSY_WAIT_US 20000

/* Bridge ------------------------------------------------------------- */

static struct {
	uint8_t in_esb[NUM_PIPES];      /* payloads ESB holds for each pipe */
	bool ack_had[NUM_PIPES];        /* the last ACK on the pipe carried one */
	bool flag_sent[NUM_PIPES];      /* active flag last told to each pipe */
	int64_t last_report_us[NUM_PIPES];
} ab;

static void bridge_load(uint8_t pipe, uint8_t kind, uint8_t flags, uint32_t id,
			const uint8_t *data, uint8_t dlen)
{
	uint8_t out[RADIO_MAX_PAYLOAD];
	struct peer *pe = &peers[pipe];
	int n = session_seal(&pe->sk, &pe->tx_counter, LINK_DIR_TO_RECEIVER, kind, flags, id,
			     data, dlen, out);

	if (n > 0 && radio_ack_payload(pipe, out, n) == 0) {
		ab.in_esb[pipe]++;
		ab.flag_sent[pipe] = flags & PKT_FLAG_ACTIVE;
		if (kind == PKT_REPORT) {
			ab.last_report_us[pipe] = radio_now_us();
		}
	} else if (kind == PKT_REPORT) {
		atomic_inc(&counters.queue_dropped); /* ESB's queue was full */
	}
}

static void bridge_status(uint8_t pipe)
{
	uint8_t data[2] = {0};

	bridge_load(pipe, PKT_STATUS, pipe == active_pipe ? PKT_FLAG_ACTIVE : 0, pkt_new_id(),
		    data, 2);
}

/* Give ESB what each pipe needs; called after every event. */
static void bridge_fill(void)
{
	int64_t now = radio_now_us();
	struct report rep;
	uint8_t data[11];

	for (uint8_t p = 1; p < NUM_PIPES; p++) {
		struct peer *pe = &peers[p];
		bool pending = !pe->used && pe->hold_until_us > now;

		if (!pe->used && !pending) {
			continue;
		}
		if (p == active_pipe && pe->used) {
			while (ab.in_esb[p] < MAX_IN_ESB && traffic_next(p, &rep)) {
				/* "more pending": more queued behind it right now. */
				uint8_t flags = PKT_FLAG_ACTIVE |
						(traffic_count(p) ? PKT_FLAG_MORE : 0);

				report_data(&rep, data);
				bridge_load(p, PKT_REPORT, flags, rep.id, data, 11);
			}
			/* STATUS only when the flag changed or as the refresh, so a
			 * report never waits behind a STATUS sealed before it. */
			if (ab.in_esb[p] == 0 &&
			    (!ab.flag_sent[p] ||
			     now - ab.last_report_us[p] >= params.refresh_ms * 1000LL)) {
				memset(data, 0, sizeof(data));
				data[0] = REPORT_REFRESH;
				bridge_load(p, PKT_REPORT, PKT_FLAG_ACTIVE, pkt_new_id(), data, 11);
			}
		} else if (ab.in_esb[p] == 0) {
			bridge_status(p); /* idle or pending: a STATUS for every poll */
		}
	}
}

static void bridge_rx(const struct radio_evt *e)
{
	struct pkt_body b;

	if (e->pipe == 0) {
		rec_rxpkt(e, stamp_now(), 0, PKT_HELLO);
		int p = disco_bridge_hello(e);

		if (p > 0) {
			ab.in_esb[p] = 0;
			ab.ack_had[p] = false;
			ab.flag_sent[p] = false;
		}
		(void)disco_bridge_offer();
		return;
	}
	struct peer *pe = &peers[e->pipe];

	/* Mirror ESB (prepare_ack_pdu_dpl): on a new packet it drops the
	 * payload the previous ACK carried, then the ACK carries the next one,
	 * if any. ESB reports only new packets, valid or not. A payload loaded
	 * between ESB building an ACK and this event makes the count err low
	 * for one poll, never high, and it corrects itself. */
	if (ab.ack_had[e->pipe] && ab.in_esb[e->pipe]) {
		ab.in_esb[e->pipe]--;
	}
	ab.ack_had[e->pipe] = ab.in_esb[e->pipe] > 0;

	if (!pe->used && pe->hold_until_us == 0) {
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
	switch (e->type) {
	case RADIO_EVT_RX:
		bridge_rx(e);
		break;
	default:
		break;
	}
}

static uint32_t bridge_tick(void)
{
	int64_t now = radio_now_us(), next = INT64_MAX;

	for (uint8_t p = 1; p < NUM_PIPES; p++) {
		struct peer *pe = &peers[p];

		if (!pe->used && pe->hold_until_us && now >= pe->hold_until_us) {
			peer_free(p, false);
		} else if (pe->used && !pe->phantom &&
			   now - pe->last_valid_us > params.loss_ms * 1000LL) {
			peer_free(p, true);
		}
		if (pe->hold_until_us) {
			next = MIN(next, pe->hold_until_us);
		}
		if (pe->used && !pe->phantom) {
			next = MIN(next, pe->last_valid_us + params.loss_ms * 1000LL + 1);
		}
	}
	bridge_fill();
	if (active_pipe && peers[active_pipe].used) {
		next = MIN(next, ab.last_report_us[active_pipe] + params.refresh_ms * 1000LL);
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
	if (pipe) {
		ab.flag_sent[pipe] = false; /* tell it with its next payload */
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
			ab.in_esb[p] = 0;
			ab.ack_had[p] = false;
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
	bool busy, active, more;
	uint32_t tx_id;
	uint8_t tx_pipe, tx_kind, tx_len;
	int64_t next_tx_us, pending_until_us, last_valid_us;
} ar;

static void rx_unlink(bool lost)
{
	if (lost) {
		rec_link(LINK_LOST, self.pipe, NULL);
	}
	link_key_destroy(&self.sk);
	memset(&ar, 0, sizeof(ar));
}

static void rx_payload(const struct radio_evt *e)
{
	struct pkt_body b;
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
	ar.last_valid_us = radio_now_us();
	if (ar.st == PENDING) {
		ar.st = LINKED;
		rec_link(LINK_ESTABLISHED, self.pipe, NULL);
	}
	bool act = b.flags & PKT_FLAG_ACTIVE;

	if (act != ar.active) {
		ar.active = act;
		rec_link(act ? LINK_ACTIVE : LINK_INACTIVE, self.pipe, NULL);
	}
	ar.more = b.flags & PKT_FLAG_MORE;
}

static void receiver_event(const struct radio_evt *e)
{
	struct radio_evt ack;

	if ((e->type != RADIO_EVT_TX_OK && e->type != RADIO_EVT_TX_FAIL) || !ar.busy) {
		return;
	}
	bool ok = e->type == RADIO_EVT_TX_OK;
	bool has_ack = ok && radio_take_ack(ar.tx_pipe, &ack);

	rec_txdone(e, ar.tx_id, ar.tx_pipe, ar.tx_kind, ok, ar.tx_len, has_ack ? ack.len : 0);
	ar.busy = false;
	ar.more = false;
	if (!has_ack) {
		return;
	}
	if (ar.st == UNLINKED) {
		if (disco_receiver_welcome(&ack) > 0) {
			ar.st = PENDING;
			ar.pending_until_us = radio_now_us() + 2 * params.hold_ms * 1000LL;
			ar.next_tx_us = 0; /* first poll at once */
		}
	} else {
		rx_payload(&ack);
	}
}

static void send_poll(void)
{
	uint8_t out[RADIO_MAX_PAYLOAD], data[2] = {1 /* USB configured */, 0};

	ar.tx_id = pkt_new_id();
	ar.tx_pipe = self.pipe;
	ar.tx_kind = PKT_POLL;
	ar.tx_len = session_seal(&self.sk, &self.tx_counter, LINK_DIR_TO_BRIDGE, PKT_POLL,
				 ar.active ? PKT_FLAG_ACTIVE : 0, ar.tx_id, data, 2, out);
	if (radio_send(self.pipe, out, ar.tx_len, params.retries_poll) == 0) {
		ar.busy = true;
	}
}

static uint32_t receiver_tick(void)
{
	int64_t now = radio_now_us();
	uint8_t out[RADIO_MAX_PAYLOAD];

	if (ar.st == PENDING && now >= ar.pending_until_us) {
		rx_unlink(false);
	} else if (ar.st == LINKED && now - ar.last_valid_us > params.loss_ms * 1000LL) {
		rx_unlink(true);
	}
	if (ar.busy) {
		return BUSY_WAIT_US;
	}
	if (ar.st == UNLINKED) {
		if (now >= ar.next_tx_us) {
			ar.tx_id = pkt_new_id();
			ar.tx_pipe = 0;
			ar.tx_kind = PKT_HELLO;
			ar.tx_len = disco_receiver_hello(out);
			if (radio_send(0, out, ar.tx_len, params.retries_hello) == 0) {
				ar.busy = true;
			}
			ar.next_tx_us = now + (params.hello_ms +
					       sys_rand32_get() % (params.hello_jitter_ms + 1)) *
						      1000LL;
		}
	} else if (ar.more || now >= ar.next_tx_us) {
		int64_t period = ar.st == PENDING ? PENDING_POLL_US
				 : ar.active      ? params.poll_active_us
						  : params.poll_idle_ms * 1000LL;

		/* Polls keep their rhythm: the next one is due a period after
		 * this one started, not after it finished. */
		ar.next_tx_us = MAX(ar.next_tx_us + period, now);
		send_poll();
	}
	return ar.busy ? BUSY_WAIT_US : (uint32_t)CLAMP(ar.next_tx_us - now, 0, 1000000);
}

/* Both sides ----------------------------------------------------------- */

static void start(void)
{
	memset(&ab, 0, sizeof(ab));
	memset(&ar, 0, sizeof(ar));
	if (g_role == 1) {
		radio_mode(RADIO_PRX, 0xFF);
		(void)disco_bridge_offer();
		radio_listen();
	} else {
		radio_mode(RADIO_PTX, 0xFF);
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

const struct cand cand_a = {
	.start = start, .stop = stop, .event = event, .tick = tick,
	.active = bridge_active, .phantom = bridge_phantom,
};
