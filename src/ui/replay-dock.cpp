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

#include "replay-dock.hpp"

#include "replay-timeline.hpp"

#include "core/playback-engine.hpp"
#include "core/plugin-settings.hpp"
#include "core/memory-calculator.hpp"
#include "core/replay-director.hpp"
#include "export/clip-exporter.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/config-file.h>

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QAction>
#include <QInputDialog>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QShortcut>
#include <QTime>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>

namespace {

constexpr int kStatusIntervalMs = 100; /* 10 Hz is enough for a buffer gauge */

/*
 * A clip as long as the ring has its first frame overwritten ~20 ms after MARK, before an export
 * can even open the encoder. Keeping this much of the ring free of clips gives the export a head
 * start of ~100 frames at 50 fps.
 */
constexpr double kExportGuardSec = 2.0;
constexpr int kSpeeds[] = {25, 50, 75, 100};

QLabel *makeBadge(const QString &text)
{
	auto *label = new QLabel(text);
	QFont font = label->font();
	font.setBold(true);
	label->setFont(font);
	label->setAlignment(Qt::AlignCenter);
	/* Colours come from the palette so the badge stays readable in every OBS theme. */
	label->setFrameShape(QFrame::StyledPanel);
	label->setMinimumWidth(70);
	return label;
}

} // namespace

ReplayDock::ReplayDock(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	layout->addWidget(buildStatusRow());
	layout->addWidget(buildCaptureRow());
	layout->addWidget(buildAnglesBox());
	layout->addWidget(buildSpeedRow());
	layout->addWidget(buildAngleRow());
	layout->addWidget(buildTimelineRow());
	layout->addWidget(buildEventsBox(), 1);
	layout->addWidget(buildExportBox());
	layout->addWidget(buildTransportRow());

	setMinimumWidth(320);

	/* Keyboard shortcuts inside the panel; the global hotkeys are registered separately. */
	auto *rename_shortcut = new QShortcut(QKeySequence(Qt::Key_F2), this);
	rename_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
	connect(rename_shortcut, &QShortcut::activated, this, &ReplayDock::renameSelectedEvent);

	auto *delete_shortcut = new QShortcut(QKeySequence(Qt::Key_Delete), this);
	delete_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
	connect(delete_shortcut, &QShortcut::activated, this, &ReplayDock::deleteSelectedEvent);

	registerHotkeys();
	refreshSceneList();

	status_timer = new QTimer(this);
	connect(status_timer, &QTimer::timeout, this, &ReplayDock::refreshStatus);
	status_timer->start(kStatusIntervalMs);

	/* Writing the config on every spinbox step would hit the disk far too often. */
	save_timer = new QTimer(this);
	save_timer->setSingleShot(true);
	save_timer->setInterval(1000);
	connect(save_timer, &QTimer::timeout, this, [] { PluginSettings::instance().save(); });
}

ReplayDock::~ReplayDock()
{
	unregisterHotkeys();
}

namespace {

/*
 * OBS saves plugin hotkey bindings itself (profile config, [Hotkeys] section) but only loads its
 * own, so the bindings have to be read back by hand.
 */
void load_binding(obs_hotkey_id id, const char *name)
{
	config_t *profile = obs_frontend_get_profile_config();
	if (!profile)
		return;

	const char *json = config_get_string(profile, "Hotkeys", name);
	if (!json)
		return;

	obs_data_t *data = obs_data_create_from_json(json);
	if (!data)
		return;

	obs_data_array_t *bindings = obs_data_get_array(data, "bindings");
	if (bindings) {
		obs_hotkey_load(id, bindings);
		obs_data_array_release(bindings);
	}
	obs_data_release(data);
}

struct HotkeyTarget {
	ReplayDock *dock;
	const char *slot;
};

void hotkey_pressed(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;

	/* Hotkey callbacks run off the Qt thread: never touch widgets from here. */
	auto *target = static_cast<HotkeyTarget *>(data);
	QMetaObject::invokeMethod(target->dock, target->slot, Qt::QueuedConnection);
}

} // namespace

void ReplayDock::registerHotkeys()
{
	static HotkeyTarget targets[] = {
		{nullptr, "onMark"},
		{nullptr, "onPlay"},
		{nullptr, "onStop"},
		{nullptr, "cycleSpeed"},
		{nullptr, "selectAngleProgram"},
		{nullptr, "selectAngle1"},
		{nullptr, "selectAngle2"},
		{nullptr, "selectAngle3"},
	};
	static const char *names[] = {
		"instant_replay.mark",        "instant_replay.play_last",     "instant_replay.stop",
		"instant_replay.speed_cycle", "instant_replay.angle_program", "instant_replay.angle_1",
		"instant_replay.angle_2",     "instant_replay.angle_3",
	};
	static const char *descriptions[] = {
		"Replay.Hotkey.Mark",       "Replay.Hotkey.Play",         "Replay.Hotkey.Stop",
		"Replay.Hotkey.SpeedCycle", "Replay.Hotkey.AngleProgram", "Replay.Hotkey.Angle1",
		"Replay.Hotkey.Angle2",     "Replay.Hotkey.Angle3",
	};

	for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
		targets[i].dock = this;
		const obs_hotkey_id id = obs_hotkey_register_frontend(names[i], obs_module_text(descriptions[i]),
								      hotkey_pressed, &targets[i]);
		if (id == OBS_INVALID_HOTKEY_ID)
			continue;

		load_binding(id, names[i]);
		hotkeys.emplace_back(id, names[i]);
	}
}

