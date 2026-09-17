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

#include <media-io/video-io.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

/* What the exporter recorded about one saved angle, enough to position inside it. */
struct FileClipInfo {
	std::string path;
	uint64_t ts_origin = 0; /* OBS timestamp of the frame stored as pts = 0 */
	uint64_t ts_last = 0;
};

/* A frame handed to the replay source. `keepalive` owns the decoded picture for as long as it lives. */
struct DecodedFrame {
	std::shared_ptr<void> keepalive;
	const uint8_t *planes[MAX_AV_PLANES] = {};
	uint32_t linesize[MAX_AV_PLANES] = {};
	uint32_t width = 0;
	uint32_t height = 0;
	video_format format = VIDEO_FORMAT_NONE;
	video_colorspace colorspace = VIDEO_CS_DEFAULT;
	video_range_type range = VIDEO_RANGE_DEFAULT;
	uint64_t timestamp = 0; /* mapped back onto the OBS clock */
	int64_t id = 0;         /* stream pts, used to skip re-sending the same picture */
};

/*
 * One saved angle, decoded on its own thread.
 *
 * File I/O and decoding must never happen on the graphics thread — a single page-cache miss inside
 * video_tick shows up as a rendering stall on air — so the thread runs ahead and the graphics
 * thread only takes ready frames out of a queue, never waiting.
 */
class ClipFileSource {
public:
	explicit ClipFileSource(FileClipInfo info);
	~ClipFileSource();

	ClipFileSource(const ClipFileSource &) = delete;
	ClipFileSource &operator=(const ClipFileSource &) = delete;

	/* Opens the file on the decoder thread and positions it at `start_ts`. */
	void start(uint64_t start_ts);
	void stop();

	bool ready() const { return ready_.load(std::memory_order_acquire); }
	bool failed() const { return failed_.load(std::memory_order_acquire); }
	const std::string &error() const { return error_; }

	/* Active angles read ahead further; the others idle with a short queue. */
	void set_active(bool active) { active_.store(active, std::memory_order_release); }

	/* Graphics thread: newest frame at or before `wanted_ts`. Never blocks, may return false. */
	bool pick(uint64_t wanted_ts, DecodedFrame &out);

private:
	void run(uint64_t start_ts);
	size_t queue_limit() const;

	FileClipInfo info_;
	std::string error_;

	std::thread thread_;
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<DecodedFrame> queue_;

	std::atomic<uint64_t> wanted_ts_{0};
	std::atomic<bool> seek_requested_{false};
	std::atomic<bool> active_{false};
	std::atomic<bool> ready_{false};
	std::atomic<bool> failed_{false};
	std::atomic<bool> stop_{false};
};
