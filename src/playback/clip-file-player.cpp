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

#include "clip-file-player.hpp"

#include <plugin-support.h>

ClipFilePlayer &ClipFilePlayer::instance()
{
	static ClipFilePlayer player;
	return player;
}

void ClipFilePlayer::prepare(const std::array<std::optional<FileClipInfo>, kAngleCount> &files, uint64_t start_ts)
{
	std::array<std::unique_ptr<ClipFileSource>, kAngleCount> replaced;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (int angle = 0; angle < kAngleCount; ++angle) {
			const auto &wanted = files[static_cast<size_t>(angle)];
			auto &source = sources_[static_cast<size_t>(angle)];

			if (!wanted) {
				replaced[static_cast<size_t>(angle)] = std::move(source);
				continue;
			}

			replaced[static_cast<size_t>(angle)] = std::move(source);
			source = std::make_unique<ClipFileSource>(*wanted);
			source->start(start_ts);
		}
	}

	/* Joining decoder threads outside the lock: pick() from the graphics thread must not wait. */
	replaced = {};
}

bool ClipFilePlayer::has_source(int angle) const
{
	if (angle < 0 || angle >= kAngleCount)
		return false;

	std::lock_guard<std::mutex> lock(mutex_);
	return sources_[static_cast<size_t>(angle)] != nullptr;
}

bool ClipFilePlayer::ready(int angle) const
{
	if (angle < 0 || angle >= kAngleCount)
		return false;

	std::lock_guard<std::mutex> lock(mutex_);
	const auto &source = sources_[static_cast<size_t>(angle)];
	return source && source->ready();
}

bool ClipFilePlayer::failed(int angle) const
{
	if (angle < 0 || angle >= kAngleCount)
		return false;

	std::lock_guard<std::mutex> lock(mutex_);
	const auto &source = sources_[static_cast<size_t>(angle)];
	return source && source->failed();
}

void ClipFilePlayer::set_active_angle(int angle)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (int index = 0; index < kAngleCount; ++index) {
		if (sources_[static_cast<size_t>(index)])
			sources_[static_cast<size_t>(index)]->set_active(index == angle);
	}
}

bool ClipFilePlayer::pick(int angle, uint64_t ts, DecodedFrame &out)
{
	if (angle < 0 || angle >= kAngleCount)
		return false;

	std::lock_guard<std::mutex> lock(mutex_);
	ClipFileSource *source = sources_[static_cast<size_t>(angle)].get();
	return source && source->pick(ts, out);
}

void ClipFilePlayer::close_all()
{
	std::array<std::unique_ptr<ClipFileSource>, kAngleCount> closing;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		closing = std::move(sources_);
		sources_ = {};
	}
	closing = {};
}
