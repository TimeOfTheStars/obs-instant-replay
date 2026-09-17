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

#include "angle-manager.hpp"

#include "main-mix-tap.hpp"
#include "memory-calculator.hpp"
#include "playback-engine.hpp"
#include "plugin-settings.hpp"
#include "view-tap.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/bmem.h>

#include <algorithm>

AngleManager &AngleManager::instance()
{
	static AngleManager manager;
	return manager;
}

AngleManager::AngleManager()
{
	for (auto &angle : angles_)
		angle = std::make_unique<AngleCapture>(teardown_);
}

uint64_t AngleManager::allocated_bytes() const
{
	uint64_t total = 0;
	for (const auto &angle : angles_)
		total += angle->running() ? angle->ring().bytes_allocated() : 0;
	return total;
}

uint64_t AngleManager::memory_budget() const
{
	const uint64_t available = available_physical_memory();
	if (available == 0)
		return 2048ull * 1024ull * 1024ull; /* unknown: stay conservative at 2 GiB */

	/*
	 * Our own rings are already subtracted from "available", so they have to be added back —.
	 * otherwise every restart measures a machine that looks emptier than it is and the budget
	 * spirals down until the cameras get crumbs.
	 */
	return (available + allocated_bytes()) / 2;
}

double AngleManager::fit_duration(double wanted_sec) const
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || ovi.fps_den == 0)
		return wanted_sec;

	const PluginSettings &settings = PluginSettings::instance();
	const double fps = static_cast<double>(ovi.fps_num) / ovi.fps_den;
	const uint32_t divisor = std::max<uint32_t>(1, settings.frame_rate_divisor);

	uint64_t per_second = ring_bytes(ovi.output_width, ovi.output_height, ovi.output_format, fps, divisor, 1.0);

	const uint32_t camera_height = std::min(settings.camera_height, ovi.base_height);
	const uint32_t camera_width = camera_width_for_height(ovi.base_width, ovi.base_height, camera_height);
	for (const CameraBinding &binding : settings.cameras) {
		if (binding.enabled)
			per_second += ring_bytes(camera_width, camera_height, ovi.output_format, fps, divisor, 1.0);
	}

	if (per_second == 0)
		return wanted_sec;

	const double affordable = static_cast<double>(memory_budget()) / per_second;
	/* Below three seconds a replay is not worth showing; let the allocation fail loudly instead. */
	return std::max(3.0, std::min(wanted_sec, affordable));
}

void AngleManager::start_from_settings()
{
	PluginSettings &settings = PluginSettings::instance();

	PlaybackEngine::instance().stop();
	stop_all();

	const double duration = fit_duration(settings.buffer_seconds);
	if (duration + 0.05 < settings.buffer_seconds)
		obs_log(LOG_WARNING, "buffers trimmed to %.1f s (asked for %.1f s): %llu MiB budget for all rings",
			duration, settings.buffer_seconds,
			static_cast<unsigned long long>(memory_budget() / (1024ull * 1024ull)));

	if (!start_program(duration, settings.frame_rate_divisor))
		obs_log(LOG_WARNING, "replay buffer is not running");

	/* Scene uuids only mean something inside the collection they were picked in. */
	std::string collection;
	if (char *name = obs_frontend_get_current_scene_collection()) {
		collection = name;
		bfree(name);
	}
	const bool same_collection = settings.scene_collection.empty() || settings.scene_collection == collection;

	for (int camera = 0; camera < kCameraCount; ++camera) {
		CameraBinding binding = settings.cameras[static_cast<size_t>(camera)];
		if (!same_collection)
			binding.uuid.clear(); /* fall back to the scene name in a different collection */
		start_camera_internal(camera, binding, settings.camera_height, duration, settings.frame_rate_divisor);
	}
}

