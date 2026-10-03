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

#include "frame-ring.hpp"

#include <util/base.h>
#include <util/bmem.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

/*
 * Allocating one contiguous block for a multi-gigabyte ring is asking for trouble on Windows after
 * OBS has been running (and fragmenting its heap) for hours, so the ring is split into chunks.
 */
constexpr size_t kTargetChunkBytes = 128u * 1024u * 1024u;
constexpr size_t kSlotAlignment = 4096;

size_t align_up(size_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

bool frame_plane_layout(video_format format, uint32_t height, size_t &plane_count, size_t rows[MAX_AV_PLANES])
{
	switch (format) {
	case VIDEO_FORMAT_NV12:
		plane_count = 2;
		rows[0] = height;
		rows[1] = (height + 1) / 2;
		return true;
	case VIDEO_FORMAT_I420:
		plane_count = 3;
		rows[0] = height;
		rows[1] = (height + 1) / 2;
		rows[2] = (height + 1) / 2;
		return true;
	case VIDEO_FORMAT_I444:
		plane_count = 3;
		rows[0] = height;
		rows[1] = height;
		rows[2] = height;
		return true;
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_RGBA:
		plane_count = 1;
		rows[0] = height;
		return true;
	default:
		/* P010/I010 (HDR) and the packed 4:2:2 formats are out of scope for now. */
		return false;
	}
}

FrameRing::~FrameRing()
{
	release();
}

void FrameRing::release()
{
	++generation_;
	for (uint8_t *chunk : chunks_)
		bfree(chunk);

	chunks_.clear();
	meta_.clear();
	capacity_ = 0;
	bytes_allocated_ = 0;
	slot_size_ = 0;
	slots_per_chunk_ = 0;
	plane_count_ = 0;
	head_.store(0, std::memory_order_release);
	gap_seq_.store(0, std::memory_order_release);
	held_back_.store(0, std::memory_order_relaxed);
	resume_gap_ = false;
	{
		std::lock_guard<std::mutex> lock(floor_mutex_);
		floors_ = {};
		min_floor_.store(UINT64_MAX, std::memory_order_release);
	}
	config_ = RingConfig{};
}

bool FrameRing::reconfigure(const RingConfig &config)
{
	release();

	size_t rows[MAX_AV_PLANES] = {};
	size_t plane_count = 0;
	if (!frame_plane_layout(config.format, config.height, plane_count, rows))
		return false;

	size_t frame_bytes = 0;
	for (size_t plane = 0; plane < plane_count; ++plane) {
		plane_offset_[plane] = frame_bytes;
		plane_rows_[plane] = rows[plane];
		frame_bytes += static_cast<size_t>(config.linesize[plane]) * rows[plane];
	}

	if (frame_bytes == 0)
		return false;

	const size_t slot_size = align_up(frame_bytes, kSlotAlignment);

	uint64_t wanted = static_cast<uint64_t>(std::ceil(config.duration_sec * config.fps));
	if (wanted == 0)
		return false;

	if (config.max_bytes > 0) {
		const uint64_t affordable = config.max_bytes / slot_size;
		if (affordable == 0)
			return false;
		wanted = std::min(wanted, affordable);
	}

	const size_t slots_per_chunk = std::max<size_t>(1, kTargetChunkBytes / slot_size);
	const size_t chunk_count = static_cast<size_t>((wanted + slots_per_chunk - 1) / slots_per_chunk);

	chunks_.reserve(chunk_count);
	for (size_t i = 0; i < chunk_count; ++i) {
		const size_t slots_here =
			std::min<size_t>(slots_per_chunk, static_cast<size_t>(wanted) - i * slots_per_chunk);
		uint8_t *chunk = static_cast<uint8_t *>(bmalloc(slots_here * slot_size));
		if (!chunk) {
			release();
			return false;
		}
		chunks_.push_back(chunk);
		bytes_allocated_ += slots_here * slot_size;
	}

	meta_.resize(static_cast<size_t>(wanted));
	config_ = config;
	slot_size_ = slot_size;
	slots_per_chunk_ = slots_per_chunk;
	plane_count_ = plane_count;
	capacity_ = wanted;
	head_.store(0, std::memory_order_release);
	return true;
}

uint8_t *FrameRing::slot(uint64_t index) const
{
	const size_t chunk = static_cast<size_t>(index / slots_per_chunk_);
	const size_t offset = static_cast<size_t>(index % slots_per_chunk_) * slot_size_;
	return chunks_[chunk] + offset;
}

int FrameRing::acquire_floor(uint64_t seq)
{
	std::lock_guard<std::mutex> lock(floor_mutex_);
	if (capacity_ == 0 || seq < oldest() || seq >= head())
		return -1;
	for (size_t index = 0; index < kMaxReaders; ++index) {
		if (floors_[index] != 0)
			continue;

		floors_[index] = seq + 1; /* stored offset by one so that 0 means "free" */
		uint64_t lowest = UINT64_MAX;
		for (uint64_t floor : floors_) {
			if (floor != 0)
				lowest = std::min(lowest, floor - 1);
		}
		min_floor_.store(lowest, std::memory_order_release);
		return static_cast<int>(index);
	}

	return -1; /* Caller must reject the export rather than read unprotected frames. */
}

void FrameRing::update_floor(int token, uint64_t seq)
{
	if (token < 0)
		return;

	std::lock_guard<std::mutex> lock(floor_mutex_);
	floors_[static_cast<size_t>(token)] = seq + 1;
	uint64_t lowest = UINT64_MAX;
	for (uint64_t floor : floors_) {
		if (floor != 0)
			lowest = std::min(lowest, floor - 1);
	}
	min_floor_.store(lowest, std::memory_order_release);
}

void FrameRing::release_floor(int token)
{
	if (token < 0)
		return;

	std::lock_guard<std::mutex> lock(floor_mutex_);
	floors_[static_cast<size_t>(token)] = 0;
	uint64_t lowest = UINT64_MAX;
	for (uint64_t floor : floors_) {
		if (floor != 0)
			lowest = std::min(lowest, floor - 1);
	}
	min_floor_.store(lowest, std::memory_order_release);
}

void FrameRing::write(const uint8_t *const source[MAX_AV_PLANES], const uint32_t source_linesize[MAX_AV_PLANES],
		      uint64_t timestamp, bool starts_gap)
{
	if (capacity_ == 0)
		return;

	/* Never wait on an exporter in the capture callback. This also makes claiming a
	 * floor atomic with respect to starting a slot overwrite. */
	std::unique_lock<std::mutex> floor_lock(floor_mutex_, std::try_to_lock);
	if (!floor_lock.owns_lock()) {
		held_back_.fetch_add(1, std::memory_order_relaxed);
		resume_gap_ = true;
		return;
	}

	/* Single producer: a relaxed load of our own head is enough. */
	const uint64_t seq = head_.load(std::memory_order_relaxed);

	/*
	 * Do not destroy a frame an export is still reading. Dropping the incoming frame stalls the
	 * live buffer for as long as the export lags, which is far cheaper than an incomplete clip.
	 */
	if (seq + 1 >= capacity_) {
		/* Account for the head we would publish, including the first wrap. */
		const uint64_t next_oldest = seq + 2 - capacity_;
		if (next_oldest > min_floor_.load(std::memory_order_acquire)) {
			held_back_.fetch_add(1, std::memory_order_relaxed);
			resume_gap_ = true;
			return;
		}
	}

	if (resume_gap_) {
		/* Frames were dropped: the recording is no longer continuous across this point. */
		resume_gap_ = false;
		starts_gap = true;
	}
	const size_t index = static_cast<size_t>(seq % capacity_);
	uint8_t *destination = slot(index);

	for (size_t plane = 0; plane < plane_count_; ++plane) {
		if (!source[plane])
			continue;

		const size_t rows = plane_rows_[plane];
		const size_t destination_stride = config_.linesize[plane];
		const size_t source_stride = source_linesize[plane];
		uint8_t *plane_destination = destination + plane_offset_[plane];

		if (source_stride == destination_stride) {
			memcpy(plane_destination, source[plane], destination_stride * rows);
		} else {
			/* libobs may hand out padded strides; copy row by row in that case. */
			const size_t copy_bytes = std::min(source_stride, destination_stride);
			for (size_t row = 0; row < rows; ++row)
				memcpy(plane_destination + row * destination_stride,
				       source[plane] + row * source_stride, copy_bytes);
		}
	}

	FrameMeta &meta = meta_[index];
	meta.timestamp = timestamp;
	meta.seq = seq;
	meta.width = config_.width;
	meta.height = config_.height;
	meta.starts_gap = starts_gap;
	for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane)
		meta.linesize[plane] = config_.linesize[plane];

	if (starts_gap)
		gap_seq_.store(seq, std::memory_order_release);

	/* Publish: everything above must be visible to consumers that observe the new head. */
	head_.store(seq + 1, std::memory_order_release);
}

