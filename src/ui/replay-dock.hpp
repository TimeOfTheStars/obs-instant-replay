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

#include "core/angle-manager.hpp"
#include "core/playback-engine.hpp"

#include <obs.h>

#include <QWidget>

#include <array>
#include <utility>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTimer;
class ReplayTimeline;

/*
 * Operator panel. Every widget here lives on the Qt thread only: the capture/playback core
 * runs on the OBS video thread and must reach this class through queued invocations.
 */
class ReplayDock : public QWidget {
	Q_OBJECT

public:
	explicit ReplayDock(QWidget *parent = nullptr);
	~ReplayDock() override;

public slots:
	/* Invoked from hotkey callbacks, which do not run on the Qt thread. */
	void onMark();
	void onPlay();
	void onStop();
	void cycleSpeed();
	void selectAngleProgram();
	void selectAngle1();
	void selectAngle2();
	void selectAngle3();

	/* Scenes changed (added, renamed, collection switched): refill the camera pickers. */
	void refreshSceneList();

private slots:
	void onSpeedChanged(int speed_percent);
	void onEventActivated();
	void onEventSelected();
	void onTimelineChanged(double in_sec, double out_sec);
	void onEventsContextMenu(const QPoint &position);
	void onSettingsChanged();
	void applyBufferSetting();
	void onCameraSettingsChanged();
	void onAngleClicked(int angle);
	void renameSelectedEvent();
	void deleteSelectedEvent();
	void refreshStatus();

private:
	QWidget *buildStatusRow();
	QWidget *buildCaptureRow();
	QWidget *buildSpeedRow();
	QWidget *buildAnglesBox();
	QWidget *buildAngleRow();
	QWidget *buildTimelineRow();
	QWidget *buildEventsBox();
	QWidget *buildTransportRow();
	QWidget *buildExportBox();

	/* status row */
	QProgressBar *buffer_bar = nullptr;
	QLabel *buffer_label = nullptr;
	QLabel *format_label = nullptr;
	QLabel *onair_label = nullptr;

	/* capture settings */
	QDoubleSpinBox *length_spin = nullptr;
	QDoubleSpinBox *offset_spin = nullptr;
	QDoubleSpinBox *buffer_spin = nullptr;

	/* transport */
	QButtonGroup *speed_group = nullptr;
	QPushButton *mark_button = nullptr;
	QPushButton *play_button = nullptr;
	QPushButton *stop_button = nullptr;
	QCheckBox *auto_return_check = nullptr;
	QCheckBox *export_check = nullptr;
	QLineEdit *export_dir_edit = nullptr;
	QComboBox *export_encoder_combo = nullptr;
	QListWidget *events_list = nullptr;
	ReplayTimeline *timeline = nullptr;
	QLabel *timeline_label = nullptr;

	/* angles */
	std::array<QCheckBox *, kCameraCount> camera_checks = {};
	std::array<QComboBox *, kCameraCount> camera_combos = {};
	QComboBox *camera_height_combo = nullptr;
	QLabel *memory_label = nullptr;
	QLabel *cameras_label = nullptr;
	QButtonGroup *angle_group = nullptr;
	bool scene_list_updating = false;

	QTimer *status_timer = nullptr;
	QTimer *save_timer = nullptr;

	struct Event {
		Clip clip;
		QString name;
		int export_job = 0; /* 0 = not exported */
	};

	/* Marked clips, in the order they were created; the list widget stores indices into this. */
	std::vector<Event> events;
	int speed_percent = 100;

	bool play(int event_index);
	int selectedEvent() const;
	void clampClipLength();
	void updateAngleButtons();
	void updateMemoryLabel();
	void selectAngle(int angle);
	void rebuildEventList();
	void updateEventItem(int event_index);
	void showSelection(int event_index);

	void registerHotkeys();
	void unregisterHotkeys();

public:
	/* Hotkey bindings live in the profile config, so they have to be re-read on a profile switch. */
	void reloadHotkeyBindings();

private:
	std::vector<std::pair<obs_hotkey_id, const char *>> hotkeys;
};
