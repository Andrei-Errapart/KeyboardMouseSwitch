#include "traffic.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include "radio.h"
#include "rec.h"
#include "stamp.h"

#define QDEPTH 32

static struct pq {
	struct report r[QDEPTH];
	uint8_t head, count;
} q[NUM_PIPES];
static struct k_spinlock lock;

static struct {
	enum pattern p;
	uint8_t pipe;
	uint32_t left, a, b, c, d, in_burst, rng;
	uint8_t key;
} st;

static void expiry(struct k_timer *t);
K_TIMER_DEFINE(traffic_timer, expiry, NULL);

static uint32_t rnd(void)
{
	st.rng ^= st.rng << 13;
	st.rng ^= st.rng >> 17;
	st.rng ^= st.rng << 5;
	return st.rng;
}

static void enqueue(uint8_t pipe, const struct report *rep)
{
	k_spinlock_key_t k = k_spin_lock(&lock);
	struct pq *pq = &q[pipe];

	if (rep->type == REPORT_MOTION && pq->count) {
		struct report *last = &pq->r[(pq->head + pq->count - 1) % QDEPTH];
		int32_t dx = last->dx + rep->dx, dy = last->dy + rep->dy;

		if (last->type == REPORT_MOTION && dx >= INT16_MIN && dx <= INT16_MAX &&
		    dy >= INT16_MIN && dy <= INT16_MAX) {
			struct rec_merged m = {.type = REC_MERGED, .t = stamp_now(), .id = rep->id,
					       .into = last->id, .pipe = pipe};

			last->dx = dx;
			last->dy = dy;
			k_spin_unlock(&lock, k);
			atomic_inc(&counters.merged);
			rec_put(&m, sizeof(m));
			return;
		}
	}
	if (pq->count == QDEPTH) {
		pq->head = (pq->head + 1) % QDEPTH;
		pq->count--;
		atomic_inc(&counters.queue_dropped);
	}
	pq->r[(pq->head + pq->count) % QDEPTH] = *rep;
	pq->count++;
	k_spin_unlock(&lock, k);
}

static void generate(void)
{
	struct report rep = {.id = pkt_new_id()};
	struct rec_queued r = {.type = REC_QUEUED, .t = stamp_now(), .id = rep.id, .pipe = st.pipe};

	if (st.p == PAT_SINGLE) {
		rep.type = REPORT_KEY;
		rep.key = 4 + st.key++ % 26; /* HID usages a..z */
	} else {
		rep.type = REPORT_MOTION;
		rep.dx = 1;
		rep.dy = -1;
	}
	rec_put(&r, sizeof(r));
	enqueue(st.pipe, &rep);
	radio_wake();
}

static void expiry(struct k_timer *t)
{
	uint32_t next_ms;

	if (st.left == 0) {
		return;
	}
	generate();
	if (--st.left == 0) {
		k_timer_stop(&traffic_timer);
		rec_text("ok traffic done");
		return;
	}
	switch (st.p) {
	case PAT_SINGLE:
		next_ms = st.b + (st.c > st.b ? rnd() % (st.c - st.b + 1) : 0);
		break;
	case PAT_BURST:
		if (++st.in_burst < st.b) {
			next_ms = st.c;
		} else {
			st.in_burst = 0;
			next_ms = st.d;
		}
		break;
	default:
		return; /* stream: periodic timer */
	}
	k_timer_start(&traffic_timer, K_MSEC(next_ms), K_NO_WAIT);
}

int traffic_start(enum pattern p, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	uint8_t pipe = radio_active_pipe();

	if (pipe == 0) {
		return -ENODEV;
	}
	k_timer_stop(&traffic_timer);
	st.p = p;
	st.pipe = pipe;
	st.a = a;
	st.b = b;
	st.c = c;
	st.d = d;
	st.in_burst = 0;
	st.rng = stamp_now() | 1;
	st.left = p == PAT_STREAM ? a * b : p == PAT_BURST ? a * b : a;
	if (st.left == 0) {
		return -EINVAL;
	}
	if (p == PAT_STREAM) {
		k_timeout_t period = K_USEC(1000000 / MAX(a, 1));

		k_timer_start(&traffic_timer, period, period);
	} else {
		k_timer_start(&traffic_timer, K_NO_WAIT, K_NO_WAIT);
	}
	return 0;
}

void traffic_stop(void)
{
	k_timer_stop(&traffic_timer);
	k_spinlock_key_t k = k_spin_lock(&lock);

	st.left = 0;
	memset(q, 0, sizeof(q));
	k_spin_unlock(&lock, k);
}

bool traffic_next(uint8_t pipe, struct report *out)
{
	k_spinlock_key_t k = k_spin_lock(&lock);
	struct pq *pq = &q[pipe];
	bool got = pq->count > 0;

	if (got) {
		*out = pq->r[pq->head];
		pq->head = (pq->head + 1) % QDEPTH;
		pq->count--;
	}
	k_spin_unlock(&lock, k);
	return got;
}

void report_data(const struct report *r, uint8_t data[11])
{
	memset(data, 0, 11);
	data[0] = r->type;
	data[1] = r->key;
	sys_put_le16(r->dx, &data[2]);
	sys_put_le16(r->dy, &data[4]);
}
