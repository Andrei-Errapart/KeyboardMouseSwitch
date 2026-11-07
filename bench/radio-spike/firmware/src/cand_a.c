/* Candidate A: filled in by Task 6. */
#include "radio.h"
#include "rec.h"

static void start(void)
{
	rec_text("error candidate A is not built yet");
}

static void stop(void)
{
}

static void event(const struct radio_evt *e)
{
	ARG_UNUSED(e);
}

static uint32_t tick(void)
{
	return UINT32_MAX;
}

const struct cand cand_a = {.start = start, .stop = stop, .event = event, .tick = tick};
