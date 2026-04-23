/*
 * OBS Input Logger — entry point.
 *
 * Registers the module and wires OBS frontend recording events to the logger.
 */

#include "input-logger.h"
#include "plugin-support.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static bool g_enabled = true;

static const char *il_current_recording_path(void)
{
	/* obs_frontend_get_last_recording() is only valid *after* stop, but most
     * OBS versions expose the active filename via the output. Fall back to
     * NULL so the logger auto-times a filename.
     *
     * We look up the "adv_file_output" / "simple_file_output" / generic
     * recording output by walking the record output. */
	obs_output_t *out = obs_frontend_get_recording_output();
	if (!out)
		return NULL;
	obs_data_t *settings = obs_output_get_settings(out);
	const char *path = NULL;
	if (settings) {
		path = obs_data_get_string(settings, "path");
		if (!path || !*path)
			path = obs_data_get_string(settings, "url");
	}
	/* Return a bstrdup so caller owns it; settings/output released here. */
	char *copy = (path && *path) ? bstrdup(path) : NULL;
	if (settings)
		obs_data_release(settings);
	obs_output_release(out);
	return copy;
}

static void il_frontend_event(enum obs_frontend_event event, void *data)
{
	(void)data;
	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED: {
		if (!g_enabled)
			break;
		char *path = (char *)il_current_recording_path();
		input_logger_start(path);
		bfree(path);
		break;
	}
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		input_logger_stop();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		input_logger_stop();
		break;
	default:
		break;
	}
}

/* --- menu toggle (Tools → Input Logger: Enabled) --- */

static void il_menu_toggled(void *priv)
{
	(void)priv;
	g_enabled = !g_enabled;

	/* Persist preference in module config. */
	config_t *cfg = obs_frontend_get_user_config();
	if (cfg) {
		config_set_bool(cfg, "InputLogger", "Enabled", g_enabled);
		config_save(cfg);
	}
	obs_log(LOG_INFO, "Input logger %s", g_enabled ? "enabled" : "disabled");
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "Input Logger v%s loading", PLUGIN_VERSION);
	if (!input_logger_module_load()) {
		obs_log(LOG_ERROR, "Failed to allocate input logger state");
		return false;
	}

	config_t *cfg = obs_frontend_get_user_config();
	if (cfg) {
		/* Default true if never set */
		config_set_default_bool(cfg, "InputLogger", "Enabled", true);
		g_enabled = config_get_bool(cfg, "InputLogger", "Enabled");
	}

	obs_frontend_add_event_callback(il_frontend_event, NULL);
	obs_frontend_add_tools_menu_item(obs_module_text("InputLogger.Menu.Toggle"), il_menu_toggled, NULL);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(il_frontend_event, NULL);
	input_logger_module_unload();
	obs_log(LOG_INFO, "Input Logger unloaded");
}
