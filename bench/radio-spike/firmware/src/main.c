/*
 * Radio bench spike firmware: see
 * docs/superpowers/plans/2026-11-07-radio-bench-spike.md.
 */
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

#include "cmd.h"
#include "ident.h"
#include "radio.h"
#include "rec.h"
#include "stamp.h"
#include "wdt.h"

#define STATS_PERIOD_MS 1000

/* Hold the HFXO for good: timestamps and the radio need the crystal. */
static int hfxo_hold(void)
{
	static struct onoff_client cli;
	struct onoff_manager *mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	int err, res;

	if (mgr == NULL) {
		return -ENODEV;
	}
	sys_notify_init_spinwait(&cli.notify);
	err = onoff_request(mgr, &cli);
	if (err < 0) {
		return err;
	}
	do {
		err = sys_notify_fetch_result(&cli.notify, &res);
		if (err == -EAGAIN) {
			k_yield();
		}
	} while (err == -EAGAIN);
	return err ?: res;
}

static void usb_msg(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	switch (msg->type) {
	case USBD_MSG_VBUS_READY:
		if (usbd_can_detect_vbus(ctx)) {
			(void)usbd_enable(ctx);
		}
		break;
	case USBD_MSG_VBUS_REMOVED:
		if (usbd_can_detect_vbus(ctx)) {
			(void)usbd_disable(ctx);
		}
		break;
	case USBD_MSG_CDC_ACM_CONTROL_LINE_STATE:
	case USBD_MSG_RESET:
	case USBD_MSG_SUSPEND:
	case USBD_MSG_RESUME:
		rec_dtr_update();
		break;
	default:
		break;
	}
}

int main(void)
{
	char line[CMD_LINE_MAX];
	int err_clk, err_stamp, err_rec, err_usb;
	int64_t next_stats;

	wdt_start();
	err_clk = hfxo_hold();
	err_stamp = stamp_init();
	err_rec = rec_init();
	cmd_init(sys_rand32_get());
	cmd_boot_record();
	err_usb = ident_usb_init(usb_msg);
	radio_init();
	cmd_resume();
	if (err_clk || err_stamp || err_rec || err_usb) {
		rec_text("error init clk=%d stamp=%d rec=%d usb=%d", err_clk,
			 err_stamp, err_rec, err_usb);
	}

	next_stats = k_uptime_get() + STATS_PERIOD_MS;
	for (;;) {
		if (cmd_feed_watchdog()) {
			wdt_feed();
		}
		if (rec_get_line(line, K_MSEC(100))) {
			cmd_handle(line);
		}
		int64_t now = k_uptime_get();

		if (now >= next_stats) {
			next_stats = now + STATS_PERIOD_MS;
			/* Only while the port is open: see the plan, Task 3. */
			if (rec_dtr()) {
				rec_stats();
			}
		}
	}
	return 0;
}
