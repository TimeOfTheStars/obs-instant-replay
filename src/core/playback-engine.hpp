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

#include "frame-ring.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

/*
 * An immutable snapshot of the part of the ring an event refers to. Frames are addressed by the
 * monotonic sequence number, positions inside the clip by timestamp.
 */
struct Clip {
	uint64_t seq_in = 0;
	uint64_t seq_out = 0;
	uint64_t ts_in = 0;
	uint64_t ts_out = 0;

	bool valid() const { return seq_out > seq_in && ts_out > ts_in; }
	double duration_sec() const { return valid() ? static_cast<double>(ts_out - ts_in) / 1000000000.0 : 0.0; }
};

/* What the replay source should put on screen this tick. */
struct FramePick {
	bool has_frame = false;
	bool finished = false; /* playback reached the out point */
	FrameMeta meta;
	video_format format = VIDEO_FORMAT_NONE;
	video_colorspace colorspace = VIDEO_CS_DEFAULT;
	video_range_type range = VIDEO_RANGE_DEFAULT;
	const uint8_t *planes[MAX_AV_PLANES] = {};

	/*
	 * Set when the pixels came from a saved file: owns the decoded picture until the tick ends.
	 * Ring frames leave it empty — those are covered by the source's reader_guard().
	 */
	std::shared_ptr<void> keepalive;
};

/*
 * Drives playback position. The clock is OBS' video clock: the playback position advances by
 * (elapsed wall time x speed), so changing speed mid-playback only re-anchors and never jumps.
 */
class PlaybackEngine {
public:
	static PlaybackEngine &instance();

	/* Builds a clip ending `trim_sec` before the live edge. UI thread. */
	bool mark(double length_sec, double trim_sec, Clip &clip, std::string &error) const;

	/* Rebuilds a clip from edited in/out timestamps (timeline dragging). UI thread. */
	bool clip_from_timestamps(uint64_t ts_in, uint64_t ts_out, Clip &clip, std::string &error) const;

	/* Timestamp of the newest buffered frame, i.e. the live edge. Returns false if empty. */
	static bool live_timestamp(uint64_t &timestamp);

	bool play(const Clip &clip, double speed);
	void stop();
	void set_speed(double speed);

	/*
	 * Switches the ring frames are read from. Position is a time offset, and every ring shares
	 * the OBS clock, so no re-anchoring is needed — the next tick simply samples another angle.
	 */
	void set_angle(int angle);
	int angle() const;

	bool playing() const;
	double speed() const;
	Clip clip() const;
	double position_sec() const;

	/* Graphics thread: advances the position and picks the frame for this tick. */
	FramePick next_frame();

private:
	PlaybackEngine() = default;

	mutable std::mutex mutex_;
	Clip clip_;
	double speed_ = 1.0;
	uint64_t anchor_wall_ns_ = 0;
	uint64_t anchor_src_ns_ = 0;
	uint64_t last_emitted_seq_ = 0;
	int last_emitted_angle_ = 0;
	bool last_emitted_from_file_ = false;
	video_colorspace ring_colorspace_ = VIDEO_CS_DEFAULT;
	video_range_type ring_range_ = VIDEO_RANGE_DEFAULT;
	bool has_emitted_ = false;
	bool playing_ = false;
};
