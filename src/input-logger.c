/*
 * OBS Input Logger - core engine implementation.
 *
 * Ring buffer + writer thread. See input-logger.h for the contract.
 *
 * Portability: uses OBS's os_atomic_* and pthread wrappers (via util/threading.h)
 * so no dependency on C11 <stdatomic.h>, which MSVC only ships experimentally.
 */

#include "input-logger.h"
#include "plugin-support.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <util/base.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define IL_LOCALTIME(tm, t) localtime_s((tm), (t))
#else
#define IL_LOCALTIME(tm, t) localtime_r((t), (tm))
#endif

/* Ring buffer size (power of two). ~256K events; mem cost 256K * 32B = 8 MiB.
 * At a sustained 1 kHz of events the writer thread keeps it nearly empty;
 * this gives ~4 minutes of headroom before overflow even if the writer stalls. */
#define IL_RING_CAPACITY (1u << 18)
#define IL_RING_MASK (IL_RING_CAPACITY - 1u)

typedef struct {
	uint64_t t_us;
	uint16_t kind; /* il_event_kind_t */
	uint16_t flag; /* 0/1 (down/up) */
	int32_t dx;
	int32_t dy;
	const char *name; /* interned, non-owning */
} il_event_t;

/* Held-state tracker for keys and mouse buttons.
 *
 * The OS fires duplicate key-down events while a key is held (keyboard auto-
 * repeat on Windows, kCGKeyboardEventAutorepeat on macOS), and occasionally
 * delivers an "up" we never saw a "down" for (focus churn, modifier keys
 * changing while a different app is foreground). We dedupe here so the log
 * contains exactly one `down` per `up` per physical key press, with no
 * orphaned `up` events.
 *
 * Keys held concurrently is tiny in practice (<10). Linear scan is fine. */
#define IL_HELD_MAX 32
typedef struct {
	const char *name;  /* interned, non-owning */
	uint8_t is_button; /* 1 = mouse button, 0 = keyboard */
} il_held_t;

static struct {
	/* Lifecycle — cross-thread flag via OBS atomic bool helpers. */
	volatile bool active;
	volatile bool writer_should_exit;

	/* Monotonic clock origin (ns). Written once in start(), read in hooks. */
	uint64_t start_ns;

	/* Counters. Protected by push_mtx (cheap — only touched in push + stop). */
	uint64_t total_events;
	uint64_t dropped;
	uint64_t deduped; /* duplicate-down / orphan-up events swallowed */

	/* Ring buffer — producer indices guarded by push_mtx. */
	il_event_t *ring;
	uint32_t head; /* next write slot */
	uint32_t tail; /* next read slot  */
	pthread_mutex_t push_mtx;

	/* Held keys/buttons — also guarded by push_mtx. */
	il_held_t held[IL_HELD_MAX];
	int held_n;

	/* Last absolute mouse position emitted; used to drop redundant samples
	 * from the idle-tick path. Guarded by push_mtx. */
	int32_t last_pos_x;
	int32_t last_pos_y;
	uint8_t last_pos_valid;

	/* Writer thread wake signal (OBS portable event — works on MSVC/mingw/clang). */
	pthread_t writer_thr;
	os_event_t *wake_evt;

	/* Output. */
	char *out_path;
	FILE *fp;
} g_il;

bool input_logger_is_active(void)
{
	return os_atomic_load_bool(&g_il.active);
}

uint64_t input_logger_now_us(void)
{
	uint64_t now_ns = os_gettime_ns();
	uint64_t start_ns = g_il.start_ns;
	if (now_ns <= start_ns)
		return 0;
	return (now_ns - start_ns) / 1000ull;
}

/* --- push path (hot) --- */

/* Must be called with push_mtx held. Returns true if the state-change event
 * (key/button down or up) should be emitted; false if it's a duplicate or
 * orphan that we swallowed. */
static bool il_dedup_locked(const il_event_t *ev)
{
	bool is_button = ev->kind == IL_EVT_MOUSE_BUTTON;
	bool down = ev->flag != 0;

	int idx = -1;
	for (int i = 0; i < g_il.held_n; ++i) {
		if (g_il.held[i].name == ev->name && (uint8_t)is_button == g_il.held[i].is_button) {
			idx = i;
			break;
		}
	}
	if (down) {
		if (idx >= 0) {
			g_il.deduped++;
			return false; /* already held — drop auto-repeat */
		}
		if (g_il.held_n < IL_HELD_MAX) {
			g_il.held[g_il.held_n].name = ev->name;
			g_il.held[g_il.held_n].is_button = (uint8_t)is_button;
			g_il.held_n++;
		}
		return true;
	} else {
		if (idx < 0) {
			g_il.deduped++;
			return false; /* orphan up — drop */
		}
		g_il.held[idx] = g_il.held[--g_il.held_n];
		return true;
	}
}

