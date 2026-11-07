/* Test traffic for the active receiver, generated in a k_timer interrupt
 * (thread-level code is slowed while the host reads records). One queue
 * per receiver; motion merges into the newest report not yet given to ESB. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum pattern { PAT_SINGLE, PAT_BURST, PAT_STREAM };
enum report_type { REPORT_KEY = 1, REPORT_MOTION = 2, REPORT_REFRESH = 3 };

struct report {
	uint32_t id;
	uint8_t type;
	uint8_t key;
	int16_t dx, dy;
};

/* single: a=n b=min_ms c=max_ms (keyboard). burst: a=bursts b=per_burst
 * c=gap_ms d=pause_ms (motion). stream: a=hz b=seconds (motion). */
int traffic_start(enum pattern p, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
void traffic_stop(void);
bool traffic_next(uint8_t pipe, struct report *out);
uint8_t traffic_count(uint8_t pipe); /* reports queued, not yet given to ESB */
/* An empty queue's motion state: the bridge doesn't keep one (bench). */
void report_data(const struct report *r, uint8_t data[11]);
