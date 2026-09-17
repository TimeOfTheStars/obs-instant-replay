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

#pragma once

#include <cstdint>

/*
 * Plugin-wide settings, stored as JSON under the module config path. The frontend config is not
 * used on purpose: obs_frontend_get_global_config is deprecated and obs_frontend_get_user_config
 * does not exist in older OBS, while a plain JSON file behaves the same everywhere.
 */
struct PluginSettings {
	/* Capture */
	/* Two seconds longer than the default clip: the export needs headroom before the ring overwrites it. */
	double buffer_seconds = 12.0;
	uint32_t frame_rate_divisor = 1;

	/* Panel */
	double clip_length_sec = 10.0;
	double clip_trim_sec = 0.0;
	int speed_percent = 100;
	bool auto_return = true;

	static PluginSettings &instance();

	void load();
	void save() const;
};
