/*
Instant Replay for OBS
Copyright (C) 2026 Trinity AEM

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QMainWindow>

#include "core/plugin-settings.hpp"
#include "core/program-capture.hpp"
#include "core/replay-director.hpp"
#include "core/replay-source.hpp"
#include "export/clip-exporter.hpp"
#include "ui/replay-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/*
 * The dock id doubles as the QWidget object name, which is what OBS uses as the key when it
 * restores the saved dock layout (QMainWindow::restoreState). It must stay stable forever:
 * changing it drops the user's dock position on the next update.
 */
static constexpr const char *kDockId = "trinity.instant-replay.dock";

static ReplayDock *dock_widget = nullptr;

static void start_capture()
{
	const PluginSettings &saved = PluginSettings::instance();

	CaptureSettings settings;
	settings.duration_sec = saved.buffer_seconds;
	settings.frame_rate_divisor = saved.frame_rate_divisor;
	settings.max_bytes = 0; /* derived from free physical memory */

	if (!ProgramCapture::instance().start(settings))
		obs_log(LOG_WARNING, "replay buffer is not running");
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		/* Video output exists only once the frontend has finished starting up. */
		start_capture();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		/* A profile switch can change resolution, frame rate and colour format. */
		ReplayDirector::instance().reset();
		if (ProgramCapture::instance().running()) {
			ProgramCapture::instance().stop();
			start_capture();
		}
		/* Hotkey bindings are stored per profile. */
		if (dock_widget)
			dock_widget->reloadHotkeyBindings();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		/* The replay scene belongs to the collection that is going away. */
		ReplayDirector::instance().reset();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		PluginSettings::instance().save();
		/* The exporter reads the ring: it has to finish before the ring goes away. */
		ClipExporter::instance().shutdown();
		/* Drop the raw callback before libobs starts tearing the video pipeline down. */
		ReplayDirector::instance().reset();
		ProgramCapture::instance().stop();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	/*
	 * Docks have to be registered while modules are being loaded: OBSBasic::OBSInit() calls
	 * restoreState() *after* loadAppModules(), so a dock added later (e.g. on
	 * OBS_FRONTEND_EVENT_FINISHED_LOADING) comes back floating on every start.
	 */
	PluginSettings::instance().load();

	register_replay_source();

	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main_window) {
		obs_log(LOG_WARNING, "no frontend main window, dock not registered");
		return true;
	}

	dock_widget = new ReplayDock(main_window);

	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("Replay.DockTitle"), dock_widget)) {
		obs_log(LOG_ERROR, "failed to register dock '%s' (id already in use?)", kDockId);
		delete dock_widget;
		dock_widget = nullptr;
		return false;
	}

	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	ClipExporter::instance().shutdown();
	ReplayDirector::instance().reset();
	ProgramCapture::instance().stop();

	if (dock_widget) {
		/* OBS owns the dock widget after add_dock_by_id, so only ask it to drop it. */
		obs_frontend_remove_dock(kDockId);
		dock_widget = nullptr;
	}

	obs_log(LOG_INFO, "plugin unloaded");
}