void ReplayDock::unregisterHotkeys()
{
	for (const auto &entry : hotkeys)
		obs_hotkey_unregister(entry.first);

	hotkeys.clear();
}

void ReplayDock::reloadHotkeyBindings()
{
	for (const auto &entry : hotkeys)
		load_binding(entry.first, entry.second);
}

QWidget *ReplayDock::buildStatusRow()
{
	auto *box = new QWidget(this);
	auto *outer = new QVBoxLayout(box);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(4);

	auto *top = new QHBoxLayout();
	buffer_bar = new QProgressBar(box);
	buffer_bar->setRange(0, 1000);
	buffer_bar->setValue(0);
	buffer_bar->setTextVisible(false);
	buffer_bar->setFixedHeight(12);

	buffer_label = new QLabel(QStringLiteral("0.0 / 0.0 s"), box);
	onair_label = makeBadge(obs_module_text("Replay.OnAir.Live"));

	top->addWidget(buffer_bar, 1);
	top->addWidget(buffer_label);
	top->addWidget(onair_label);

	format_label = new QLabel(obs_module_text("Replay.Status.NoBuffer"), box);
	format_label->setEnabled(false);

	outer->addLayout(top);
	outer->addWidget(format_label);
	return box;
}

QWidget *ReplayDock::buildCaptureRow()
{
	auto *box = new QWidget(this);
	auto *row = new QHBoxLayout(box);
	row->setContentsMargins(0, 0, 0, 0);

	mark_button = new QPushButton(obs_module_text("Replay.Mark"), box);
	mark_button->setMinimumHeight(56);
	QFont mark_font = mark_button->font();
	mark_font.setBold(true);
	mark_font.setPointSize(mark_font.pointSize() + 2);
	mark_button->setFont(mark_font);
	connect(mark_button, &QPushButton::clicked, this, &ReplayDock::onMark);

	auto *form = new QFormLayout();
	form->setContentsMargins(0, 0, 0, 0);

	/* A goal is roughly "4 s before the whistle, 2 s after", hence two numbers, not a range. */
	length_spin = new QDoubleSpinBox(box);
	length_spin->setRange(1.0, 30.0);
	length_spin->setSingleStep(0.5);
	length_spin->setValue(PluginSettings::instance().clip_length_sec);
	length_spin->setSuffix(QStringLiteral(" s"));

	offset_spin = new QDoubleSpinBox(box);
	offset_spin->setRange(0.0, 30.0);
	offset_spin->setSingleStep(0.5);
	offset_spin->setValue(PluginSettings::instance().clip_trim_sec);
	offset_spin->setSuffix(QStringLiteral(" s"));

	/* How much footage the ring keeps; changing it reallocates, so it is applied on commit. */
	buffer_spin = new QDoubleSpinBox(box);
	buffer_spin->setRange(3.0, 60.0);
	buffer_spin->setSingleStep(1.0);
	buffer_spin->setValue(PluginSettings::instance().buffer_seconds);
	buffer_spin->setSuffix(QStringLiteral(" s"));
	buffer_spin->setKeyboardTracking(false);

	form->addRow(obs_module_text("Replay.Length"), length_spin);
	form->addRow(obs_module_text("Replay.Offset"), offset_spin);
	form->addRow(obs_module_text("Replay.BufferLength"), buffer_spin);

	clampClipLength();

	connect(length_spin, &QDoubleSpinBox::valueChanged, this, &ReplayDock::onSettingsChanged);
	connect(offset_spin, &QDoubleSpinBox::valueChanged, this, &ReplayDock::onSettingsChanged);
	connect(buffer_spin, &QDoubleSpinBox::valueChanged, this, &ReplayDock::applyBufferSetting);

	row->addWidget(mark_button, 1);
	row->addLayout(form, 1);
	return box;
}

QWidget *ReplayDock::buildSpeedRow()
{
	auto *box = new QWidget(this);
	auto *row = new QHBoxLayout(box);
	row->setContentsMargins(0, 0, 0, 0);
	row->addWidget(new QLabel(obs_module_text("Replay.Speed"), box));

	speed_percent = PluginSettings::instance().speed_percent;
	speed_group = new QButtonGroup(this);
	speed_group->setExclusive(true);

	for (int speed : kSpeeds) {
		auto *button = new QPushButton(QStringLiteral("%1%").arg(speed), box);
		button->setCheckable(true);
		button->setChecked(speed == PluginSettings::instance().speed_percent);
		speed_group->addButton(button, speed);
		row->addWidget(button, 1);
	}

	connect(speed_group, &QButtonGroup::idClicked, this, &ReplayDock::onSpeedChanged);
	return box;
}

