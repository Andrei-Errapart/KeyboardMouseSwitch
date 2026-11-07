/* Packets on air. See "Packets" in
 * docs/superpowers/plans/2026-11-07-radio-bench-spike.md. */
#pragma once
#include <stdint.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#define PKT_VERSION 1

enum pkt_kind {
	PKT_HELLO = 1, PKT_WELCOME = 2, PKT_REPORT = 3, PKT_PING = 4,
	PKT_STATUS = 5, PKT_POLL = 6,
};

#define PKT_FLAG_ACTIVE BIT(0)
#define PKT_FLAG_MORE BIT(1)

/* Session packets: counter (4, clear) ‖ CCM(body) ‖ MIC (4). */
struct __packed pkt_body {
	uint8_t kind;
	uint8_t flags;
	uint32_t id;
	uint8_t data[11];
};
#define PKT_BODY_REPORT 17 /* REPORT: 11 data bytes */
#define PKT_BODY_SHORT 8   /* PING, POLL, STATUS: 2 data bytes */
BUILD_ASSERT(sizeof(struct pkt_body) == PKT_BODY_REPORT);

/* Discovery packets: salt (8, clear, it is the nonce) ‖ CCM(body) ‖ MIC,
 * under the pre-shared key. */
struct __packed pkt_hello {
	uint8_t kind;
	uint8_t version;
	uint8_t rx_addr[6];
};
struct __packed pkt_welcome {
	uint8_t kind;
	uint8_t pipe;
};

#define PKT_ONAIR_REPORT (4 + PKT_BODY_REPORT + 4)          /* 25 */
#define PKT_ONAIR_SHORT (4 + PKT_BODY_SHORT + 4)            /* 16 */
#define PKT_ONAIR_HELLO (8 + sizeof(struct pkt_hello) + 4)    /* 20 */
#define PKT_ONAIR_WELCOME (8 + sizeof(struct pkt_welcome) + 4) /* 14 */
