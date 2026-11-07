/* `kmb` commands from the host. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

extern uint8_t g_role; /* 0 none, 1 bridge, 2 receiver */
extern uint8_t g_cand; /* 0, 'A' or 'B' */

void cmd_init(uint32_t boot_id);
/* Handle one line; anything before "kmb " is ignored, lines without it too. */
void cmd_handle(char *line);
void cmd_boot_record(void);
/* False once `kmb reboot` asked the watchdog to reset the dongle. */
bool cmd_feed_watchdog(void);
