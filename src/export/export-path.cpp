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

#include "export-path.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

std::string trim_trailing_separators(std::string path)
{
	while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
		path.pop_back();
	return path;
}

/* Windows forbids these in file names; the rest of the world is happier without them too. */
std::string sanitize_component(const std::string &name)
{
	std::string result;
	result.reserve(name.size());
	for (unsigned char c : name) {
		const bool forbidden = c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
				       c == '"' || c == '<' || c == '>' || c == '|';
		result.push_back(forbidden ? '_' : static_cast<char>(c));
	}
	while (!result.empty() && (result.back() == ' ' || result.back() == '.'))
		result.pop_back();
	return result.empty() ? std::string("event") : result;
}

void local_time(std::tm &out)
{
	const std::time_t now = std::time(nullptr);
#ifdef _WIN32
	localtime_s(&out, &now);
#else
	localtime_r(&now, &out);
#endif
}

} // namespace

std::string export_base_directory(const std::string &configured)
{
	if (!configured.empty())
		return trim_trailing_separators(configured);

	/* Same folder OBS records into, whichever output mode is active. */
	if (char *record_path = obs_frontend_get_current_record_output_path()) {
		std::string result(record_path);
		bfree(record_path);
		if (!result.empty())
			return trim_trailing_separators(result);
	}

	if (config_t *profile = obs_frontend_get_profile_config()) {
		const char *mode = config_get_string(profile, "Output", "Mode");
		const bool advanced = mode && strcmp(mode, "Advanced") == 0;
		const char *path = advanced ? config_get_string(profile, "AdvOut", "RecFilePath")
					    : config_get_string(profile, "SimpleOutput", "FilePath");
		if (path && *path)
			return trim_trailing_separators(path);
	}

	if (char *home = os_get_config_path_ptr("")) {
		std::string result(home);
		bfree(home);
		return trim_trailing_separators(result);
	}

	return ".";
}

bool build_export_path(const std::string &base, const std::string &event_name, int sequence, std::string &path,
		       std::string &error)
{
	std::tm now = {};
	local_time(now);

	char day[16];
	char clock[16];
	std::snprintf(day, sizeof(day), "%04d-%02d-%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
	std::snprintf(clock, sizeof(clock), "%02d-%02d-%02d", now.tm_hour, now.tm_min, now.tm_sec);

	const std::string directory = base + "/" + day;
	const int made = os_mkdirs(directory.c_str());
	if (made != MKDIR_SUCCESS && made != MKDIR_EXISTS) {
		error = "cannot create folder " + directory;
		return false;
	}

	const std::string stem = directory + "/" + clock + "_" + sanitize_component(event_name);
	char suffix[16];
	std::snprintf(suffix, sizeof(suffix), "-%02d", sequence);

	std::string candidate = stem + suffix + ".mp4";
	for (int attempt = 2; os_file_exists(candidate.c_str()) && attempt < 100; ++attempt)
		candidate = stem + suffix + "-" + std::to_string(attempt) + ".mp4";

	path = candidate;
	return true;
}