QWidget *ReplayDock::buildAnglesBox()
{
	auto *box = new QGroupBox(obs_module_text("Replay.Angles"), this);
	auto *layout = new QVBoxLayout(box);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(4);

	for (int camera = 0; camera < kCameraCount; ++camera) {
		auto *row = new QHBoxLayout();
		auto *check = new QCheckBox(
			QStringLiteral("%1 %2").arg(obs_module_text("Replay.Angles.Camera")).arg(camera + 1), box);
		auto *combo = new QComboBox(box);
		combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		combo->setMinimumContentsLength(12);
		row->addWidget(check);
		row->addWidget(combo, 1);
		layout->addLayout(row);

		camera_checks[static_cast<size_t>(camera)] = check;
		camera_combos[static_cast<size_t>(camera)] = combo;
		connect(check, &QCheckBox::toggled, this, &ReplayDock::onCameraSettingsChanged);
		connect(combo, &QComboBox::currentIndexChanged, this, &ReplayDock::onCameraSettingsChanged);
	}

	auto *height_row = new QHBoxLayout();
	height_row->addWidget(new QLabel(obs_module_text("Replay.Angles.Height"), box));
	camera_height_combo = new QComboBox(box);
	camera_height_combo->addItem(QStringLiteral("540p"), 540);
	camera_height_combo->addItem(QStringLiteral("720p"), 720);
	camera_height_combo->addItem(QStringLiteral("1080p"), 1080);
	const int height_index =
		camera_height_combo->findData(static_cast<int>(PluginSettings::instance().camera_height));
	camera_height_combo->setCurrentIndex(height_index >= 0 ? height_index : 1);
	connect(camera_height_combo, &QComboBox::currentIndexChanged, this, &ReplayDock::onCameraSettingsChanged);
	height_row->addWidget(camera_height_combo, 1);
	layout->addLayout(height_row);

	/* The one number that decides whether this machine survives the match: total ring memory. */
	memory_label = new QLabel(box);
	memory_label->setWordWrap(true);
	layout->addWidget(memory_label);

	cameras_label = new QLabel(box);
	cameras_label->setWordWrap(true);
	cameras_label->setEnabled(false);
	layout->addWidget(cameras_label);
	return box;
}

QWidget *ReplayDock::buildAngleRow()
{
	auto *box = new QWidget(this);
	auto *row = new QHBoxLayout(box);
	row->setContentsMargins(0, 0, 0, 0);
	row->addWidget(new QLabel(obs_module_text("Replay.Angle"), box));

	angle_group = new QButtonGroup(this);
	angle_group->setExclusive(true);

	for (int angle = 0; angle < kAngleCount; ++angle) {
		const QString text =
			angle == kProgramAngle
				? QString(obs_module_text("Replay.Angle.Program"))
				: QStringLiteral("%1 %2").arg(obs_module_text("Replay.Angles.Camera")).arg(angle);
		auto *button = new QPushButton(text, box);
		button->setCheckable(true);
		button->setChecked(angle == kProgramAngle);
		angle_group->addButton(button, angle);
		row->addWidget(button, 1);
	}

	connect(angle_group, &QButtonGroup::idClicked, this, &ReplayDock::onAngleClicked);
	return box;
}

void ReplayDock::refreshSceneList()
{
	scene_list_updating = true;

	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	const PluginSettings &settings = PluginSettings::instance();
	for (int camera = 0; camera < kCameraCount; ++camera) {
		QComboBox *combo = camera_combos[static_cast<size_t>(camera)];
		const CameraBinding &binding = settings.cameras[static_cast<size_t>(camera)];

		combo->clear();
		combo->addItem(obs_module_text("Replay.Angles.NoScene"), QString());

		int selected = 0;
		for (size_t index = 0; index < scenes.sources.num; ++index) {
			obs_source_t *scene = scenes.sources.array[index];
			const char *name = obs_source_get_name(scene);
			const char *uuid = obs_source_get_uuid(scene);
			if (!name || strcmp(name, "Instant Replay") == 0)
				continue;

			combo->addItem(QString::fromUtf8(name), QString::fromUtf8(uuid ? uuid : ""));
			/* uuid first, name as the fallback when the collection was re-created. */
			if ((!binding.uuid.empty() && uuid && binding.uuid == uuid) ||
			    (selected == 0 && !binding.name.empty() && binding.name == name))
				selected = combo->count() - 1;
		}
		combo->setCurrentIndex(selected);
		camera_checks[static_cast<size_t>(camera)]->setChecked(binding.enabled);
	}

	obs_frontend_source_list_free(&scenes);
	scene_list_updating = false;
	updateMemoryLabel();
}

