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

#include "replay-director.hpp"

#include "angle-manager.hpp"
#include "replay-source.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/vec2.h>
#include <util/platform.h>

namespace {

constexpr const char *kReplaySceneName = "Instant Replay";

/* Recording resumes this long after the return transition starts, so no replay tail is captured. */
constexpr uint64_t kResumeGuardNs = 1200000000; /* 1.2 s */

/* Safety net in case playback never reports completion. */
uint64_t watchdog_for(double duration_sec, double speed)
{
	const double playback_sec = speed > 0.0 ? duration_sec / speed : duration_sec;
	return os_gettime_ns() + static_cast<uint64_t>((playback_sec + 5.0) * 1000000000.0);
}

bool same_source(obs_weak_source_t *weak, obs_source_t *source)
{
	return weak && source && obs_weak_source_references_source(weak, source);
}

} // namespace

ReplayDirector &ReplayDirector::instance()
{
	static ReplayDirector director;
	return director;
}

obs_source_t *ReplayDirector::ensure_replay_scene()
{
	if (replay_scene_) {
		obs_source_t *existing = obs_weak_source_get_source(replay_scene_);
		if (existing)
			return existing; /* caller releases */

		obs_weak_source_release(replay_scene_);
		replay_scene_ = nullptr;
	}

	/* The scene lives in the user's scene collection, so it may already be there from last time. */
	obs_source_t *found = obs_get_source_by_name(kReplaySceneName);
	if (found) {
		if (obs_source_get_type(found) == OBS_SOURCE_TYPE_SCENE) {
			replay_scene_ = obs_source_get_weak_source(found);
			return found;
		}

		/* Something else already owns the name; do not hijack the user's source. */
		obs_log(LOG_ERROR, "'%s' exists but is not a scene", kReplaySceneName);
		obs_source_release(found);
		return nullptr;
	}

	obs_scene_t *scene = obs_scene_create(kReplaySceneName);
	if (!scene) {
		obs_log(LOG_ERROR, "could not create the replay scene");
		return nullptr;
	}

	obs_source_t *replay =
		obs_source_create(kReplaySourceId, obs_module_text("Replay.SourceName"), nullptr, nullptr);
	if (!replay) {
		obs_log(LOG_ERROR, "could not create the replay source");
		obs_scene_release(scene);
		return nullptr;
	}

	obs_sceneitem_t *item = obs_scene_add(scene, replay);
	if (item) {
		obs_video_info ovi = {};
		if (obs_get_video_info(&ovi)) {
			/* Fit the canvas, keeping the aspect ratio whatever the buffer resolution is. */
			vec2 bounds;
			vec2_set(&bounds, static_cast<float>(ovi.base_width), static_cast<float>(ovi.base_height));
			obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
			obs_sceneitem_set_bounds(item, &bounds);
			obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
		}
	}

	obs_source_release(replay);

	obs_source_t *scene_source = obs_source_get_ref(obs_scene_get_source(scene));
	replay_scene_ = obs_source_get_weak_source(scene_source);
	obs_scene_release(scene);

	obs_log(LOG_INFO, "created scene '%s'", kReplaySceneName);
	return scene_source;
}

bool ReplayDirector::play_to_program(const Clip &clip, double speed, bool auto_return)
{
	if (!clip.valid())
		return false;

	obs_source_t *replay_scene = ensure_replay_scene();
	if (!replay_scene)
		return false;

	obs_source_t *current = obs_frontend_get_current_scene();
	if (current && same_source(replay_scene_, current)) {
		/* Already on the replay scene: just restart playback, do not overwrite the return scene. */
		obs_source_release(current);
	} else {
		if (return_scene_) {
			obs_weak_source_release(return_scene_);
			return_scene_ = nullptr;
		}
		if (current) {
			return_scene_ = obs_source_get_weak_source(current);
			obs_source_release(current);
		}
	}

	/*
	 * Stop recording before the transition starts: the ring captures the program mix, so a replay
	 * on program would otherwise be recorded back into the buffer.
	 */
	AngleManager::instance().set_paused_all(true);

	if (!PlaybackEngine::instance().play(clip, speed)) {
		AngleManager::instance().set_paused_all(false);
		obs_source_release(replay_scene);
		return false;
	}

	auto_return_ = auto_return;
	active_ = true;
	watchdog_deadline_ns_ = watchdog_for(clip.duration_sec(), speed);
	resume_capture_at_ns_ = 0;

	obs_frontend_set_current_scene(replay_scene);
	obs_source_release(replay_scene);

	obs_log(LOG_INFO, "replay on program: %.1f s at %.0f%%", clip.duration_sec(), speed * 100.0);
	return true;
}

void ReplayDirector::stop()
{
	PlaybackEngine::instance().stop();
	finish(true);
}

void ReplayDirector::finish(bool return_to_live)
{
	if (!active_)
		return;

	active_ = false;
	AngleManager::instance().set_active_angle(kProgramAngle);

	if (return_to_live && return_scene_) {
		obs_source_t *live = obs_weak_source_get_source(return_scene_);
		if (live) {
			obs_frontend_set_current_scene(live);
			obs_source_release(live);
		} else {
			obs_log(LOG_WARNING, "live scene is gone, staying on the replay scene");
		}
	}

	/* Let the return transition finish before the ring starts recording again. */
	resume_capture_at_ns_ = os_gettime_ns() + kResumeGuardNs;
}

void ReplayDirector::poll()
{
	if (resume_capture_at_ns_ && os_gettime_ns() >= resume_capture_at_ns_) {
		resume_capture_at_ns_ = 0;
		AngleManager::instance().set_paused_all(false);
	}

	if (!active_)
		return;

	/* If the operator switched away themselves, the replay is over and we do not fight them. */
	obs_source_t *current = obs_frontend_get_current_scene();
	const bool on_replay_scene = same_source(replay_scene_, current);
	if (current)
		obs_source_release(current);

	if (!on_replay_scene) {
		obs_log(LOG_INFO, "operator switched scenes during the replay, standing down");
		PlaybackEngine::instance().stop();
		finish(false);
		return;
	}

	if (!PlaybackEngine::instance().playing()) {
		finish(auto_return_);
		return;
	}

	if (os_gettime_ns() > watchdog_deadline_ns_) {
		obs_log(LOG_WARNING, "replay watchdog fired, returning to the live scene");
		PlaybackEngine::instance().stop();
		finish(true);
	}
}

void ReplayDirector::reset()
{
	active_ = false;
	watchdog_deadline_ns_ = 0;
	resume_capture_at_ns_ = 0;
	PlaybackEngine::instance().stop();

	if (return_scene_) {
		obs_weak_source_release(return_scene_);
		return_scene_ = nullptr;
	}
	if (replay_scene_) {
		obs_weak_source_release(replay_scene_);
		replay_scene_ = nullptr;
	}
}