uint64_t FrameRing::oldest() const
{
	/*
	 * Once the ring has wrapped, slot (head - capacity) is the one the writer is copying into right
	 * now: its pixels change before its metadata does, so it must not be handed to readers. The
	 * oldest readable frame is therefore one past it, and the effective capacity is N - 1.
	 */
	const uint64_t current_head = head();
	return current_head >= capacity_ ? current_head - capacity_ + 1 : 0;
}

bool FrameRing::read(uint64_t seq, FrameMeta &meta, const uint8_t *planes[MAX_AV_PLANES]) const
{
	const uint64_t current_head = head();
	if (capacity_ == 0 || seq >= current_head || seq < oldest())
		return false;

	const size_t index = static_cast<size_t>(seq % capacity_);
	meta = meta_[index];
	if (meta.seq != seq)
		return false; /* overwritten while we were reading it */

	const uint8_t *base = slot(index);
	for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane)
		planes[plane] = plane < plane_count_ ? base + plane_offset_[plane] : nullptr;

	return true;
}

bool FrameRing::timestamp_at(uint64_t seq, uint64_t &timestamp) const
{
	if (capacity_ == 0 || seq >= head() || seq < oldest())
		return false;

	const FrameMeta &meta = meta_[static_cast<size_t>(seq % capacity_)];
	if (meta.seq != seq)
		return false;

	timestamp = meta.timestamp;
	return true;
}