void ReplayDock::onCameraSettingsChanged()
{
	if (scene_list_updating)
		return;

	PluginSettings &settings = PluginSettings::instance();
	for (int camera = 0; camera < kCameraCount; ++camera) {
		CameraBinding &binding = settings.cameras[static_cast<size_t>(camera)];
		const QComboBox *combo = camera_combos[static_cast<size_t>(camera)];
		binding.uuid = combo->currentData().toString().toStdString();
		binding.name = combo->currentIndex() > 0 ? combo->currentText().toStdString() : std::string();
		binding.enabled = camera_checks[static_cast<size_t>(camera)]->isChecked() && !binding.uuid.empty();
	}
	settings.camera_height = static_cast<uint32_t>(camera_height_combo->currentData().toInt());

	if (char *collection = obs_frontend_get_current_scene_collection()) {
		settings.scene_collection = collection;
		bfree(collection);
	}
	save_timer->start();

	/* Cameras never hold clips of their own, so restarting them costs nothing but a refill. */
	AngleManager::instance().start_cameras_from_settings();
	updateMemoryLabel();
}

void ReplayDock::updateMemoryLabel()
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || ovi.fps_den == 0) {
		memory_label->setText(QStringLiteral("—"));
		return;
	}

	const PluginSettings &settings = PluginSettings::instance();
	const double fps = static_cast<double>(ovi.fps_num) / ovi.fps_den;
	const double seconds = buffer_spin ? buffer_spin->value() : settings.buffer_seconds;
	const uint32_t divisor = std::max<uint32_t>(1, settings.frame_rate_divisor);

	uint64_t required = ring_bytes(ovi.output_width, ovi.output_height, ovi.output_format, fps, divisor, seconds);

	const uint32_t height = static_cast<uint32_t>(camera_height_combo->currentData().toInt());
	const uint32_t camera_height = std::min(height, ovi.base_height);
	const uint32_t camera_width = camera_width_for_height(ovi.base_width, ovi.base_height, camera_height);
	for (int camera = 0; camera < kCameraCount; ++camera) {
		if (camera_checks[static_cast<size_t>(camera)]->isChecked() &&
		    camera_combos[static_cast<size_t>(camera)]->currentIndex() > 0)
			required += ring_bytes(camera_width, camera_height, ovi.output_format, fps, divisor, seconds);
	}

	const uint64_t budget = AngleManager::memory_budget();
	const double gib = 1024.0 * 1024.0 * 1024.0;
	memory_label->setText(QStringLiteral("%1 %2 GB   ·   %3 %4 GB")
				      .arg(obs_module_text("Replay.Angles.Memory.Required"))
				      .arg(static_cast<double>(required) / gib, 0, 'f', 2)
				      .arg(obs_module_text("Replay.Angles.Memory.Budget"))
				      .arg(static_cast<double>(budget) / gib, 0, 'f', 2));

	/* Palette colours everywhere else; red is the one exception an operator must not miss. */
	memory_label->setStyleSheet(required > budget ? QStringLiteral("color: #e04040; font-weight: bold;")
						      : QString());
}

void ReplayDock::onAngleClicked(int angle)
{
	PlaybackEngine::instance().set_angle(angle);
}

void ReplayDock::selectAngle(int angle)
{
	if (QAbstractButton *button = angle_group->button(angle)) {
		if (!button->isEnabled())
			return;
		button->setChecked(true);
	}
	onAngleClicked(angle);
}

void ReplayDock::selectAngleProgram()
{
	selectAngle(kProgramAngle);
}

void ReplayDock::selectAngle1()
{
	selectAngle(1);
}

void ReplayDock::selectAngle2()
{
	selectAngle(2);
}

void ReplayDock::selectAngle3()
{
	selectAngle(3);
}

void ReplayDock::updateAngleButtons()
{
	AngleManager &angles = AngleManager::instance();
	const int selected = selectedEvent();
	const Clip *clip = (selected >= 0 && selected < static_cast<int>(events.size()))
				   ? &events[static_cast<size_t>(selected)].clip
				   : nullptr;

	if (QAbstractButton *program = angle_group->button(kProgramAngle))
		program->setToolTip(obs_module_text("Replay.Angle.Tip.Program"));

	for (int angle = 1; angle < kAngleCount; ++angle) {
		QAbstractButton *button = angle_group->button(angle);
		if (!button)
			continue;

		const CameraState state = angles.camera_state(angle - 1);
		const AngleCapture &capture = angles.angle(angle);

		bool available = capture.running();
		QString tip;

		if (!state.enabled) {
			tip = obs_module_text("Replay.Angle.Tip.NotConfigured");
		} else if (!available) {
			tip = QStringLiteral("%1 %2")
				      .arg(obs_module_text("Replay.Angle.Tip.NotRunning"))
				      .arg(QString::fromStdString(state.error));
		} else if (clip && clip->valid() && !(capture.covers(clip->ts_in) && capture.covers(clip->ts_out))) {
			/* A camera that was not buffering during the clip has nothing to show for it. */
			available = false;
			tip = obs_module_text("Replay.Angle.Tip.NoFootage");
		} else {
			tip = obs_module_text("Replay.Angle.Tip.Available");
		}

		button->setEnabled(available);
		button->setToolTip(tip);
	}

	/*
	 * Playback silently falls back to the programme when the chosen camera cannot serve the
	 * moment; keep the buttons telling the same story instead of leaving a dead angle selected.
	 */
	const int active = angles.active_angle();
	if (active != kProgramAngle) {
		const QAbstractButton *button = angle_group->button(active);
		if (!button || !button->isEnabled())
			angles.set_active_angle(kProgramAngle);
	}

	if (QAbstractButton *current = angle_group->button(angles.active_angle()))
		current->setChecked(true);
}