bool AngleManager::start_program(double duration_sec, uint32_t frame_rate_divisor)
{
	program_duration_ = duration_sec;
	program_divisor_ = std::max<uint32_t>(1, frame_rate_divisor);

	TapRequest request;
	request.frame_rate_divisor = program_divisor_;

	AngleCapture &capture = program();
	if (!capture.start(std::make_unique<MainMixTap>(), request, duration_sec, memory_budget())) {
		obs_log(LOG_ERROR, "programme capture: %s", capture.error().c_str());
		return false;
	}

	const CaptureStatus status = capture.status();
	obs_log(LOG_INFO, "programme capture started: %ux%u, %.2f fps, %.1f s, %.2f GiB", status.width, status.height,
		status.fps, status.capacity_sec, static_cast<double>(status.bytes) / (1024.0 * 1024.0 * 1024.0));
	if (status.capacity_sec + 0.05 < duration_sec)
		obs_log(LOG_WARNING, "programme buffer trimmed to %.1f s (asked for %.1f s) by the memory budget",
			status.capacity_sec, duration_sec);
	return true;
}

obs_weak_source_t *AngleManager::resolve_scene(const CameraBinding &binding, std::string &resolved_name) const
{
	obs_source_t *scene = nullptr;

	/* uuid survives renames; the name is the fallback for a scene re-created by hand. */
	if (!binding.uuid.empty())
		scene = obs_get_source_by_uuid(binding.uuid.c_str());
	if (!scene && !binding.name.empty())
		scene = obs_get_source_by_name(binding.name.c_str());

	if (!scene)
		return nullptr;

	if (!obs_source_is_scene(scene)) {
		obs_source_release(scene);
		return nullptr;
	}

	resolved_name = obs_source_get_name(scene);
	obs_weak_source_t *weak = obs_source_get_weak_source(scene);
	obs_source_release(scene);
	return weak;
}

bool AngleManager::start_camera(int camera, const CameraBinding &binding, uint32_t height, double duration_sec,
				uint32_t frame_rate_divisor)
{
	return start_camera_internal(camera, binding, height, duration_sec, frame_rate_divisor);
}

bool AngleManager::start_camera_internal(int camera, const CameraBinding &binding, uint32_t height, double duration_sec,
					 uint32_t frame_rate_divisor)
{
	if (camera < 0 || camera >= kCameraCount)
		return false;

	stop_camera(camera);
	bindings_[static_cast<size_t>(camera)] = binding;
	camera_heights_[static_cast<size_t>(camera)] = height;
	camera_errors_[static_cast<size_t>(camera)].clear();

	if (!binding.enabled)
		return true;

	std::string resolved_name;
	obs_weak_source_t *scene = resolve_scene(binding, resolved_name);
	if (!scene) {
		camera_errors_[static_cast<size_t>(camera)] = "scene not found";
		obs_log(LOG_WARNING, "camera %d: scene '%s' not found", camera + 1, binding.name.c_str());
		return false;
	}
	scenes_[static_cast<size_t>(camera)] = scene;
	bindings_[static_cast<size_t>(camera)].name = resolved_name;

	obs_video_info ovi = {};
	obs_get_video_info(&ovi);

	TapRequest request;
	request.scene = scene;
	request.frame_rate_divisor = std::max<uint32_t>(1, frame_rate_divisor);
	if (height > 0 && height < ovi.base_height) {
		request.output_height = height;
		request.output_width = camera_width_for_height(ovi.base_width, ovi.base_height, height);
	}

	/*
	 * The duration was already fitted to the budget for the whole set of rings, so the cap here is
	 * just this ring's own requirement — it must not trim the camera below the programme.
	 */
	const uint32_t requested_height = request.output_height ? request.output_height : ovi.output_height;
	const uint32_t requested_width = request.output_width ? request.output_width : ovi.output_width;
	const double fps = ovi.fps_den ? static_cast<double>(ovi.fps_num) / ovi.fps_den : 0.0;
	const uint64_t cap = ring_bytes(requested_width, requested_height, ovi.output_format, fps,
					std::max<uint32_t>(1, frame_rate_divisor), duration_sec);

	AngleCapture &capture = angle(camera + 1);
	if (!capture.start(std::make_unique<ViewTap>(), request, duration_sec, cap)) {
		camera_errors_[static_cast<size_t>(camera)] = capture.error();
		obs_log(LOG_ERROR, "camera %d ('%s'): %s", camera + 1, resolved_name.c_str(), capture.error().c_str());
		return false;
	}

	const CaptureStatus status = capture.status();
	obs_log(LOG_INFO, "camera %d ('%s') started: %ux%u, %.2f fps, %.1f s, %.2f GiB", camera + 1,
		resolved_name.c_str(), status.width, status.height, status.fps, status.capacity_sec,
		static_cast<double>(status.bytes) / (1024.0 * 1024.0 * 1024.0));
	return true;
}

