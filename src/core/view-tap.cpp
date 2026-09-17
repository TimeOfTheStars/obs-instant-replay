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

#include "view-tap.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>

namespace {

constexpr const char *kRawOutputId = "trinity_replay_raw_output";

struct RawOutput {
	obs_output_t *output = nullptr;
	ViewTap *tap = nullptr;
};

const char *raw_output_name(void *)
{
	return "Instant Replay camera tap";
}

void *raw_output_create(obs_data_t *settings, obs_output_t *output)
{
	auto *context = new RawOutput();
	context->output = output;
	/* The owning tap is smuggled through the settings; outputs are only created from the Qt thread. */
	context->tap = reinterpret_cast<ViewTap *>(static_cast<intptr_t>(obs_data_get_int(settings, "tap")));
	return context;
}

void raw_output_destroy(void *data)
{
	delete static_cast<RawOutput *>(data);
}

bool raw_output_start(void *data)
{
	auto *context = static_cast<RawOutput *>(data);
	if (!obs_output_can_begin_data_capture(context->output, 0))
		return false;
	return obs_output_begin_data_capture(context->output, 0);
}

void raw_output_stop(void *data, uint64_t)
{
	auto *context = static_cast<RawOutput *>(data);
	obs_output_end_data_capture(context->output);
}

void raw_output_video(void *data, video_data *frame)
{
	auto *context = static_cast<RawOutput *>(data);
	if (context->tap)
		context->tap->deliver(frame);
}

} // namespace

void ViewTap::register_output_type()
{
	obs_output_info info = {};
	info.id = kRawOutputId;
	info.flags = OBS_OUTPUT_VIDEO;
	info.get_name = raw_output_name;
	info.create = raw_output_create;
	info.destroy = raw_output_destroy;
	info.start = raw_output_start;
	info.stop = raw_output_stop;
	info.raw_video = raw_output_video;
	obs_register_output(&info);
}

ViewTap::~ViewTap()
{
	stop();
}

bool ViewTap::start(const TapRequest &request, FrameSink &sink, TapInfo &info, std::string &error)
{
	stop();

	obs_source_t *scene = request.scene ? obs_weak_source_get_source(request.scene) : nullptr;
	if (!scene) {
		error = "scene is not available";
		return false;
	}

	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		obs_source_release(scene);
		error = "video output is not initialised";
		return false;
	}

	/* Same canvas, frame rate and colour as the programme; only the output size differs. */
	if (request.output_width && request.output_height) {
		ovi.output_width = request.output_width;
		ovi.output_height = request.output_height;
	}

	view_ = obs_view_create();
	obs_view_set_source(view_, 0, scene);
	obs_source_release(scene);

	video_ = obs_view_add2(view_, &ovi);
	if (!video_) {
		obs_view_set_source(view_, 0, nullptr);
		obs_view_destroy(view_);
		view_ = nullptr;
		error = "could not create a video mix for the camera";
		return false;
	}

	sink_ = &sink;

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "tap", static_cast<long long>(reinterpret_cast<intptr_t>(this)));
	output_ = obs_output_create(kRawOutputId, "Instant Replay camera tap", settings, nullptr);
	obs_data_release(settings);

	if (!output_) {
		error = "raw output type is not registered";
		stop();
		return false;
	}

	/* No conversion request: the mix is consumed as it is produced, no swscale. */
	obs_output_set_media(output_, video_, nullptr);
	if (!obs_output_start(output_)) {
		error = "could not start the camera tap";
		stop();
		return false;
	}

	info.width = ovi.output_width;
	info.height = ovi.output_height;
	info.format = ovi.output_format;
	info.fps = ovi.fps_den ? static_cast<double>(ovi.fps_num) / ovi.fps_den : 0.0;
	info.divisor_applied = false; /* outputs always get every frame; the sink decimates */
	return true;
}

void ViewTap::stop()
{
	/*
	 * Order matters. Releasing the output joins its end-data-capture thread, so only afterwards is
	 * the callback guaranteed silent; only then may the mix (video_t) be torn down via the view.
	 */
	if (output_) {
		obs_output_stop(output_);
		obs_output_release(output_);
		output_ = nullptr;
	}

	sink_ = nullptr;

	if (view_) {
		if (video_) {
			obs_view_remove(view_);
			video_ = nullptr;
		}
		obs_view_set_source(view_, 0, nullptr);
		obs_view_destroy(view_);
		view_ = nullptr;
	}
}

void ViewTap::deliver(video_data *frame)
{
	if (sink_)
		sink_->on_frame(frame);
}