QWidget *ReplayDock::buildTimelineRow()
{
	auto *box = new QWidget(this);
	auto *layout = new QVBoxLayout(box);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(2);

	timeline = new ReplayTimeline(box);
	connect(timeline, &ReplayTimeline::selectionChanged, this, &ReplayDock::onTimelineChanged);

	timeline_label = new QLabel(obs_module_text("Replay.Timeline.Empty"), box);
	timeline_label->setEnabled(false);

	layout->addWidget(timeline);
	layout->addWidget(timeline_label);
	return box;
}

QWidget *ReplayDock::buildEventsBox()
{
	auto *box = new QGroupBox(obs_module_text("Replay.Events"), this);
	auto *layout = new QVBoxLayout(box);
	layout->setContentsMargins(6, 6, 6, 6);

	events_list = new QListWidget(box);
	events_list->setAlternatingRowColors(true);
	connect(events_list, &QListWidget::itemDoubleClicked, this, &ReplayDock::onEventActivated);
	connect(events_list, &QListWidget::currentRowChanged, this, &ReplayDock::onEventSelected);
	events_list->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(events_list, &QListWidget::customContextMenuRequested, this, &ReplayDock::onEventsContextMenu);
	layout->addWidget(events_list);

	auto *hint = new QLabel(obs_module_text("Replay.Events.Hint"), box);
	hint->setWordWrap(true);
	hint->setEnabled(false);
	layout->addWidget(hint);
	return box;
}

QWidget *ReplayDock::buildExportBox()
{
	auto *box = new QGroupBox(obs_module_text("Replay.Export"), this);
	auto *layout = new QVBoxLayout(box);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(4);

	const PluginSettings &settings = PluginSettings::instance();

	export_check = new QCheckBox(obs_module_text("Replay.Export.Enabled"), box);
	export_check->setChecked(settings.export_enabled);
	connect(export_check, &QCheckBox::toggled, this, &ReplayDock::onSettingsChanged);

	auto *folder_row = new QHBoxLayout();
	export_dir_edit = new QLineEdit(QString::fromStdString(settings.export_dir), box);
	export_dir_edit->setPlaceholderText(obs_module_text("Replay.Export.DefaultFolder"));
	connect(export_dir_edit, &QLineEdit::editingFinished, this, &ReplayDock::onSettingsChanged);
	auto *browse = new QPushButton(QStringLiteral("…"), box);
	browse->setFixedWidth(32);
	connect(browse, &QPushButton::clicked, this, [this] {
		const QString chosen = QFileDialog::getExistingDirectory(
			this, obs_module_text("Replay.Export.ChooseFolder"), export_dir_edit->text());
		if (!chosen.isEmpty()) {
			export_dir_edit->setText(chosen);
			onSettingsChanged();
		}
	});
	folder_row->addWidget(export_dir_edit, 1);
	folder_row->addWidget(browse);

	auto *encoder_row = new QHBoxLayout();
	encoder_row->addWidget(new QLabel(obs_module_text("Replay.Export.Encoder"), box));
	export_encoder_combo = new QComboBox(box);
	export_encoder_combo->addItem(obs_module_text("Replay.Export.Encoder.Auto"), QStringLiteral("auto"));
	export_encoder_combo->addItem(QStringLiteral("x264 (CPU)"), QStringLiteral("x264"));
	export_encoder_combo->addItem(QStringLiteral("NVENC (NVIDIA GPU)"), QStringLiteral("nvenc"));
	export_encoder_combo->addItem(QStringLiteral("AMF (AMD GPU)"), QStringLiteral("amf"));
	export_encoder_combo->addItem(QStringLiteral("QSV (Intel GPU)"), QStringLiteral("qsv"));
	const int current = export_encoder_combo->findData(QString::fromStdString(settings.export_encoder));
	export_encoder_combo->setCurrentIndex(std::max(0, current));
	connect(export_encoder_combo, &QComboBox::currentIndexChanged, this, &ReplayDock::onSettingsChanged);
	encoder_row->addWidget(export_encoder_combo, 1);

	layout->addWidget(export_check);
	layout->addLayout(folder_row);
	layout->addLayout(encoder_row);
	return box;
}

QWidget *ReplayDock::buildTransportRow()
{
	auto *box = new QWidget(this);
	auto *outer = new QVBoxLayout(box);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(6);

	auto *row = new QHBoxLayout();
	play_button = new QPushButton(obs_module_text("Replay.Play"), box);
	play_button->setMinimumHeight(40);
	stop_button = new QPushButton(obs_module_text("Replay.Stop"), box);
	stop_button->setMinimumHeight(40);
	connect(play_button, &QPushButton::clicked, this, &ReplayDock::onPlay);
	connect(stop_button, &QPushButton::clicked, this, &ReplayDock::onStop);

	row->addWidget(play_button, 3);
	row->addWidget(stop_button, 1);

	auto_return_check = new QCheckBox(obs_module_text("Replay.AutoReturn"), box);
	auto_return_check->setChecked(PluginSettings::instance().auto_return);
	connect(auto_return_check, &QCheckBox::toggled, this, &ReplayDock::onSettingsChanged);

	outer->addLayout(row);
	outer->addWidget(auto_return_check);
	return box;
}

