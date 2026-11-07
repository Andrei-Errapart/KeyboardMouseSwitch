/* A free-running 1 MHz clock, and the time of the latest radio ADDRESS
 * event sent and received, captured in hardware. */
#pragma once
#include <stdint.h>

int stamp_init(void);
uint32_t stamp_now(void);
uint32_t stamp_last_tx_addr(void);
uint32_t stamp_last_rx_addr(void);
void stamp_counts(uint32_t *tx, uint32_t *rx);
uint32_t stamp_lost(void);
