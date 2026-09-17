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

#include "angle-capture.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>

constexpr int kCameraCount = 3;
constexpr int kAngleCount = kCameraCount + 1;
constexpr int kProgramAngle = 0; /* cameras are angles 1..3 */

struct CameraBinding {
	bool enabled = false;
	std::string uuid;
	std::string name;
};

struct CameraState {
	bool enabled = false;
	bool bound = false;   /* scene found in the current collection */
	bool running = false; /* tap is delivering frames */
	std::string name;
	std::string error;
};

/*
 * Owns every ring: the programme mix plus up to three camera scenes rendered through private
 * views. Everything here except reader_guard() runs on the Qt thread.
 */
class AngleManager {
public:
	static AngleManager &instance();

	/*
	 * Restarts every ring from the saved settings. All rings get the same duration on purpose: a
	 * camera ring shorter than the programme one cannot serve older clips, which makes its angle
	 * button dead weight.
	 */
	void start_from_settings();

	/* Programme buffer. */
	bool start_program(double duration_sec, uint32_t frame_rate_divisor);

	/* Seconds that fit the memory budget for the current set of rings; <= wanted. */
	double fit_duration(double wanted_sec) const;

	/* Duration the rings were actually started with. */
	double active_duration() const { return program_duration_; }

	/* Cameras: (re)bind a scene and start its tap; stop_camera releases the ring. */
	bool start_camera(int camera, const CameraBinding &binding, uint32_t height, double duration_sec,
			  uint32_t frame_rate_divisor);
	void stop_camera(int camera);
	void stop_cameras();
	void stop_all();

	bool program_running() const { return angles_[kProgramAngle]->running(); }
	AngleCapture &program() { return *angles_[kProgramAngle]; }
	const AngleCapture &program() const { return *angles_[kProgramAngle]; }
	AngleCapture &angle(int index) { return *angles_[static_cast<size_t>(index)]; }
	const AngleCapture &angle(int index) const { return *angles_[static_cast<size_t>(index)]; }
	CameraState camera_state(int camera) const;

	/* Suspends every ring while a replay is on programme (the programme ring would loop back). */
	void set_paused_all(bool paused);

	/* Which ring the replay source reads from; switched live from the panel. */
	int active_angle() const { return active_angle_.load(std::memory_order_acquire); }
	void set_active_angle(int angle);

	/* Graphics thread holds this while reading frames; teardown takes it exclusively. */
	std::shared_lock<std::shared_mutex> reader_guard() { return std::shared_lock<std::shared_mutex>(teardown_); }

	/* Qt timer: rebuilds after video setting changes, notices deleted camera scenes. */
	void poll();

	/* Memory all rings together may take. */
	uint64_t memory_budget() const;

private:
	AngleManager();

	std::shared_mutex teardown_;
	std::array<std::unique_ptr<AngleCapture>, kAngleCount> angles_;
	std::array<CameraBinding, kCameraCount> bindings_;
	std::array<obs_weak_source_t *, kCameraCount> scenes_ = {};
	std::array<std::string, kCameraCount> camera_errors_;
	std::array<uint32_t, kCameraCount> camera_heights_ = {};
	double program_duration_ = 12.0;
	uint32_t program_divisor_ = 1;
	std::atomic<int> active_angle_{kProgramAngle};

	obs_weak_source_t *resolve_scene(const CameraBinding &binding, std::string &resolved_name) const;
	bool start_camera_internal(int camera, const CameraBinding &binding, uint32_t height, double duration_sec,
				   uint32_t frame_rate_divisor);
	uint64_t allocated_bytes() const;
};