void ReplayDock::onMark()
{
	Clip clip;
	std::string error;
	if (!PlaybackEngine::instance().mark(length_spin->value(), offset_spin->value(), clip, error)) {
		obs_log(LOG_WARNING, "MARK failed: %s", error.c_str());
		format_label->setText(QString::fromStdString(error));
		return;
	}

	Event event;
	event.clip = clip;
	event.name = QStringLiteral("%1 %2").arg(obs_module_text("Replay.Event")).arg(events.size() + 1);
	if (PluginSettings::instance().export_enabled) {
		ExportOptions options;
		options.encoder = PluginSettings::instance().export_encoder;
		options.crf = PluginSettings::instance().export_crf;
		options.base_dir = PluginSettings::instance().export_dir;
		event.export_job = ClipExporter::instance().enqueue(clip, event.name.toStdString(), options);
	}

	events.push_back(event);

	auto *item = new QListWidgetItem();
	item->setData(Qt::UserRole, static_cast<int>(events.size()) - 1);
	events_list->addItem(item);
	updateEventItem(static_cast<int>(events.size()) - 1);
	events_list->setCurrentItem(item);
	showSelection(static_cast<int>(events.size()) - 1);

	obs_log(LOG_INFO, "marked clip %.1f s (%llu frames)", clip.duration_sec(),
		static_cast<unsigned long long>(clip.seq_out - clip.seq_in));
}

void ReplayDock::updateEventItem(int event_index)
{
	if (event_index < 0 || event_index >= static_cast<int>(events.size()))
		return;

	for (int row = 0; row < events_list->count(); ++row) {
		QListWidgetItem *item = events_list->item(row);
		if (item->data(Qt::UserRole).toInt() != event_index)
			continue;

		const Event &event = events[static_cast<size_t>(event_index)];
		QString text = QStringLiteral("%1   %2 s").arg(event.name).arg(event.clip.duration_sec(), 0, 'f', 1);
		if (event.export_job > 0) {
			const ExportStatus status = ClipExporter::instance().status(event.export_job);
			switch (status.state) {
			case ExportState::Queued:
				text += QStringLiteral("   ● %1").arg(obs_module_text("Replay.Export.Queued"));
				break;
			case ExportState::Encoding: {
				const int percent =
					status.frames_total
						? static_cast<int>(status.frames_done * 100 / status.frames_total)
						: 0;
				text += QStringLiteral("   ● %1 %2%")
						.arg(obs_module_text("Replay.Export.Saving"))
						.arg(percent);
				break;
			}
			case ExportState::Done:
				text += QStringLiteral("   ✓ %1").arg(obs_module_text("Replay.Export.Saved"));
				break;
			case ExportState::DoneTruncated:
				text += QStringLiteral("   ✓ %1").arg(obs_module_text("Replay.Export.SavedTruncated"));
				break;
			case ExportState::Failed:
				text += QStringLiteral("   ✗ %1").arg(QString::fromStdString(status.error));
				break;
			}
			item->setToolTip(QString::fromStdString(status.path));
		}
		item->setText(text);
		return;
	}
}

void ReplayDock::showSelection(int event_index)
{
	if (event_index < 0 || event_index >= static_cast<int>(events.size())) {
		timeline->clearSelection();
		timeline_label->setText(obs_module_text("Replay.Timeline.Empty"));
		return;
	}

	uint64_t live = 0;
	if (!PlaybackEngine::live_timestamp(live))
		return;

	const Clip &clip = events[static_cast<size_t>(event_index)].clip;
	/* The timeline axis is "seconds before the live edge", so the clip drifts left as it ages. */
	const double in_sec = live > clip.ts_in ? static_cast<double>(live - clip.ts_in) / 1e9 : 0.0;
	const double out_sec = live > clip.ts_out ? static_cast<double>(live - clip.ts_out) / 1e9 : 0.0;
	timeline->setSelection(in_sec, out_sec);

	timeline_label->setText(QStringLiteral("IN -%1 s   OUT -%2 s   %3 %4 s")
					.arg(in_sec, 0, 'f', 1)
					.arg(out_sec, 0, 'f', 1)
					.arg(obs_module_text("Replay.Timeline.Duration"))
					.arg(clip.duration_sec(), 0, 'f', 1));
}

void ReplayDock::rebuildEventList()
{
	events_list->clear();
	for (size_t index = 0; index < events.size(); ++index) {
		auto *item = new QListWidgetItem();
		item->setData(Qt::UserRole, static_cast<int>(index));
		events_list->addItem(item);
		updateEventItem(static_cast<int>(index));
	}
}

