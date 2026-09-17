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

#include "capture-tap.hpp"

/*
 * A camera angle: a private obs_view rendering one scene into its own video mix, tapped by a raw
 * obs_output. libobs only downloads a mix to CPU memory while an output holds it active, and the
 * only exported way to hold a secondary mix active is an output — the same trick OBS' own virtual
 * camera uses for its "Scene" mode.
 */
class ViewTap : public CaptureTap {
public:
	~ViewTap() override;

	bool start(const TapRequest &request, FrameSink &sink, TapInfo &info, std::string &error) override;
	void stop() override;

	/* Called by the raw output on its video-io thread. */
	void deliver(video_data *frame);

	/* Registers the raw output type; once, from obs_module_load. */
	static void register_output_type();

private:
	FrameSink *sink_ = nullptr;
	obs_view_t *view_ = nullptr;
	video_t *video_ = nullptr;
	obs_output_t *output_ = nullptr;
};
