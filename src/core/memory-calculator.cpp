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

#include "memory-calculator.hpp"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#endif

uint64_t ring_bytes(uint32_t width, uint32_t height, video_format format, double fps, uint32_t divisor, double seconds)
{
	double bytes_per_pixel = 1.5; /* NV12 / I420 */
	switch (format) {
	case VIDEO_FORMAT_I444:
		bytes_per_pixel = 3.0;
		break;
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_RGBA:
		bytes_per_pixel = 4.0;
		break;
	default:
		break;
	}

	const uint64_t frame = static_cast<uint64_t>(std::ceil(width * height * bytes_per_pixel));
	const uint64_t slot = (frame + 4095) & ~static_cast<uint64_t>(4095);
	const double effective_fps = fps / std::max<uint32_t>(1, divisor);
	const uint64_t frames = static_cast<uint64_t>(std::ceil(effective_fps * seconds));
	return slot * frames;
}

uint64_t available_physical_memory()
{
#ifdef _WIN32
	MEMORYSTATUSEX status = {};
	status.dwLength = sizeof(status);
	if (GlobalMemoryStatusEx(&status))
		return status.ullAvailPhys;
	return 0;
#elif defined(__APPLE__)
	vm_statistics64_data_t stats = {};
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) !=
	    KERN_SUCCESS)
		return 0;

	vm_size_t page_size = 0;
	if (host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS)
		return 0;

	return static_cast<uint64_t>(stats.free_count + stats.inactive_count) * page_size;
#else
	return 0;
#endif
}

uint32_t camera_width_for_height(uint32_t base_width, uint32_t base_height, uint32_t height)
{
	if (base_width == 0 || base_height == 0 || height == 0)
		return 0;

	const double width = static_cast<double>(base_width) * height / base_height;
	return static_cast<uint32_t>(std::lround(width / 2.0)) * 2;
}
