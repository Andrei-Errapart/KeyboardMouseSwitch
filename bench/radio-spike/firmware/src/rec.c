#include "rec.h"

#include <stdarg.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>

#include "stamp.h"

#define RING_SIZE 8192
#define MAX_REC 80
#define SLIP_END 0xC0
#define SLIP_ESC 0xDB

struct bench_counters counters;

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(board_cdc_acm_uart));
RING_BUF_DECLARE(ring, RING_SIZE);
static struct k_spinlock lock;
static uint32_t dropped;
static atomic_t dtr;
/* 1 while the CDC writer is stopped. Waking it costs a work-queue round
 * trip (about 300 µs of the caller's time), so only a put that finds it
 * idle wakes it; the driver calls back by itself while TX stays enabled. */
static atomic_t tx_idle = ATOMIC_INIT(1);

static void tx_wake(void)
{
	if (atomic_get(&dtr) && atomic_cas(&tx_idle, 1, 0)) {
		uart_irq_tx_enable(uart);
	}
}

K_MSGQ_DEFINE(lines, CMD_LINE_MAX, 4, 1);
static char line_buf[CMD_LINE_MAX];
static size_t line_len;

static size_t slip_encode(const uint8_t *in, size_t len, uint8_t *out)
{
	size_t n = 0;

	for (size_t i = 0; i < len; i++) {
		if (in[i] == SLIP_END) {
			out[n++] = SLIP_ESC;
			out[n++] = 0xDC;
		} else if (in[i] == SLIP_ESC) {
			out[n++] = SLIP_ESC;
			out[n++] = 0xDD;
		} else {
			out[n++] = in[i];
		}
	}
	out[n++] = SLIP_END;
	return n;
}

bool rec_put(const void *rec, size_t len)
{
	uint8_t buf[2 * MAX_REC + 1];
	bool ok;

	if (len > MAX_REC) {
		len = MAX_REC;
	}
	size_t n = slip_encode(rec, len, buf);
	k_spinlock_key_t key = k_spin_lock(&lock);

	ok = ring_buf_space_get(&ring) >= n;
	if (ok) {
		ring_buf_put(&ring, buf, n);
	} else {
		dropped++;
	}
	k_spin_unlock(&lock, key);
	if (ok) {
		tx_wake();
	}
	return ok;
}

void rec_text(const char *fmt, ...)
{
	uint8_t rec[1 + 64];
	va_list ap;

	rec[0] = REC_TEXT;
	va_start(ap, fmt);
	int n = vsnprintk((char *)&rec[1], sizeof(rec) - 1, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	rec_put(rec, 1 + MIN((size_t)n, sizeof(rec) - 2));
}

uint32_t rec_dropped(void)
{
	return dropped;
}

void rec_stats(void)
{
	struct rec_stats s = {
		.type = REC_STATS,
		.t = stamp_now(),
		.rec_dropped = dropped,
		.stamp_lost = stamp_lost(),
		.mic_fail = atomic_get(&counters.mic_fail),
		.replay_reject = atomic_get(&counters.replay_reject),
		.merged = atomic_get(&counters.merged),
		.queue_dropped = atomic_get(&counters.queue_dropped),
		.mode_switches = atomic_get(&counters.mode_switches),
		.mode_switch_us_total = atomic_get(&counters.mode_switch_us_total),
		.mode_switch_us_max = MIN(atomic_get(&counters.mode_switch_us_max), UINT16_MAX),
	};
	uint32_t tx, rx;

	stamp_counts(&tx, &rx);
	s.addr_tx = tx;
	s.addr_rx = rx;
	rec_put(&s, sizeof(s));
}

void rec_dtr_update(void)
{
	uint32_t v = 0;

	uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &v);
	atomic_set(&dtr, v ? 1 : 0);
	tx_wake();
}

bool rec_dtr(void)
{
	return atomic_get(&dtr) != 0;
}

bool rec_get_line(char line[CMD_LINE_MAX], k_timeout_t timeout)
{
	return k_msgq_get(&lines, line, timeout) == 0;
}

static void rx_byte(uint8_t b)
{
	if (b == '\n' || b == '\r') {
		if (line_len > 0) {
			line_buf[line_len] = '\0';
			(void)k_msgq_put(&lines, line_buf, K_NO_WAIT);
			line_len = 0;
		}
	} else if (line_len < CMD_LINE_MAX - 1) {
		line_buf[line_len++] = (char)b;
	} else {
		line_len = 0; /* overlong line: drop it */
	}
}

static void uart_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t buf[64];
			int n = uart_fifo_read(dev, buf, sizeof(buf));

			for (int i = 0; i < n; i++) {
				rx_byte(buf[i]);
			}
		}
		if (uart_irq_tx_ready(dev)) {
			uint8_t *data;
			uint32_t n;

			k_spinlock_key_t key = k_spin_lock(&lock);

			n = atomic_get(&dtr) ? ring_buf_get_claim(&ring, &data, 64) : 0;
			k_spin_unlock(&lock, key);
			if (n == 0) {
				/* Stop, then look again: a put between the claim
				 * and tx_idle = 1 saw the writer busy and didn't
				 * wake it. */
				uart_irq_tx_disable(dev);
				atomic_set(&tx_idle, 1);
				if (!ring_buf_is_empty(&ring)) {
					tx_wake();
				}
				break;
			}
			int sent = uart_fifo_fill(dev, data, n);

			key = k_spin_lock(&lock);
			ring_buf_get_finish(&ring, sent > 0 ? sent : 0);
			k_spin_unlock(&lock, key);
		}
	}
}

int rec_init(void)
{
	if (!device_is_ready(uart)) {
		return -ENODEV;
	}
	uart_irq_callback_set(uart, uart_cb);
	uart_irq_rx_enable(uart);
	return 0;
}