void ReplayDock::onEventsContextMenu(const QPoint &position)
{
	if (selectedEvent() < 0)
		return;

	QMenu menu(this);
	QAction *rename = menu.addAction(obs_module_text("Replay.Event.Rename"));
	QAction *remove = menu.addAction(obs_module_text("Replay.Event.Delete"));

	const QAction *chosen = menu.exec(events_list->mapToGlobal(position));
	if (chosen == rename)
		renameSelectedEvent();
	else if (chosen == remove)
		deleteSelectedEvent();
}

void ReplayDock::renameSelectedEvent()
{
	const int index = selectedEvent();
	if (index < 0 || index >= static_cast<int>(events.size()))
		return;

	bool accepted = false;
	const QString name = QInputDialog::getText(this, obs_module_text("Replay.Event.Rename"),
						   obs_module_text("Replay.Event.Name"), QLineEdit::Normal,
						   events[static_cast<size_t>(index)].name, &accepted);
	if (!accepted || name.isEmpty())
		return;

	events[static_cast<size_t>(index)].name = name;
	updateEventItem(index);
}

void ReplayDock::deleteSelectedEvent()
{
	const int index = selectedEvent();
	if (index < 0 || index >= static_cast<int>(events.size()))
		return;

	events.erase(events.begin() + index);

	/* Item payloads are plain indices, so the whole list is rebuilt after a removal. */
	rebuildEventList();
	showSelection(selectedEvent());
}

void ReplayDock::onEventSelected()
{
	showSelection(selectedEvent());
}

void ReplayDock::onTimelineChanged(double in_sec, double out_sec)
{
	const int index = selectedEvent();
	if (index < 0 || index >= static_cast<int>(events.size()))
		return;

	uint64_t live = 0;
	if (!PlaybackEngine::live_timestamp(live))
		return;

	const uint64_t ts_in = live - static_cast<uint64_t>(in_sec * 1e9);
	const uint64_t ts_out = live - static_cast<uint64_t>(out_sec * 1e9);

	Clip trimmed;
	std::string error;
	if (!PlaybackEngine::instance().clip_from_timestamps(ts_in, ts_out, trimmed, error)) {
		timeline_label->setText(QString::fromStdString(error));
		return;
	}

	events[static_cast<size_t>(index)].clip = trimmed;
	updateEventItem(index);

	timeline_label->setText(QStringLiteral("IN -%1 s   OUT -%2 s   %3 %4 s")
					.arg(in_sec, 0, 'f', 1)
					.arg(out_sec, 0, 'f', 1)
					.arg(obs_module_text("Replay.Timeline.Duration"))
					.arg(trimmed.duration_sec(), 0, 'f', 1));
}

int ReplayDock::selectedEvent() const
{
	const QListWidgetItem *item = events_list->currentItem();
	if (!item)
		return events.empty() ? -1 : static_cast<int>(events.size()) - 1;

	return item->data(Qt::UserRole).toInt();
}

bool ReplayDock::play(int event_index)
{
	if (event_index < 0 || event_index >= static_cast<int>(events.size()))
		return false;

	if (!ReplayDirector::instance().play_to_program(events[static_cast<size_t>(event_index)].clip,
							speed_percent / 100.0, auto_return_check->isChecked())) {
		obs_log(LOG_WARNING, "PLAY failed: clip is no longer valid");
		return false;
	}

	obs_log(LOG_INFO, "playing clip %d at %d%%", event_index + 1, speed_percent);
	return true;
}

void ReplayDock::onPlay()
{
	play(selectedEvent());
}

void ReplayDock::onEventActivated()
{
	play(selectedEvent());
}

void ReplayDock::onStop()
{
	ReplayDirector::instance().stop();
	obs_log(LOG_INFO, "playback stopped");
}

void ReplayDock::cycleSpeed()
{
	const int order[] = {25, 50, 75, 100};
	int next = order[0];
	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
		if (order[i] == speed_percent) {
			next = order[(i + 1) % (sizeof(order) / sizeof(order[0]))];
			break;
		}
	}

	if (QAbstractButton *button = speed_group->button(next))
		button->setChecked(true);

	onSpeedChanged(next);
}

void ReplayDock::clampClipLength()
{
	const double limit = std::max(1.0, buffer_spin->value() - offset_spin->value() - kExportGuardSec);
	length_spin->setMaximum(limit);
	length_spin->setToolTip(
		QStringLiteral("%1 %2 s").arg(obs_module_text("Replay.Length.Limit")).arg(limit, 0, 'f', 1));
}

void ReplayDock::onSettingsChanged()
{
	clampClipLength();

	PluginSettings &settings = PluginSettings::instance();
	settings.clip_length_sec = length_spin->value();
	settings.clip_trim_sec = offset_spin->value();
	settings.speed_percent = speed_percent;
	settings.auto_return = auto_return_check->isChecked();
	if (export_check) {
		settings.export_enabled = export_check->isChecked();
		settings.export_dir = export_dir_edit->text().trimmed().toStdString();
		settings.export_encoder = export_encoder_combo->currentData().toString().toStdString();
	}
	save_timer->start();
}

