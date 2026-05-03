/*
 * OBS Input Logger - core engine
 *
 * Public API used by plugin-main.c and platform hook modules.
 *
 * Concurrency model:
 *  - Hook threads (macOS CFRunLoop / Windows message loop) call
 *    input_logger_push_key() and input_logger_push_mouse_move() with
 *    zero allocations and only a single atomic check + mutex'd enqueue.
 *  - A dedicated writer thread drains the ring buffer and does all JSON
 *    formatting + buffered I/O, so no syscalls happen on the input path.
 *  - OBS frontend thread calls input_logger_start() / _stop().
 *
 * The ring buffer is bounded; on overflow events are dropped and counted
 * (dropped_count) rather than blocking the hook.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event kinds pushed by platform hooks. */
typedef enum {
	IL_EVT_KEY = 1,
	IL_EVT_MOUSE_MOVE = 2,
	IL_EVT_MOUSE_BUTTON = 3,
	IL_EVT_MOUSE_WHEEL = 4,
	IL_EVT_MOUSE_POS = 5,
} il_event_kind_t;

/* Called once when the module loads. Returns false on unrecoverable error. */
bool input_logger_module_load(void);

/* Called once when the module unloads. Safe to call even if never started. */
void input_logger_module_unload(void);

/* Begin/end a logging session bound to the given target video path.
 * target_video_path may be NULL; if provided, the log will be placed
 * alongside it as <basename>.inputlog.jsonl. If NULL, a timestamped file
 * is created in OBS's module config dir.
 *
 * Both are safe to call from the OBS frontend thread. Each call produces
 * a NEW, uniquely-named log file — re-running never overwrites.
 */
void input_logger_start(const char *target_video_path);
void input_logger_stop(void);

/* Thread-safe: cheap atomic check used by hook threads to short-circuit. */
bool input_logger_is_active(void);

/* Monotonic microseconds since the current session started. */
uint64_t input_logger_now_us(void);

/* Enqueue events. Non-blocking; on buffer overflow they are counted as dropped.
 * vk_name is a short interned ASCII string ("w", "space", "shift", "mouse_left",
 * ...). It must remain valid forever (use string literals / the built-in table).
 */
void input_logger_push_key(uint64_t t_us, const char *vk_name, bool down);
void input_logger_push_mouse_move(uint64_t t_us, int32_t dx, int32_t dy);
void input_logger_push_mouse_button(uint64_t t_us, const char *btn_name, bool down);
void input_logger_push_mouse_wheel(uint64_t t_us, int32_t dx, int32_t dy);
/* Absolute cursor position in virtual-desktop pixel coordinates. Emitted
 * alongside every mouse event (move/click/scroll) so the log always carries
 * a current cursor location. Coordinate space is whatever the OS reports for
 * the global cursor (Windows: virtual-screen px;
 * macOS: global display points with origin at upper-left of the main display).
 */
void input_logger_push_mouse_pos(uint64_t t_us, int32_t x, int32_t y);

/* Platform hook lifecycle — implemented in hooks-<platform>.{c,m}. */
bool input_logger_hooks_start(void);
void input_logger_hooks_stop(void);

#ifdef __cplusplus
}
#endif
