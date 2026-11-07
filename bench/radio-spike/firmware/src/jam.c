/*
 * `kmb jam <ch> <percent>`: a controlled interferer for the bench. Sends
 * 25-byte dummy packets on channel <ch> with exponentially distributed
 * gaps, so about <percent> % of the time is on air and the timing never
 * locks to the link under test. `kmb jam off` stops it and reports the
 * on-air share it reached. Drives RADIO directly with ESB off, like the
 * scan, and runs as a pseudo-candidate in the radio thread.
 */
#include <hal/nrf_radio.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

#include "radio.h"
#include "rec.h"
#include "stamp.h"

#define JAM_LEN 25
/* On air at 2 Mbit/s: preamble 2, address 5, length 1, payload, CRC 2. */
#define AIR_US ((2 + 5 + 1 + JAM_LEN + 2) * 4)

static struct {
	uint8_t ch, percent;
	uint32_t sent, t0;
	uint8_t pkt[1 + JAM_LEN];
} jam;

static void wait_event(nrf_radio_event_t ev)
{
	while (!nrf_radio_event_check(NRF_RADIO, ev)) {
	}
	nrf_radio_event_clear(NRF_RADIO, ev);
}

void jam_set(uint8_t ch, uint8_t percent)
{
	jam.ch = ch;
	jam.percent = percent;
}

static void jam_start(void)
{
	nrf_radio_packet_conf_t conf = {
		.lflen = 8, .plen = NRF_RADIO_PREAMBLE_LENGTH_16BIT, .maxlen = JAM_LEN,
		.balen = 4, .big_endian = true, .whiteen = true,
	};

	radio_mode(RADIO_OFF, 0);
	nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
	nrf_radio_modecnf0_set(NRF_RADIO, params.fast_ramp, RADIO_MODECNF0_DTX_Center);
	nrf_radio_packet_configure(NRF_RADIO, &conf);
	nrf_radio_base0_set(NRF_RADIO, 0x4A414D21); /* "JAM!", no bench address */
	nrf_radio_prefix0_set(NRF_RADIO, 0x5A);
	nrf_radio_txaddress_set(NRF_RADIO, 0);
	nrf_radio_crc_configure(NRF_RADIO, 2, NRF_RADIO_CRC_ADDR_INCLUDE, 0x11021);
	nrf_radio_crcinit_set(NRF_RADIO, 0xFFFF);
	nrf_radio_datawhiteiv_set(NRF_RADIO, jam.ch);
	nrf_radio_txpower_set(NRF_RADIO, (nrf_radio_txpower_t)(uint8_t)params.tx_power);
	nrf_radio_frequency_set(NRF_RADIO, 2400 + jam.ch);
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_END_DISABLE_MASK);
	jam.pkt[0] = JAM_LEN;
	sys_rand_get(&jam.pkt[1], JAM_LEN);
	nrf_radio_packetptr_set(NRF_RADIO, jam.pkt);
	jam.sent = 0;
	jam.t0 = stamp_now();
}

static void jam_stop(void)
{
	uint32_t dt = stamp_now() - jam.t0;
	uint32_t permille = dt ? (uint32_t)((uint64_t)jam.sent * AIR_US * 1000 / dt) : 0;

	nrf_radio_shorts_set(NRF_RADIO, 0);
	if (nrf_radio_state_get(NRF_RADIO) != NRF_RADIO_STATE_DISABLED) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
		wait_event(NRF_RADIO_EVENT_DISABLED);
	}
	rec_text("ok jam off: ch %u, %u packets, %u.%u %% on air", jam.ch, jam.sent,
		 permille / 10, permille % 10);
}

static void jam_event(const struct radio_evt *e)
{
	ARG_UNUSED(e);
}

/* One packet, then an exponential gap whose mean, with the ramp-up, puts
 * `percent` of the time on air. */
static uint32_t jam_tick(void)
{
	float ramp = params.fast_ramp ? 40.0f : 130.0f;
	float mean = AIR_US * 100.0f / jam.percent - AIR_US - ramp;
	float u = ((float)sys_rand32_get() + 1.0f) / 4294967296.0f;

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_TXEN);
	wait_event(NRF_RADIO_EVENT_DISABLED);
	jam.sent++;
	return mean > 0.0f ? (uint32_t)(-mean * logf(u)) : 0;
}

const struct cand cand_jam = {
	.start = jam_start, .stop = jam_stop, .event = jam_event, .tick = jam_tick,
};
