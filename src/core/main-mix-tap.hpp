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

/* The programme mix, tapped through libobs' raw video callback on the main video output. */
class MainMixTap : public CaptureTap {
public:
	~MainMixTap() override;

	bool start(const TapRequest &request, FrameSink &sink, TapInfo &info, std::string &error) override;
	void stop() override;

private:
	static void raw_video(void *param, video_data *frame);

	FrameSink *sink_ = nullptr;
	bool active_ = false;
};
