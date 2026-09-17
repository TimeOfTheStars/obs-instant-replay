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

#include "main-mix-tap.hpp"

#include <algorithm>

MainMixTap::~MainMixTap()
{
	stop();
}

bool MainMixTap::start(const TapRequest &request, FrameSink &sink, TapInfo &info, std::string &error)
{
	stop();

	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		error = "video output is not initialised";
		return false;
	}

	if (ovi.fps_den == 0 || ovi.fps_num == 0) {
		error = "video output reports no frame rate";
		return false;
	}

	const uint32_t divisor = std::max<uint32_t>(1, request.frame_rate_divisor);

	/*
	 * Asking for exactly what the mix already produces keeps libobs from spinning up a swscale
	 * converter, which would run on the video-io thread for every subscriber, encoders included.
	 */
	video_scale_info conversion = {};
	conversion.format = ovi.output_format;
	conversion.width = ovi.output_width;
	conversion.height = ovi.output_height;
	conversion.range = ovi.range;
	conversion.colorspace = ovi.colorspace;

	sink_ = &sink;
	active_ = true;
	/* The divisor is applied inside libobs before any conversion or download: free. */
	obs_add_raw_video_callback2(&conversion, divisor, raw_video, this);

	info.width = ovi.output_width;
	info.height = ovi.output_height;
	info.format = ovi.output_format;
	info.fps = static_cast<double>(ovi.fps_num) / ovi.fps_den / divisor;
	info.divisor_applied = true;
	return true;
}

void MainMixTap::stop()
{
	if (!active_)
		return;

	/* Returns after the video-io thread has let go of the callback. */
	obs_remove_raw_video_callback(raw_video, this);
	active_ = false;
	sink_ = nullptr;
}

void MainMixTap::raw_video(void *param, video_data *frame)
{
	auto *tap = static_cast<MainMixTap *>(param);
	if (tap->sink_)
		tap->sink_->on_frame(frame);
}
