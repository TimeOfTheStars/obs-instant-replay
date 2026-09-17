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

#include "replay-source.hpp"

#include "playback-engine.hpp"
#include "angle-manager.hpp"

#include <obs-module.h>
#include <media-io/video-io.h>

#include <cstring>

namespace {

struct ReplaySource {
	obs_source_t *source = nullptr;

	/* Colour conversion parameters, recomputed only when the buffer format changes. */
	video_format colour_format = VIDEO_FORMAT_NONE;
	video_range_type range = VIDEO_RANGE_DEFAULT;
	float colour_matrix[16] = {};
	float colour_range_min[3] = {};
	float colour_range_max[3] = {};
	bool colour_valid = false;
};

const char *replay_source_name(void *)
{
	return obs_module_text("Replay.SourceName");
}

void *replay_source_create(obs_data_t *, obs_source_t *source)
{
	auto *context = new ReplaySource();
	context->source = source;

	/*
	 * Pacing is done here, one frame per tick, so libobs must not buffer or re-time anything:
	 * unbuffered mode makes it show the newest frame handed to it and skip its own heuristics.
	 */
	obs_source_set_async_unbuffered(source, true);
	return context;
}

void replay_source_destroy(void *data)
{
	delete static_cast<ReplaySource *>(data);
}

bool update_colour_parameters(ReplaySource *context, video_format format)
{
	if (context->colour_valid && context->colour_format == format)
		return true;

	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
		return false;

	/*
	 * obs_source_frame2 carries no colour space field: without the matrix filled in, the replay
	 * comes out grey or blown out.
	 */
	if (!video_format_get_parameters_for_format(ovi.colorspace, ovi.range, format, context->colour_matrix,
						    context->colour_range_min, context->colour_range_max))
		return false;

	context->colour_format = format;
	context->range = ovi.range;
	context->colour_valid = true;
	return true;
}

void replay_source_video_tick(void *data, float)
{
	auto *context = static_cast<ReplaySource *>(data);

	/* Keeps the ring alive for as long as we hold pointers into it. */
	const auto ring_guard = AngleManager::instance().reader_guard();

	const FramePick pick = PlaybackEngine::instance().next_frame();
	if (!pick.has_frame)
		return;

	const video_format format = pick.format;
	if (!update_colour_parameters(context, format))
		return;

	obs_source_frame2 frame = {};
	for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane) {
		frame.data[plane] = const_cast<uint8_t *>(pick.planes[plane]);
		frame.linesize[plane] = pick.meta.linesize[plane];
	}

	frame.width = pick.meta.width;
	frame.height = pick.meta.height;
	frame.format = format;
	frame.range = context->range;
	/*
	 * The timestamp is the current OBS time, not the (stretched) source one: slow motion comes
	 * from sampling the ring more slowly, not from re-timing the output.
	 */
	frame.timestamp = obs_get_video_frame_time();
	frame.trc = VIDEO_TRC_DEFAULT;
	memcpy(frame.color_matrix, context->colour_matrix, sizeof(frame.color_matrix));
	memcpy(frame.color_range_min, context->colour_range_min, sizeof(frame.color_range_min));
	memcpy(frame.color_range_max, context->colour_range_max, sizeof(frame.color_range_max));

	obs_source_output_video2(context->source, &frame);
}

} // namespace

void register_replay_source()
{
	obs_source_info info = {};
	info.id = kReplaySourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = replay_source_name;
	info.create = replay_source_create;
	info.destroy = replay_source_destroy;
	info.video_tick = replay_source_video_tick;
	info.icon_type = OBS_ICON_TYPE_MEDIA;

	obs_register_source(&info);
}
