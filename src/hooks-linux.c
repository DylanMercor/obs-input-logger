/*
 * Linux input hooks — stub.
 *
 * There's no display-server-agnostic, non-root way to capture global input on
 * Linux (Xlib XInput2 works only under X11; Wayland exposes no global listener
 * without a portal). Rather than ship a broken implementation, this stub logs
 * a warning; the rest of the plugin still builds and runs (no events emitted).
 *
 * A future X11-only implementation could use XInput2 RawMotion/RawKey events
 * on a dedicated thread; a Wayland implementation would need `libei` or a
 * desktop-portal. PRs welcome.
 */

#include "input-logger.h"
#include "plugin-support.h"
#include <obs-module.h>

bool input_logger_hooks_start(void)
{
	obs_log(LOG_WARNING, "Input Logger: Linux input capture is not implemented; log will contain no events.");
	return false;
}

void input_logger_hooks_stop(void) {}