void AngleManager::stop_camera(int camera)
{
	if (camera < 0 || camera >= kCameraCount)
		return;

	if (active_angle() == camera + 1)
		set_active_angle(kProgramAngle);

	angle(camera + 1).stop();

	obs_weak_source_t *&scene = scenes_[static_cast<size_t>(camera)];
	if (scene) {
		obs_weak_source_release(scene);
		scene = nullptr;
	}
}

void AngleManager::stop_cameras()
{
	for (int camera = 0; camera < kCameraCount; ++camera)
		stop_camera(camera);
}

void AngleManager::stop_all()
{
	PlaybackEngine::instance().stop();
	stop_cameras();
	program().stop();
}

CameraState AngleManager::camera_state(int camera) const
{
	CameraState state;
	if (camera < 0 || camera >= kCameraCount)
		return state;

	const CameraBinding &binding = bindings_[static_cast<size_t>(camera)];
	state.enabled = binding.enabled;
	state.name = binding.name;
	state.bound = scenes_[static_cast<size_t>(camera)] != nullptr;
	state.running = angle(camera + 1).running();
	state.error = camera_errors_[static_cast<size_t>(camera)];
	return state;
}

void AngleManager::set_paused_all(bool paused)
{
	for (auto &angle : angles_)
		angle->set_paused(paused);
}

void AngleManager::set_active_angle(int angle_index)
{
	if (angle_index < 0 || angle_index >= kAngleCount)
		angle_index = kProgramAngle;
	active_angle_.store(angle_index, std::memory_order_release);
}

void AngleManager::poll()
{
	/* Applying new video settings raises no frontend event, so the geometry is polled. */
	if (program().running()) {
		obs_video_info ovi = {};
		if (obs_get_video_info(&ovi)) {
			const RingConfig &config = program().ring().config();
			if (ovi.output_width != config.width || ovi.output_height != config.height ||
			    ovi.output_format != config.format) {
				obs_log(LOG_INFO, "video settings changed (%ux%u -> %ux%u), restarting capture",
					config.width, config.height, ovi.output_width, ovi.output_height);

				/* Playback and the camera views both depend on the old geometry. */
				PlaybackEngine::instance().stop();
				const auto bindings = bindings_;
				const auto heights = camera_heights_;
				stop_all();
				start_program(program_duration_, program_divisor_);
				for (int camera = 0; camera < kCameraCount; ++camera)
					start_camera(camera, bindings[static_cast<size_t>(camera)],
						     heights[static_cast<size_t>(camera)], program_duration_,
						     program_divisor_);
				return;
			}
		}
	}

	/* A camera scene deleted from the collection leaves its view rendering black. */
	for (int camera = 0; camera < kCameraCount; ++camera) {
		obs_weak_source_t *scene = scenes_[static_cast<size_t>(camera)];
		if (!scene || !angle(camera + 1).running())
			continue;

		obs_source_t *source = obs_weak_source_get_source(scene);
		const bool gone = !source || obs_source_removed(source);
		if (source)
			obs_source_release(source);

		if (gone) {
			obs_log(LOG_WARNING, "camera %d: scene was removed, stopping its buffer", camera + 1);
			stop_camera(camera);
			camera_errors_[static_cast<size_t>(camera)] = "scene was removed";
		}
	}
}
