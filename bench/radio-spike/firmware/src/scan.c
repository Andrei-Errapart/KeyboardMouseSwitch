/*
 * `kmb scan <dwell_ms>`: RSSI on channels 0..80, radio otherwise idle.
 * Runs in the radio thread and sleeps between samples, so the main loop
 * keeps feeding the watchdog and the USB stack keeps running.
 */
#include <hal/nrf_radio.h>
#include <string.h>
#include <zephyr/kernel.h>

#include "radio.h"
#include "rec.h"
#include "stamp.h"

#define BUSY_DBM 80 /* samples above -80 dBm count as busy */
#define SAMPLE_GAP_US 100

static void wait_event(nrf_radio_event_t ev)
{
	while (!nrf_radio_event_check(NRF_RADIO, ev)) {
	}
	nrf_radio_event_clear(NRF_RADIO, ev);
}

void scan_run(uint16_t dwell_ms)
{
	static uint16_t hist[128];

	radio_mode(RADIO_OFF, 0);
	nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
	for (uint8_t ch = 0; ch <= 80; ch++) {
		uint32_t n = 0, busy = 0, half, acc = 0;
		uint8_t lo = 127, hi = 0, median = 127;
		int64_t end = k_uptime_get() + dwell_ms;

		memset(hist, 0, sizeof(hist));
		nrf_radio_frequency_set(NRF_RADIO, 2400 + ch);
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
		wait_event(NRF_RADIO_EVENT_READY);
		while (k_uptime_get() < end) {
			nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND);
			nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
			wait_event(NRF_RADIO_EVENT_RSSIEND);
			uint8_t s = nrf_radio_rssi_sample_get(NRF_RADIO) & 0x7f; /* -dBm */

			hist[s]++;
			lo = MIN(lo, s);
			hi = MAX(hi, s);
			busy += s < BUSY_DBM;
			n++;
			k_usleep(SAMPLE_GAP_US);
		}
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
		wait_event(NRF_RADIO_EVENT_DISABLED);

		half = (n + 1) / 2;
		for (int i = 0; i < 128; i++) { /* small magnitude = strong */
			acc += hist[i];
			if (acc >= half) {
				median = i;
				break;
			}
		}
		struct rec_scan r = {
			.type = REC_SCAN, .channel = ch,
			.rssi_min = -(int8_t)hi, .rssi_median = -(int8_t)median,
			.rssi_max = -(int8_t)lo,
			.busy_permille = n ? (uint16_t)(busy * 1000 / n) : 0,
		};

		rec_put(&r, sizeof(r));
	}
	rec_text("ok scan");
}
