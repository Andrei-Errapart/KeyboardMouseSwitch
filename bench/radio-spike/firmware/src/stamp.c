/*
 * TIMER3 counts microseconds. A PPI channel from RADIO EVENTS_ADDRESS
 * captures it into CC[1] and, through the fork, triggers EGU3. The EGU3
 * interrupt, at a higher priority than ESB's event interrupt (2) and equal
 * to its radio interrupt (0, see prj.conf), files the capture as sent or
 * received from RADIO->STATE before ESB's callback runs. The two don't
 * collide: ADDRESS comes well after the radio interrupt that set up the
 * packet has returned. ESB uses
 * TIMER2 and EGU0 on the nRF52840, so TIMER3 and EGU3 are free.
 */
#include "stamp.h"

#include <hal/nrf_egu.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_timer.h>
#include <helpers/nrfx_gppi.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#define STAMP_TIMER NRF_TIMER3
#define STAMP_EGU NRF_EGU3
#define STAMP_IRQ SWI3_EGU3_IRQn
#define STAMP_IRQ_PRIO 0

static volatile uint32_t last_tx, last_rx, n_tx, n_rx, n_lost;

static void stamp_isr(const void *arg)
{
	ARG_UNUSED(arg);

	if (!nrf_egu_event_check(STAMP_EGU, NRF_EGU_EVENT_TRIGGERED0)) {
		return;
	}
	nrf_egu_event_clear(STAMP_EGU, NRF_EGU_EVENT_TRIGGERED0);

	uint32_t t = nrf_timer_cc_get(STAMP_TIMER, NRF_TIMER_CC_CHANNEL1);

	/* A late interrupt may find the packet over and the radio disabling;
	 * that still tells the direction. Ramp-up states belong to the next
	 * packet (the ACK turnaround), so they don't. */
	switch (nrf_radio_state_get(NRF_RADIO)) {
	case NRF_RADIO_STATE_TX:
	case NRF_RADIO_STATE_TXDISABLE:
		last_tx = t;
		n_tx++;
		break;
	case NRF_RADIO_STATE_RX:
	case NRF_RADIO_STATE_RXDISABLE:
		last_rx = t;
		n_rx++;
		break;
	default:
		/* Ran too late to tell the direction. Forget both stamps, so no
		 * record carries a stale one into a false clock pair. */
		last_tx = 0;
		last_rx = 0;
		n_lost++;
		break;
	}
}

int stamp_init(void)
{
	nrfx_gppi_handle_t h;
	int err;

	nrf_timer_mode_set(STAMP_TIMER, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(STAMP_TIMER, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(STAMP_TIMER, 4); /* 16 MHz / 2^4 = 1 MHz */
	nrf_timer_task_trigger(STAMP_TIMER, NRF_TIMER_TASK_CLEAR);
	nrf_timer_task_trigger(STAMP_TIMER, NRF_TIMER_TASK_START);

	nrf_egu_event_clear(STAMP_EGU, NRF_EGU_EVENT_TRIGGERED0);
	nrf_egu_int_enable(STAMP_EGU, NRF_EGU_INT_TRIGGERED0);
	IRQ_CONNECT(SWI3_EGU3_IRQn, STAMP_IRQ_PRIO, stamp_isr, NULL, 0);
	irq_enable(STAMP_IRQ);

	err = nrfx_gppi_conn_alloc(
		nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS),
		nrf_timer_task_address_get(STAMP_TIMER, NRF_TIMER_TASK_CAPTURE1), &h);
	if (err) {
		return err;
	}
	err = nrfx_gppi_ep_attach(nrf_egu_task_address_get(STAMP_EGU, NRF_EGU_TASK_TRIGGER0), h);
	if (err) {
		return err;
	}
	nrfx_gppi_conn_enable(h);
	return 0;
}

uint32_t stamp_now(void)
{
	unsigned int key = irq_lock();

	nrf_timer_task_trigger(STAMP_TIMER, NRF_TIMER_TASK_CAPTURE0);
	uint32_t t = nrf_timer_cc_get(STAMP_TIMER, NRF_TIMER_CC_CHANNEL0);

	irq_unlock(key);
	return t;
}

uint32_t stamp_last_tx_addr(void)
{
	return last_tx;
}

uint32_t stamp_last_rx_addr(void)
{
	return last_rx;
}

void stamp_counts(uint32_t *tx, uint32_t *rx)
{
	*tx = n_tx;
	*rx = n_rx;
}

uint32_t stamp_lost(void)
{
	return n_lost;
}
