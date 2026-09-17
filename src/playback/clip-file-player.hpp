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

#include "clip-file-source.hpp"

#include "core/angle-manager.hpp"

#include <array>
#include <memory>
#include <mutex>
#include <optional>

/*
 * Keeps a decoder open for every saved angle of the event the operator has selected.
 *
 * All of them are opened at once and follow the same playback position, so switching angle during
 * a replay costs nothing; opening on demand would mean a seek of up to a couple of hundred
 * milliseconds — several visibly dropped frames.
 */
class ClipFilePlayer {
public:
	static ClipFilePlayer &instance();

	/* Qt thread: make these files ready to play from `start_ts`. Replaces the previous set. */
	void prepare(const std::array<std::optional<FileClipInfo>, kAngleCount> &files, uint64_t start_ts);

	/* Qt thread: true if this angle has a decoder that opened successfully. */
	bool ready(int angle) const;
	bool failed(int angle) const;
	bool has_source(int angle) const;

	void set_active_angle(int angle);

	/* Graphics thread: newest decoded frame at or before `ts`. Never blocks. */
	bool pick(int angle, uint64_t ts, DecodedFrame &out);

	/* Qt thread, before the rings go away. */
	void close_all();

private:
	ClipFilePlayer() = default;

	mutable std::mutex mutex_;
	std::array<std::unique_ptr<ClipFileSource>, kAngleCount> sources_;
};
