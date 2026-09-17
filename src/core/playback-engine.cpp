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

#include "playback-engine.hpp"
#include "angle-manager.hpp"

#include <obs.h>

#include <algorithm>

namespace {

/*
 * Frames reach the raw callback a frame or two after they were rendered, so the newest sequence in
 * the ring is not yet what the viewer sees. Backing off keeps the tail of the clip clean.
 */
constexpr uint64_t kLiveEdgeFrames = 2;

double clamp_speed(double speed)
{
	return std::clamp(speed, 0.05, 4.0);
}

} // namespace

PlaybackEngine &PlaybackEngine::instance()
{
	static PlaybackEngine engine;
	return engine;
}

bool PlaybackEngine::mark(double length_sec, double trim_sec, Clip &clip, std::string &error) const
{
	const AngleCapture &capture = AngleManager::instance().program();
	if (!capture.running()) {
		error = "buffer is not running";
		return false;
	}

	const FrameRing &ring = capture.ring();
	const uint64_t head = ring.head();
	if (head <= kLiveEdgeFrames) {
		error = "buffer is still filling";
		return false;
	}

	/* Never reach past a gap: those frames belong to a different, older recording run. */
	const uint64_t oldest = std::max(ring.oldest(), ring.gap_seq());

	const double fps = ring.config().fps;
	if (fps <= 0.0) {
		error = "buffer has no frame rate";
		return false;
	}

	const uint64_t trim_frames = static_cast<uint64_t>(std::max(0.0, trim_sec) * fps);
	const uint64_t length_frames = static_cast<uint64_t>(std::max(0.1, length_sec) * fps);

	uint64_t seq_out = head - kLiveEdgeFrames;
	if (seq_out <= oldest + 1) {
		error = "buffer is still filling";
		return false;
	}
	if (trim_frames >= seq_out - oldest) {
		error = "trim is longer than the buffer";
		return false;
	}
	seq_out -= trim_frames;

	uint64_t seq_in = seq_out > length_frames ? seq_out - length_frames : 0;
	seq_in = std::max(seq_in, oldest);
	if (seq_out <= seq_in + 1) {
		error = "not enough footage buffered yet";
		return false;
	}

	Clip built;
	built.seq_in = seq_in;
	built.seq_out = seq_out;
	if (!ring.timestamp_at(seq_in, built.ts_in) || !ring.timestamp_at(seq_out, built.ts_out)) {
		error = "footage was overwritten while marking";
		return false;
	}

	if (!built.valid()) {
		error = "marked clip is empty";
		return false;
	}

	clip = built;
	return true;
}

bool PlaybackEngine::clip_from_timestamps(uint64_t ts_in, uint64_t ts_out, Clip &clip, std::string &error) const
{
	const AngleCapture &capture = AngleManager::instance().program();
	if (!capture.running()) {
		error = "buffer is not running";
		return false;
	}

	if (ts_out <= ts_in) {
		error = "out point is before the in point";
		return false;
	}

	const FrameRing &ring = capture.ring();
	const uint64_t head = ring.head();
	if (head == 0) {
		error = "buffer is empty";
		return false;
	}

	const uint64_t oldest = std::max(ring.oldest(), ring.gap_seq());

	Clip built;
	if (!ring.find_by_timestamp(ts_in, oldest, head - 1, built.seq_in) ||
	    !ring.find_by_timestamp(ts_out, oldest, head - 1, built.seq_out)) {
		error = "requested range is no longer buffered";
		return false;
	}

	if (built.seq_out <= built.seq_in + 1) {
		error = "range is too short";
		return false;
	}

	if (!ring.timestamp_at(built.seq_in, built.ts_in) || !ring.timestamp_at(built.seq_out, built.ts_out)) {
		error = "footage was overwritten while trimming";
		return false;
	}

	clip = built;
	return true;
}

bool PlaybackEngine::live_timestamp(uint64_t &timestamp)
{
	return AngleManager::instance().program().live_timestamp(timestamp);
}