static inline void il_push(il_event_t ev)
{
	if (!input_logger_is_active())
		return;

	bool wake = false;
	pthread_mutex_lock(&g_il.push_mtx);

	/* Dedup key/button events under the lock so held-state reads and writes
	 * stay consistent with the ring write. */
	if (ev.kind == IL_EVT_KEY || ev.kind == IL_EVT_MOUSE_BUTTON) {
		if (!il_dedup_locked(&ev)) {
			pthread_mutex_unlock(&g_il.push_mtx);
			return;
		}
	}

	uint32_t head = g_il.head;
	uint32_t tail = g_il.tail;
	if ((uint32_t)(head - tail) >= IL_RING_CAPACITY) {
		g_il.dropped++;
	} else {
		g_il.ring[head & IL_RING_MASK] = ev;
		g_il.head = head + 1;
		/* Signal writer only periodically to avoid syscall storm on fast mouse moves. */
		if (((head + 1) & 0x3Fu) == 0)
			wake = true;
	}
	pthread_mutex_unlock(&g_il.push_mtx);

	if (wake)
		os_event_signal(g_il.wake_evt);
}

void input_logger_push_key(uint64_t t_us, const char *vk_name, bool down)
{
	il_event_t ev = {0};
	ev.t_us = t_us;
	ev.kind = IL_EVT_KEY;
	ev.flag = down ? 1 : 0;
	ev.name = vk_name;
	il_push(ev);
}

void input_logger_push_mouse_move(uint64_t t_us, int32_t dx, int32_t dy)
{
	if (dx == 0 && dy == 0)
		return;
	il_event_t ev = {0};
	ev.t_us = t_us;
	ev.kind = IL_EVT_MOUSE_MOVE;
	ev.dx = dx;
	ev.dy = dy;
	il_push(ev);
}

void input_logger_push_mouse_button(uint64_t t_us, const char *btn_name, bool down)
{
	il_event_t ev = {0};
	ev.t_us = t_us;
	ev.kind = IL_EVT_MOUSE_BUTTON;
	ev.flag = down ? 1 : 0;
	ev.name = btn_name;
	il_push(ev);
}

void input_logger_push_mouse_wheel(uint64_t t_us, int32_t dx, int32_t dy)
{
	if (dx == 0 && dy == 0)
		return;
	il_event_t ev = {0};
	ev.t_us = t_us;
	ev.kind = IL_EVT_MOUSE_WHEEL;
	ev.dx = dx;
	ev.dy = dy;
	il_push(ev);
}

void input_logger_push_mouse_pos(uint64_t t_us, int32_t x, int32_t y)
{
	/* Drop redundant samples (same coords as the last one we emitted) so a
	 * mouse held perfectly still during a flurry of non-motion mouse events
	 * doesn't pad the log. `last_pos_valid` starts as 0 each session — see
	 * input_logger_start(). */
	pthread_mutex_lock(&g_il.push_mtx);
	if (g_il.last_pos_valid && g_il.last_pos_x == x && g_il.last_pos_y == y) {
		pthread_mutex_unlock(&g_il.push_mtx);
		return;
	}
	g_il.last_pos_x = x;
	g_il.last_pos_y = y;
	g_il.last_pos_valid = 1;
	pthread_mutex_unlock(&g_il.push_mtx);

	il_event_t ev = {0};
	ev.t_us = t_us;
	ev.kind = IL_EVT_MOUSE_POS;
	ev.dx = x;
	ev.dy = y;
	il_push(ev);
}

/* --- writer thread --- */

