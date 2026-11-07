/* The nRF52840 watchdog, 2 s, fed from the main loop and long commands. */
#pragma once

void wdt_start(void);
void wdt_feed(void);
