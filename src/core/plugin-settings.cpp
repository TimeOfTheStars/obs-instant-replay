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

#include "plugin-settings.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <string>

namespace {

constexpr const char *kFileName = "config.json";

std::string config_file_path()
{
	char *path = obs_module_config_path(kFileName);
	if (!path)
		return {};

	std::string result(path);
	bfree(path);
	return result;
}

} // namespace

PluginSettings &PluginSettings::instance()
{
	static PluginSettings settings;
	return settings;
}

void PluginSettings::load()
{
	const std::string path = config_file_path();
	if (path.empty())
		return;

	obs_data_t *data = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!data) {
		obs_log(LOG_INFO, "no saved settings, using defaults");
		return;
	}

	obs_data_set_default_double(data, "buffer_seconds", buffer_seconds);
	obs_data_set_default_int(data, "frame_rate_divisor", frame_rate_divisor);
	obs_data_set_default_double(data, "clip_length_sec", clip_length_sec);
	obs_data_set_default_double(data, "clip_trim_sec", clip_trim_sec);
	obs_data_set_default_int(data, "speed_percent", speed_percent);
	obs_data_set_default_bool(data, "auto_return", auto_return);
	obs_data_set_default_int(data, "camera_height", camera_height);
	obs_data_set_default_string(data, "scene_collection", scene_collection.c_str());
	obs_data_set_default_bool(data, "export_enabled", export_enabled);
	obs_data_set_default_string(data, "export_dir", export_dir.c_str());
	obs_data_set_default_string(data, "export_encoder", export_encoder.c_str());
	obs_data_set_default_int(data, "export_crf", export_crf);

	buffer_seconds = obs_data_get_double(data, "buffer_seconds");
	frame_rate_divisor = static_cast<uint32_t>(obs_data_get_int(data, "frame_rate_divisor"));
	clip_length_sec = obs_data_get_double(data, "clip_length_sec");
	clip_trim_sec = obs_data_get_double(data, "clip_trim_sec");
	speed_percent = static_cast<int>(obs_data_get_int(data, "speed_percent"));
	auto_return = obs_data_get_bool(data, "auto_return");
	camera_height = static_cast<uint32_t>(obs_data_get_int(data, "camera_height"));
	scene_collection = obs_data_get_string(data, "scene_collection");
	if (obs_data_array_t *list = obs_data_get_array(data, "cameras")) {
		const size_t count = std::min<size_t>(obs_data_array_count(list), cameras.size());
		for (size_t index = 0; index < count; ++index) {
			obs_data_t *item = obs_data_array_item(list, index);
			cameras[index].enabled = obs_data_get_bool(item, "enabled");
			cameras[index].uuid = obs_data_get_string(item, "uuid");
			cameras[index].name = obs_data_get_string(item, "name");
			obs_data_release(item);
		}
		obs_data_array_release(list);
	}
	export_enabled = obs_data_get_bool(data, "export_enabled");
	if (obs_data_array_t *saved_angles = obs_data_get_array(data, "export_angles")) {
		const size_t count = std::min<size_t>(obs_data_array_count(saved_angles), export_angles.size());
		for (size_t index = 0; index < count; ++index) {
			obs_data_t *item = obs_data_array_item(saved_angles, index);
			export_angles[index] = obs_data_get_bool(item, "enabled");
			obs_data_release(item);
		}
		obs_data_array_release(saved_angles);
	} else {
		/* Older config: the single switch meant "save the programme". */
		export_angles = {export_enabled, false, false, false};
	}
	export_dir = obs_data_get_string(data, "export_dir");
	export_encoder = obs_data_get_string(data, "export_encoder");
	export_crf = static_cast<int>(obs_data_get_int(data, "export_crf"));

	obs_data_release(data);
}

void PluginSettings::save() const
{
	char *directory = obs_module_config_path(nullptr);
	if (directory) {
		os_mkdirs(directory);
		bfree(directory);
	}

	const std::string path = config_file_path();
	if (path.empty())
		return;

	obs_data_t *data = obs_data_create();
	obs_data_set_double(data, "buffer_seconds", buffer_seconds);
	obs_data_set_int(data, "frame_rate_divisor", frame_rate_divisor);
	obs_data_set_double(data, "clip_length_sec", clip_length_sec);
	obs_data_set_double(data, "clip_trim_sec", clip_trim_sec);
	obs_data_set_int(data, "speed_percent", speed_percent);
	obs_data_set_bool(data, "auto_return", auto_return);
	obs_data_set_int(data, "camera_height", camera_height);
	obs_data_set_string(data, "scene_collection", scene_collection.c_str());
	obs_data_array_t *list = obs_data_array_create();
	for (const CameraBinding &camera : cameras) {
		obs_data_t *item = obs_data_create();
		obs_data_set_bool(item, "enabled", camera.enabled);
		obs_data_set_string(item, "uuid", camera.uuid.c_str());
		obs_data_set_string(item, "name", camera.name.c_str());
		obs_data_array_push_back(list, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "cameras", list);
	obs_data_array_release(list);
	obs_data_set_bool(data, "export_enabled", export_enabled);
	obs_data_array_t *angle_list = obs_data_array_create();
	for (bool enabled : export_angles) {
		obs_data_t *item = obs_data_create();
		obs_data_set_bool(item, "enabled", enabled);
		obs_data_array_push_back(angle_list, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "export_angles", angle_list);
	obs_data_array_release(angle_list);
	obs_data_set_string(data, "export_dir", export_dir.c_str());
	obs_data_set_string(data, "export_encoder", export_encoder.c_str());
	obs_data_set_int(data, "export_crf", export_crf);

	/* Atomic write with a backup: a half-written config would lose the operator's setup. */
	if (!obs_data_save_json_safe(data, path.c_str(), "tmp", "bak"))
		obs_log(LOG_WARNING, "could not save settings to %s", path.c_str());

	obs_data_release(data);
}
