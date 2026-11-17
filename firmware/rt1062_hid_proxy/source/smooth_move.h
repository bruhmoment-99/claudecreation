/*
 * smooth_move.h - Renders cmd.move(dx,dy) as an eased motion over time
 * instead of a single jump.
 *
 * Each move becomes a "segment" in a small FIFO. A periodic tick (PIT timer
 * ISR, APP_SMOOTH_TICK_HZ) walks the active segment along a precomputed
 * ease-in/ease-out curve and feeds per-tick deltas into hid_merge, which
 * merges them with the physical mouse exactly like any other injection.
 *
 * Properties
 *   - Exact: the per-tick deltas of a segment always sum to (dx, dy). The
 *     position is recomputed from the curve each tick (not accumulated), so
 *     rounding never drifts.
 *   - Deterministic: no randomness. The same command always produces the
 *     same report sequence.
 *   - Fixed point only: Q15 lookup table, one 64-bit multiply per axis and
 *     one hardware divide per tick. No floating point and no libm.
 *   - Default OFF: with smoothing disabled cmd.move goes straight to
 *     hid_merge, as before.
 */
#ifndef SMOOTH_MOVE_H_
#define SMOOTH_MOVE_H_

#include <stdbool.h>
#include <stdint.h>

void smooth_move_init(void);

/* 0 = off (moves apply instantly), otherwise the duration of each move in ms
 * (1..APP_SMOOTH_MAX_MS). Turning it off flushes any motion still in flight
 * straight to hid_merge, so nothing is lost. Task context. */
void smooth_move_set_duration(uint32_t ms);
uint32_t smooth_move_get_duration(void);

/* Called by the cmd.move handler (task context). */
void smooth_move_start(int32_t dx, int32_t dy);

/* Advance one tick: computes this tick's (dx, dy), passes it to hid_merge if
 * non-zero, and returns it through the out pointers (either may be NULL).
 * Returns (0, 0) when idle, so physical passthrough is untouched. Call ONLY
 * from the tick context (timer ISR or one high-priority task), never from two
 * contexts at once. */
void smooth_move_tick(int32_t *outDx, int32_t *outDy);

/* cmd.stop(): drop the move in progress and everything queued. The cursor
 * stays where the eased motion has brought it so far. Task context. */
void smooth_move_cancel(void);

/* True while segments are queued or in progress. */
bool smooth_move_busy(void);

/* Firmware only: starts/stops the PIT tick source (no-op in unit tests). */
void smooth_move_hw_init(void);

#endif /* SMOOTH_MOVE_H_ */