bool FrameRing::find_by_timestamp(uint64_t timestamp, uint64_t lo, uint64_t hi, uint64_t &seq) const
{
	if (capacity_ == 0 || hi < lo)
		return false;

	const uint64_t first = oldest();
	const uint64_t last = head();
	if (last == 0)
		return false;

	lo = std::max(lo, first);
	hi = std::min(hi, last - 1);
	if (hi < lo)
		return false;

	uint64_t low = lo;
	uint64_t high = hi;
	uint64_t found = lo;

	while (low <= high) {
		const uint64_t middle = low + (high - low) / 2;
		uint64_t middle_ts = 0;
		if (!timestamp_at(middle, middle_ts))
			return false;

		if (middle_ts <= timestamp) {
			found = middle;
			if (middle == hi)
				break;
			low = middle + 1;
		} else {
			if (middle == lo)
				break;
			high = middle - 1;
		}
	}

	seq = found;
	return true;
}

double FrameRing::buffered_seconds() const
{
	const uint64_t current_head = head();
	if (current_head == 0 || capacity_ == 0)
		return 0.0;

	const uint64_t first = oldest();
	const uint64_t last = current_head - 1;
	if (last <= first)
		return 0.0;

	const uint64_t first_ts = meta_[static_cast<size_t>(first % capacity_)].timestamp;
	const uint64_t last_ts = meta_[static_cast<size_t>(last % capacity_)].timestamp;
	if (last_ts <= first_ts)
		return 0.0;

	return static_cast<double>(last_ts - first_ts) / 1000000000.0;
}
