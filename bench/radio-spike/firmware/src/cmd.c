#include "cmd.h"

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "fw_version.h"
#include "ident.h"
#include "link.h"
#include "rec.h"
#include "stamp.h"
#include "wdt.h"

#define MAX_ARGS 8

uint8_t g_role;
uint8_t g_cand;
static uint32_t boot_id;
static atomic_t stop_feeding;

void cmd_init(uint32_t id)
{
	boot_id = id;
}

bool cmd_feed_watchdog(void)
{
	return !atomic_get(&stop_feeding);
}

void cmd_boot_record(void)
{
	struct rec_boot b = {
		.type = REC_BOOT,
		.t = stamp_now(),
		.boot_id = boot_id,
		.role = g_role,
		.cand = g_cand,
	};
	char sn[13];

	ident_serial(sn);
	memcpy(b.serial, sn, sizeof(b.serial));
	memcpy(b.fw, FW_VERSION, MIN(strlen(FW_VERSION), sizeof(b.fw)));
	rec_put(&b, sizeof(b));
}

static void cmd_flood(int argc, char **argv)
{
	unsigned long n = argc > 1 ? strtoul(argv[1], NULL, 10) : 0;

	for (unsigned long i = 0; i < n; i++) {
		rec_stats();
		if (i % 1000 == 0 && cmd_feed_watchdog()) {
			wdt_feed(); /* a long flood outlasts the 2 s watchdog */
		}
	}
	rec_text("ok flood %lu", n);
}

void cmd_handle(char *line)
{
	char *argv[MAX_ARGS];
	int argc = 0;
	char *p = strstr(line, "kmb ");

	if (p == NULL) {
		return;
	}
	for (char *tok = strtok(p + 4, " \t"); tok && argc < MAX_ARGS;
	     tok = strtok(NULL, " \t")) {
		argv[argc++] = tok;
	}
	if (argc == 0) {
		return;
	}

	if (!strcmp(argv[0], "info")) {
		cmd_boot_record();
	} else if (!strcmp(argv[0], "stats")) {
		rec_stats();
	} else if (!strcmp(argv[0], "dfu")) {
		rec_text("ok dfu");
		k_msleep(100); /* let the reply reach the host */
		ident_reboot_to_bootloader();
	} else if (!strcmp(argv[0], "reboot")) {
		/* Through the watchdog: a soft reset would leave it running. */
		rec_text("ok reboot");
		atomic_set(&stop_feeding, 1);
	} else if (!strcmp(argv[0], "hang")) {
		rec_text("ok hang");
		for (;;) {
			k_sleep(K_FOREVER);
		}
	} else if (!strcmp(argv[0], "selftest")) {
		link_selftest();
	} else if (!strcmp(argv[0], "flood")) {
		cmd_flood(argc, argv);
	} else {
		rec_text("error unknown command %s", argv[0]);
	}
}
