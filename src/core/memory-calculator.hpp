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

#include <cstdint>

/* Bytes one ring takes; mirrors FrameRing::reconfigure so the panel's estimate matches reality. */
uint64_t ring_bytes(uint32_t width, uint32_t height, video_format format, double fps, uint32_t divisor, double seconds);

/* Free physical memory right now, 0 if unknown. */
uint64_t available_physical_memory();

/* Camera buffer width for a given height, keeping the canvas aspect ratio and even dimensions. */
uint32_t camera_width_for_height(uint32_t base_width, uint32_t base_height, uint32_t height);
