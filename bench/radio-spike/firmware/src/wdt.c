#include "wdt.h"

#include <hal/nrf_wdt.h>

#define WDT_TIMEOUT_TICKS (2 * 32768) /* 2 s of LFCLK */

/* A soft reset (Zephyr's fatal error handler) leaves the watchdog running,
 * and a running watchdog can't be configured again: just keep feeding it. */
void wdt_start(void)
{
	if (!nrf_wdt_started_check(NRF_WDT)) {
		nrf_wdt_behaviour_set(NRF_WDT, NRF_WDT_BEHAVIOUR_RUN_SLEEP_MASK);
		nrf_wdt_reload_value_set(NRF_WDT, WDT_TIMEOUT_TICKS);
		nrf_wdt_reload_request_enable(NRF_WDT, NRF_WDT_RR0);
		nrf_wdt_task_trigger(NRF_WDT, NRF_WDT_TASK_START);
	}
	wdt_feed();
}

void wdt_feed(void)
{
	nrf_wdt_reload_request_set(NRF_WDT, NRF_WDT_RR0);
}
