#include "cmd.h"

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/atomic.h>

#include "fw_version.h"
#include "ident.h"
#include "link.h"
#include "radio.h"
#include "traffic.h"
#include "rec.h"
#include "stamp.h"
#include "wdt.h"

#define MAX_ARGS 8

uint8_t g_role;
uint8_t g_cand;
static uint32_t boot_id;
static atomic_t stop_feeding;

/* `kmb reboot` keeps the role and settings across the watchdog reset, so
 * discovery (T7) runs from boot without waiting for the host. RAM survives
 * a watchdog reset; Zephyr doesn't clear .noinit. */
#define KEEP_MAGIC 0x4B4D5352
static __noinit struct {
	uint32_t magic;
	uint8_t role, cand;
	struct bench_params params;
} keep;

void cmd_resume(void)
{
	if (keep.magic != KEEP_MAGIC) {
		return;
	}
	keep.magic = 0;
	params = keep.params;
	if (keep.role) {
		radio_command(CMD_ROLE, keep.role, keep.cand);
	}
}

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

static const struct {
	const char *name;
	void *p;
	uint8_t size;
} settable[] = {
	{"ch", &params.channel, 1},
	{"power", &params.tx_power, 1},
	{"ard", &params.ard_us, 2},
	{"retries_report", &params.retries_report, 1},
	{"retries_poll", &params.retries_poll, 1},
	{"retries_ping", &params.retries_ping, 1},
	{"retries_hello", &params.retries_hello, 1},
	{"ping_ms", &params.ping_ms, 2},
	{"refresh_ms", &params.refresh_ms, 2},
	{"hello_ms", &params.hello_ms, 2},
	{"hello_jitter_ms", &params.hello_jitter_ms, 2},
	{"poll_active_us", &params.poll_active_us, 2},
	{"poll_idle_ms", &params.poll_idle_ms, 2},
	{"loss_ms", &params.loss_ms, 2},
	{"hold_ms", &params.hold_ms, 2},
	{"fast_ramp", &params.fast_ramp, 1},
};

/* Takes effect at the next `kmb role`. */
static void cmd_set(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "defaults")) {
		params = params_default; /* settings outlive roles: runs reset them */
		rec_text("ok set defaults");
		return;
	}
	if (argc < 3) {
		rec_text("error set: name value");
		return;
	}
	long v = strtol(argv[2], NULL, 0);

	for (size_t i = 0; i < ARRAY_SIZE(settable); i++) {
		if (!strcmp(argv[1], settable[i].name)) {
			if (settable[i].size == 1) {
				*(uint8_t *)settable[i].p = (uint8_t)v;
			} else {
				*(uint16_t *)settable[i].p = (uint16_t)v;
			}
			rec_text("ok set %s %ld", argv[1], v);
			return;
		}
	}
	rec_text("error set: unknown %s", argv[1]);
}

static void cmd_role(int argc, char **argv)
{
	uint8_t role = argc > 1 && !strcmp(argv[1], "bridge")     ? 1
		       : argc > 1 && !strcmp(argv[1], "receiver") ? 2
								  : 0;
	uint8_t cand = argc > 2 ? argv[2][0] : 0;

	if (role && cand != 'A' && cand != 'B') {
		rec_text("error role: bridge|receiver|none A|B");
		return;
	}
	radio_command(CMD_ROLE, role, cand);
}

static void cmd_traffic(int argc, char **argv)
{
	uint32_t v[4] = {0};
	int err = -EINVAL;

	for (int i = 2; i < argc && i < 6; i++) {
		v[i - 2] = strtoul(argv[i], NULL, 10);
	}
	if (argc < 2) {
		;
	} else if (!strcmp(argv[1], "stop")) {
		traffic_stop();
		err = 0;
	} else if (!strcmp(argv[1], "single") && argc >= 5) {
		err = traffic_start(PAT_SINGLE, v[0], v[1], v[2], 0);
	} else if (!strcmp(argv[1], "burst") && argc >= 6) {
		err = traffic_start(PAT_BURST, v[0], v[1], v[2], v[3]);
	} else if (!strcmp(argv[1], "stream") && argc >= 4) {
		err = traffic_start(PAT_STREAM, v[0], v[1], 0, 0);
	}
	if (err) {
		rec_text("error traffic %d", err);
	} else {
		rec_text("ok traffic %s", argv[1]);
	}
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
		keep.role = g_role;
		keep.cand = g_cand;
		keep.params = params;
		keep.magic = KEEP_MAGIC;
		rec_text("ok reboot");
		atomic_set(&stop_feeding, 1);
	} else if (!strcmp(argv[0], "hang")) {
		rec_text("ok hang");
		for (;;) {
			k_sleep(K_FOREVER);
		}
	} else if (!strcmp(argv[0], "role")) {
		cmd_role(argc, argv);
	} else if (!strcmp(argv[0], "active")) {
		radio_command(CMD_ACTIVE,
			      argc > 1 && strcmp(argv[1], "none") ? strtoul(argv[1], NULL, 10) : 0, 0);
	} else if (!strcmp(argv[0], "phantom")) {
		radio_command(CMD_PHANTOM, argc > 1 ? strtoul(argv[1], NULL, 10) : 1, 0);
	} else if (!strcmp(argv[0], "scan")) {
		unsigned long dwell = argc > 1 ? strtoul(argv[1], NULL, 10) : 200;

		radio_command(CMD_SCAN, dwell & 0xff, (dwell >> 8) & 0xff);
	} else if (!strcmp(argv[0], "set")) {
		cmd_set(argc, argv);
	} else if (!strcmp(argv[0], "traffic")) {
		cmd_traffic(argc, argv);
	} else if (!strcmp(argv[0], "selftest")) {
		link_selftest();
	} else if (!strcmp(argv[0], "flood")) {
		cmd_flood(argc, argv);
	} else {
		rec_text("error unknown command %s", argv[0]);
	}
}