static void il_write_event(FILE *fp, const il_event_t *ev)
{
	/* Format matches the sample schema exactly. */
	char line[160];
	int n = 0;
	switch ((il_event_kind_t)ev->kind) {
	case IL_EVT_KEY:
		n = snprintf(line, sizeof(line),
			     "{\"t\": %llu, \"dev\": \"kb\", \"type\": \"key\", \"vk\": \"%s\", \"state\": \"%s\"}\n",
			     (unsigned long long)ev->t_us, ev->name ? ev->name : "?", ev->flag ? "down" : "up");
		break;
	case IL_EVT_MOUSE_MOVE:
		n = snprintf(line, sizeof(line),
			     "{\"t\": %llu, \"dev\": \"mouse\", \"type\": \"move\", \"dx\": %d, \"dy\": %d}\n",
			     (unsigned long long)ev->t_us, (int)ev->dx, (int)ev->dy);
		break;
	case IL_EVT_MOUSE_BUTTON:
		n = snprintf(
			line, sizeof(line),
			"{\"t\": %llu, \"dev\": \"mouse\", \"type\": \"button\", \"vk\": \"%s\", \"state\": \"%s\"}\n",
			(unsigned long long)ev->t_us, ev->name ? ev->name : "?", ev->flag ? "down" : "up");
		break;
	case IL_EVT_MOUSE_WHEEL:
		n = snprintf(line, sizeof(line),
			     "{\"t\": %llu, \"dev\": \"mouse\", \"type\": \"wheel\", \"dx\": %d, \"dy\": %d}\n",
			     (unsigned long long)ev->t_us, (int)ev->dx, (int)ev->dy);
		break;
	case IL_EVT_MOUSE_POS:
		/* Absolute screen-space cursor position. dx/dy slots reused for x/y. */
		n = snprintf(line, sizeof(line),
			     "{\"t\": %llu, \"dev\": \"mouse\", \"type\": \"pos\", \"x\": %d, \"y\": %d}\n",
			     (unsigned long long)ev->t_us, (int)ev->dx, (int)ev->dy);
		break;
	}
	if (n > 0)
		fwrite(line, 1, (size_t)n, fp);
}

static void *il_writer_main(void *arg)
{
	(void)arg;
	os_set_thread_name("input-logger-writer");

	/* Large buffered stream — final flush on stop. */
	setvbuf(g_il.fp, NULL, _IOFBF, 1 << 16);

	/* Drain loop: sleep up to 50ms, then flush whatever arrived. */
	for (;;) {
		(void)os_event_timedwait(g_il.wake_evt, 50);
		os_event_reset(g_il.wake_evt);
		bool should_exit = os_atomic_load_bool(&g_il.writer_should_exit);

		/* Snapshot head, drain up to there in bursts. */
		for (;;) {
			pthread_mutex_lock(&g_il.push_mtx);
			uint32_t head = g_il.head;
			uint32_t tail = g_il.tail;
			pthread_mutex_unlock(&g_il.push_mtx);
			if (head == tail)
				break;

			uint32_t burst = head - tail;
			if (burst > 4096)
				burst = 4096;

			for (uint32_t i = 0; i < burst; ++i) {
				il_event_t ev = g_il.ring[(tail + i) & IL_RING_MASK];
				il_write_event(g_il.fp, &ev);
			}

			pthread_mutex_lock(&g_il.push_mtx);
			g_il.tail = tail + burst;
			g_il.total_events += burst;
			pthread_mutex_unlock(&g_il.push_mtx);
		}
		fflush(g_il.fp);

		if (should_exit)
			break;
	}
	return NULL;
}

/* --- path helpers --- */

static char *il_derive_log_path(const char *video_path)
{
	/* Place the log next to the video with a matching basename + .inputlog.jsonl.
     * If the target already exists for any reason, append .1, .2, ... so we
     * never overwrite prior runs. */
	struct dstr p = {0};
	if (video_path && *video_path) {
		dstr_copy(&p, video_path);
		size_t last_sep = 0;
		for (size_t i = 0; i < p.len; ++i)
			if (p.array[i] == '/' || p.array[i] == '\\')
				last_sep = i;
		size_t last_dot = 0;
		for (size_t i = last_sep; i < p.len; ++i)
			if (p.array[i] == '.')
				last_dot = i;
		if (last_dot > last_sep)
			dstr_resize(&p, last_dot);
		dstr_cat(&p, ".inputlog.jsonl");
	} else {
		char *cfg = obs_module_config_path("logs");
		if (cfg)
			os_mkdirs(cfg);
		time_t now = time(NULL);
		struct tm tmv;
		IL_LOCALTIME(&tmv, &now);
		char stamp[64];
		strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tmv);
		dstr_printf(&p, "%s/input-%s.jsonl", cfg ? cfg : ".", stamp);
		bfree(cfg);
	}

	/* Uniquify if needed — never overwrite a prior log. */
	if (os_file_exists(p.array)) {
		struct dstr base = {0};
		dstr_copy_dstr(&base, &p);
		for (int i = 1; i < 10000; ++i) {
			dstr_printf(&p, "%s.%d", base.array, i);
			if (!os_file_exists(p.array))
				break;
		}
		dstr_free(&base);
	}

	return p.array; /* caller takes ownership of the bmalloc'd buffer */
}

