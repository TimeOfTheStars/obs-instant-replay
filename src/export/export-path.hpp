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

#include <string>

/*
 * Where exported clips go. The operator may pin a folder; otherwise clips land next to OBS
 * recordings. Inside, one sub-folder per calendar day collects everything from that match.
 */
std::string export_base_directory(const std::string &configured);

/*
 * Creates <base>/<YYYY-MM-DD>/ and returns the shared name prefix for one event, shaped like
 * <folder>/HH-MM-SS_<event>-NN. Every angle of the event appends its own suffix to this, so the
 * files sort next to each other. Returns false with `error` set if the folder cannot be created.
 */
bool build_export_stem(const std::string &base, const std::string &event_name, int sequence, std::string &stem,
		       std::string &error);

/* <stem>_program.mp4 / <stem>_camN.mp4 */
std::string export_angle_file(const std::string &stem, int angle);