bool PlaybackEngine::play(const Clip &clip, double speed)
{
	if (!clip.valid())
		return false;

	std::lock_guard<std::mutex> lock(mutex_);
	clip_ = clip;
	speed_ = clamp_speed(speed);
	anchor_wall_ns_ = obs_get_video_frame_time();
	anchor_src_ns_ = 0;
	last_emitted_seq_ = 0;
	has_emitted_ = false;
	playing_ = true;
	return true;
}

void PlaybackEngine::stop()
{
	std::lock_guard<std::mutex> lock(mutex_);
	playing_ = false;
}

void PlaybackEngine::set_angle(int angle)
{
	AngleManager::instance().set_active_angle(angle);
}

int PlaybackEngine::angle() const
{
	return AngleManager::instance().active_angle();
}

void PlaybackEngine::set_speed(double speed)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const double wanted = clamp_speed(speed);
	if (playing_) {
		/* Re-anchor first, otherwise the picture would jump by the elapsed time. */
		const uint64_t now = obs_get_video_frame_time();
		if (now > anchor_wall_ns_)
			anchor_src_ns_ += static_cast<uint64_t>((now - anchor_wall_ns_) * speed_);
		anchor_wall_ns_ = now;
	}
	speed_ = wanted;
}

bool PlaybackEngine::playing() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return playing_;
}

double PlaybackEngine::speed() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return speed_;
}

Clip PlaybackEngine::clip() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return clip_;
}

double PlaybackEngine::position_sec() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!playing_)
		return 0.0;

	const uint64_t now = obs_get_video_frame_time();
	uint64_t position = anchor_src_ns_;
	if (now > anchor_wall_ns_)
		position += static_cast<uint64_t>((now - anchor_wall_ns_) * speed_);

	return static_cast<double>(position) / 1000000000.0;
}

FramePick PlaybackEngine::next_frame()
{
	FramePick pick;

	std::lock_guard<std::mutex> lock(mutex_);
	if (!playing_)
		return pick;

	const uint64_t now = obs_get_video_frame_time();
	uint64_t position = anchor_src_ns_;
	if (now > anchor_wall_ns_)
		position += static_cast<uint64_t>((now - anchor_wall_ns_) * speed_);

	const uint64_t span = clip_.ts_out - clip_.ts_in;
	if (position >= span) {
		playing_ = false;
		pick.finished = true;
		return pick;
	}

	/*
	 * Cameras are looked up by timestamp within their own ring; clip sequence numbers only mean
	 * something in the programme ring. A camera that does not cover this instant falls back to
	 * the programme picture instead of freezing the replay.
	 */
	AngleManager &angles = AngleManager::instance();
	const uint64_t wanted_ts = clip_.ts_in + position;
	int angle = angles.active_angle();
	if (angle != kProgramAngle && !angles.angle(angle).covers(wanted_ts))
		angle = kProgramAngle;

	const FrameRing &ring = angles.angle(angle).ring();
	uint64_t seq = 0;
	bool found;
	if (angle == kProgramAngle) {
		found = ring.find_by_timestamp(wanted_ts, clip_.seq_in, clip_.seq_out, seq);
	} else {
		/* Whole ring, gap marker included: see AngleCapture::covers(). */
		const uint64_t head = ring.head();
		found = head > 0 && ring.find_by_timestamp(wanted_ts, ring.oldest(), head - 1, seq);
	}

	if (!found) {
		/* The clip fell out of the ring (only possible while recording keeps running). */
		playing_ = false;
		pick.finished = true;
		return pick;
	}

	/*
	 * Below 100 % the same source frame covers several ticks. Emitting it once is enough: the
	 * async source keeps showing the last frame, and skipping saves a full frame copy per tick.
	 */
	if (has_emitted_ && seq == last_emitted_seq_ && angle == last_emitted_angle_)
		return pick;

	if (!ring.read(seq, pick.meta, pick.planes))
		return pick;

	pick.format = ring.config().format;
	last_emitted_angle_ = angle;
	last_emitted_seq_ = seq;
	has_emitted_ = true;
	pick.has_frame = true;
	return pick;
}