void ReplayDock::applyBufferSetting()
{
	PluginSettings &settings = PluginSettings::instance();
	settings.buffer_seconds = buffer_spin->value();
	clampClipLength();
	settings.clip_length_sec = length_spin->value();
	save_timer->start();

	updateMemoryLabel();

	if (!AngleManager::instance().program_running())
		return;

	/* Reallocating the rings throws away everything buffered so far, including marked clips. */
	ReplayDirector::instance().reset();
	events.clear();
	rebuildEventList();
	showSelection(-1);

	AngleManager::instance().stop_all();
	AngleManager::instance().start_from_settings();
}

void ReplayDock::onSpeedChanged(int percent)
{
	speed_percent = percent;
	onSettingsChanged();

	/* Changing speed mid-playback is the whole point of the panel, so apply it right away. */
	PlaybackEngine::instance().set_speed(percent / 100.0);
}

void ReplayDock::refreshStatus()
{
	/* Export progress lives in the list items; a handful of rows at 10 Hz is cheap to repaint. */
	for (size_t index = 0; index < events.size(); ++index) {
		if (events[index].export_job > 0)
			updateEventItem(static_cast<int>(index));
	}

	AngleManager &angles = AngleManager::instance();
	angles.poll();
	ReplayDirector::instance().poll();
	updateAngleButtons();

	QString cameras;
	for (int camera = 0; camera < kCameraCount; ++camera) {
		const CameraState state = angles.camera_state(camera);
		if (!state.enabled)
			continue;
		if (!cameras.isEmpty())
			cameras += QStringLiteral("   ·   ");
		cameras += QStringLiteral("%1 %2: ").arg(obs_module_text("Replay.Angles.Camera")).arg(camera + 1);
		if (state.running) {
			const CaptureStatus camera_status = angles.angle(camera + 1).status();
			cameras += QStringLiteral("%1×%2 %3 %4 s")
					   .arg(camera_status.width)
					   .arg(camera_status.height)
					   .arg(camera_status.paused ? obs_module_text("Replay.Status.Paused")
								     : obs_module_text("Replay.Status.Recording"))
					   .arg(std::min(camera_status.buffered_sec, camera_status.capacity_sec), 0,
						'f', 1);
			if (camera_status.slow_frames > 0)
				cameras += QStringLiteral(" ⚠%1").arg(camera_status.slow_frames);
		} else {
			cameras += QString::fromStdString(state.error.empty() ? std::string("—") : state.error);
		}
	}
	if (cameras.isEmpty())
		cameras = obs_module_text("Replay.Angles.NoneHint");
	cameras_label->setText(cameras);

	const CaptureStatus status = angles.program().status();

	if (!status.running) {
		buffer_bar->setValue(0);
		buffer_label->setText(QStringLiteral("—"));
		format_label->setText(status.error.empty() ? obs_module_text("Replay.Status.NoBuffer")
							   : QString::fromStdString(status.error));
		return;
	}

	PlaybackEngine &playback = PlaybackEngine::instance();
	const bool replaying = playback.playing();
	onair_label->setText(replaying ? obs_module_text("Replay.OnAir.Replay") : obs_module_text("Replay.OnAir.Live"));

	const double capacity = status.capacity_sec > 0.0 ? status.capacity_sec : 1.0;
	const double filled = std::min(status.buffered_sec, capacity);
	buffer_bar->setValue(static_cast<int>(filled / capacity * 1000.0));
	buffer_label->setText(QStringLiteral("%1 / %2 s").arg(filled, 0, 'f', 1).arg(capacity, 0, 'f', 1));

	QString details = QStringLiteral("%1×%2 @ %3   %4 GB   %5")
				  .arg(status.width)
				  .arg(status.height)
				  .arg(status.fps, 0, 'f', 0)
				  .arg(static_cast<double>(status.bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2)
				  .arg(status.paused ? obs_module_text("Replay.Status.Paused")
						     : obs_module_text("Replay.Status.Recording"));

	/* Copy cost matters: anything slow here shows up as encoder lag during the broadcast. */
	details += QStringLiteral("   %1 %2/%3 ms")
			   .arg(obs_module_text("Replay.Status.Copy"))
			   .arg(status.copy_ms_avg, 0, 'f', 2)
			   .arg(status.copy_ms_max, 0, 'f', 2);

	timeline->setBuffer(capacity, filled);
	timeline->setPlayhead(replaying ? playback.position_sec() : -1.0);

	if (replaying) {
		const Clip clip = playback.clip();
		details += QStringLiteral("   ▶ %1 / %2 s  %3%")
				   .arg(playback.position_sec(), 0, 'f', 1)
				   .arg(clip.duration_sec(), 0, 'f', 1)
				   .arg(playback.speed() * 100.0, 0, 'f', 0);
	}

	if (status.slow_frames > 0)
		details += QStringLiteral("   ⚠ %1: %2")
				   .arg(obs_module_text("Replay.Status.SlowFrames"))
				   .arg(status.slow_frames);

	format_label->setText(details);
}
