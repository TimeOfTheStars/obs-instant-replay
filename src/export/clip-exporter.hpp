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

#include "core/playback-engine.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

enum class ExportState { Queued, Encoding, Done, DoneTruncated, Failed };

struct ExportStatus {
	ExportState state = ExportState::Queued;
	uint64_t frames_done = 0;
	uint64_t frames_total = 0;
	std::string path;
	std::string error;
};

struct ExportOptions {
	std::string encoder = "auto"; /* auto | x264 | nvenc */
	int crf = 18;
	std::string base_dir; /* empty = OBS recording folder */
};

/*
 * Encodes marked clips straight out of the ring into MP4 files on a low-priority worker thread.
 *
 * The ring keeps moving underneath: a clip nearly as long as the buffer has its first frames
 * overwritten within tens of milliseconds, so every frame is re-validated after it is copied and
 * a clip that loses frames is written truncated rather than corrupted.
 */
class ClipExporter {
public:
	static ClipExporter &instance();

	/* Qt thread. Returns a job id for status polling. */
	int enqueue(const Clip &clip, const std::string &event_name, const ExportOptions &options);

	ExportStatus status(int job_id) const;

	/* Finishes the current file early, drops the queue and joins the worker. Qt thread. */
	void shutdown();

private:
	ClipExporter() = default;
	~ClipExporter();

	struct Job {
		int id = 0;
		Clip clip;
		std::string event_name;
		std::string encoder; /* resolved: libx264 | h264_nvenc */
		int crf = 18;
		std::string base_dir;
	};

	void ensure_worker();
	void worker_loop();
	void run(const Job &job);
	void update(int job_id, ExportState state, uint64_t done, uint64_t total, const std::string &path,
		    const std::string &error);

	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<Job> queue_;
	std::map<int, ExportStatus> statuses_;
	std::thread worker_;
	std::atomic<bool> stop_{false};
	int next_id_ = 1;
	int sequence_ = 0;
};