/* --- lifecycle --- */

bool input_logger_module_load(void)
{
	memset(&g_il, 0, sizeof(g_il));
	g_il.ring = bzalloc(sizeof(il_event_t) * IL_RING_CAPACITY);
	pthread_mutex_init(&g_il.push_mtx, NULL);
	if (os_event_init(&g_il.wake_evt, OS_EVENT_TYPE_MANUAL) != 0) {
		bfree(g_il.ring);
		g_il.ring = NULL;
		return false;
	}
	return g_il.ring != NULL;
}

void input_logger_module_unload(void)
{
	input_logger_stop();
	bfree(g_il.ring);
	g_il.ring = NULL;
	pthread_mutex_destroy(&g_il.push_mtx);
	if (g_il.wake_evt) {
		os_event_destroy(g_il.wake_evt);
		g_il.wake_evt = NULL;
	}
}

void input_logger_start(const char *target_video_path)
{
	if (input_logger_is_active()) {
		obs_log(LOG_WARNING, "input_logger_start called while already active");
		return;
	}

	g_il.out_path = il_derive_log_path(target_video_path);
	g_il.fp = os_fopen(g_il.out_path, "wb");
	if (!g_il.fp) {
		obs_log(LOG_ERROR, "Could not open log file: %s", g_il.out_path);
		bfree(g_il.out_path);
		g_il.out_path = NULL;
		return;
	}

	/* Header line so readers can identify the producing build. Schema-compatible
	 * with the sample format (just an extra "meta" key unknown readers ignore). */
	fprintf(g_il.fp,
		"{\"meta\": \"obs-input-logger\", \"version\": \"%s\", \"platform\": \"%s\", \"mouse_source\": \"%s\"}\n",
		PLUGIN_VERSION,
#ifdef _WIN32
		"windows",
#elif defined(__APPLE__)
		"macos",
#else
		"linux",
#endif
#ifdef _WIN32
		"rawinput"
#elif defined(__APPLE__)
		"cgeventtap"
#else
		"none"
#endif
	);

	/* Reset ring + counters + held-state tracker. */
	pthread_mutex_lock(&g_il.push_mtx);
	g_il.head = 0;
	g_il.tail = 0;
	g_il.total_events = 0;
	g_il.dropped = 0;
	g_il.deduped = 0;
	g_il.held_n = 0;
	g_il.last_pos_valid = 0;
	g_il.last_pos_x = 0;
	g_il.last_pos_y = 0;
	pthread_mutex_unlock(&g_il.push_mtx);

	os_atomic_store_bool(&g_il.writer_should_exit, false);
	g_il.start_ns = os_gettime_ns();
	os_atomic_store_bool(&g_il.active, true);
	os_event_reset(g_il.wake_evt);

	pthread_create(&g_il.writer_thr, NULL, il_writer_main, NULL);

	if (!input_logger_hooks_start()) {
		obs_log(LOG_WARNING, "Platform input hooks failed to start — log will be empty");
	}

	obs_log(LOG_INFO, "Input logger started -> %s", g_il.out_path);
}

void input_logger_stop(void)
{
	if (!os_atomic_set_bool(&g_il.active, false))
		return; /* was already inactive */

	input_logger_hooks_stop();

	/* Signal writer to drain & exit. */
	os_atomic_store_bool(&g_il.writer_should_exit, true);
	os_event_signal(g_il.wake_evt);
	pthread_join(g_il.writer_thr, NULL);

	if (g_il.fp) {
		fflush(g_il.fp);
		fclose(g_il.fp);
		g_il.fp = NULL;
	}

	pthread_mutex_lock(&g_il.push_mtx);
	uint64_t total = g_il.total_events;
	uint64_t dropped = g_il.dropped;
	uint64_t deduped = g_il.deduped;
	pthread_mutex_unlock(&g_il.push_mtx);

	obs_log(LOG_INFO, "Input logger stopped: %llu events, %llu dropped, %llu deduped -> %s",
		(unsigned long long)total, (unsigned long long)dropped, (unsigned long long)deduped,
		g_il.out_path ? g_il.out_path : "(no file)");

	bfree(g_il.out_path);
	g_il.out_path = NULL;
}
