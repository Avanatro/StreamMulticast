/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalCanvasDock.hpp"

#include "CanvasPreview.hpp"
#include "VerticalItemList.hpp"
#include "VerticalSceneLink.hpp"
#include "VerticalSceneList.hpp"
#include "../pipeline/Autosave.hpp"
#include "../pipeline/OwnedCanvas.hpp"
#include "../pipeline/VerticalRecorder.hpp"
#include "../pipeline/VerticalReplayBuffer.hpp"

#include "../core/SafeZone.hpp"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPointer>
#include <QStyle>
#include <QVBoxLayout>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>

namespace smulti {

VerticalCanvasDock::VerticalCanvasDock(OwnedCanvas &canvas, VerticalRecorder &recorder, VerticalReplayBuffer &replay,
				       QWidget *parent)
	: QWidget(parent),
	  m_canvas(canvas),
	  m_recorder(recorder),
	  m_replay(replay)
{
	setObjectName("StreamMulticastVerticalDock");
	setMinimumWidth(240);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	/* Parent set at the end of the constructor: QObject deletes children in order, so the selection goes
	 * last, after the preview and the list that reference it. */
	m_selection = new VerticalSelection();

	/* Safe-zone toggle above the preview. Preview only; the choice is a per-user UI flag. */
	auto *zone_row = new QHBoxLayout();
	auto *zone_label = new QLabel(tr("Safe zone:"), this);
	m_safe_zone = new QComboBox(this);
	m_safe_zone->addItem(tr("Off"), QString::fromLatin1(safe_zone::profile_to_string(safe_zone::Profile::Off)));
	m_safe_zone->addItem(tr("TikTok (approx.)"),
			     QString::fromLatin1(safe_zone::profile_to_string(safe_zone::Profile::TikTok)));
	m_safe_zone->addItem(tr("Shorts (approx.)"),
			     QString::fromLatin1(safe_zone::profile_to_string(safe_zone::Profile::Shorts)));
	const QString zone_tip = tr("Approximate areas covered by the app UI. Preview only, never recorded or streamed.");
	zone_label->setToolTip(zone_tip);
	m_safe_zone->setToolTip(zone_tip);
	zone_row->addWidget(zone_label);
	zone_row->addWidget(m_safe_zone, 1);
	layout->addLayout(zone_row);

	m_preview = new CanvasPreview(canvas, *m_selection, this);
	layout->addWidget(m_preview, 3);

	{
		const char *saved = nullptr;
		if (config_t *cfg = obs_frontend_get_user_config())
			saved = config_get_string(cfg, "StreamMulticast", "SafeZoneProfile");
		const safe_zone::Profile start = safe_zone::profile_from_string(saved ? saved : "");
		const int idx = m_safe_zone->findData(QString::fromLatin1(safe_zone::profile_to_string(start)));
		m_safe_zone->setCurrentIndex(idx < 0 ? 0 : idx);
		m_preview->set_safe_zone(start);
	}
	connect(m_safe_zone, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		const std::string key = m_safe_zone->currentData().toString().toStdString();
		m_preview->set_safe_zone(safe_zone::profile_from_string(key));
		if (config_t *cfg = obs_frontend_get_user_config()) {
			config_set_string(cfg, "StreamMulticast", "SafeZoneProfile", key.c_str());
			config_save_safe(cfg, "tmp", nullptr);
		}
	});

	/* No colours here: checked state and the theme's text-danger class mark a running recording. */
	m_record = new QPushButton(tr("Record vertical"), this);
	m_record->setCheckable(true);
	connect(m_record, &QPushButton::clicked, this, [this]() {
		m_recorder.toggle();
		update_record_ui(); // a refused start leaves the button unchecked
	});
	layout->addWidget(m_record);

	m_tabs = new QTabWidget(this);
	m_scenes = new VerticalSceneList(canvas, m_tabs);
	m_tabs->addTab(m_scenes, tr("Scenes"));
	m_items = new VerticalItemList(canvas, *m_selection, m_tabs);
	m_tabs->addTab(m_items, tr("Sources"));
	m_links = new VerticalSceneLink(canvas, m_tabs);
	m_tabs->addTab(m_links, tr("Links"));
	m_tabs->addTab(build_record_tab(), tr("Record"));
	m_tabs->setTabToolTip(0, tr("\"Set active\" is overridden by the next main scene change while links are on."));
	layout->addWidget(m_tabs, 2);

	/* Preview editing goes through the list's scene access (same reference and INV-6 rules). Both are
	 * children of this dock; the list refuses access after shutdown. */
	m_preview->set_scene_access([this](const SceneFn &fn) { return m_items && m_items->with_active_scene(fn); });
	m_preview->set_context_menu_handler([this](const QPoint &pos, int64_t id, const std::string &scene_uuid) {
		if (m_items)
			m_items->show_item_menu(pos, id, scene_uuid);
	});
	m_preview->set_edit_listener([this]() {
		request_autosave("preview edit"); // once per finished drag / nudge / undo, never per mouse move
		if (m_items)
			m_items->reload_editor(true);
	});
	m_items->set_change_listener([this]() {
		if (m_preview)
			m_preview->refresh_overlay();
	});
	m_selection->setParent(this);

	/* The recorder calls back on the UI thread; the dock may be destroyed by Qt first at OBS exit. */
	QPointer<VerticalCanvasDock> self(this);

	/* A scene switch changes the transition's child, not channel 0, so no canvas channel_change fires:
	 * OwnedCanvas reports it (UI thread). Queued, so a list never rebuilds inside its own action. */
	m_canvas.set_active_scene_listener([self]() {
		if (!self)
			return;
		QMetaObject::invokeMethod(
			self.data(),
			[self]() {
				if (!self || self->m_shut_down)
					return;
				if (self->m_scenes)
					self->m_scenes->refresh();
				if (self->m_items)
					self->m_items->refresh();
			},
			Qt::QueuedConnection);
	});
	m_recorder.set_state_listener([self]() {
		if (self)
			self->update_record_ui();
	});
	m_replay.set_state_listener([self]() {
		if (self)
			self->update_replay_ui();
	});
	update_record_ui();
	update_replay_ui();
}

QWidget *VerticalCanvasDock::build_record_tab()
{
	auto *tab = new QWidget(m_tabs);
	auto *v = new QVBoxLayout(tab);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(4);

	const VerticalRecordSettings s = m_recorder.settings();

	m_record_with_main = new QCheckBox(tr("Record vertical together with main recording"), tab);
	m_record_with_main->setChecked(s.record_with_main);
	m_record_with_main->setToolTip(tr("Starting the OBS recording also starts the vertical recording; stopping it "
					  "stops a vertical recording that was started this way."));
	v->addWidget(m_record_with_main);

	auto *row = new QHBoxLayout();
	m_record_folder = new QLineEdit(QString::fromStdString(s.folder), tab);
	m_record_folder->setPlaceholderText(tr("Default: main recording folder/Vertical"));
	auto *browse = new QPushButton(tr("Browse..."), tab);
	row->addWidget(m_record_folder, 1);
	row->addWidget(browse);
	v->addLayout(row);

	m_record_target = new QLabel(tab);
	m_record_target->setWordWrap(true);
	v->addWidget(m_record_target);

	auto *hint = new QLabel(tr("MKV, 1080x1920 at the canvas fps, NVENC HEVC CQP 20 (x264 CRF 20 without NVENC), audio "
				   "track 1. Remuxed to MP4 when the profile has automatic remux on. Never written to C:."),
				tab);
	hint->setWordWrap(true);
	v->addWidget(hint);
	v->addWidget(build_replay_group(tab));
	v->addStretch(1);

	connect(m_record_with_main, &QCheckBox::toggled, this, [this](bool) { on_record_settings_changed(); });
	connect(m_record_folder, &QLineEdit::editingFinished, this, [this]() { on_record_settings_changed(); });
	connect(browse, &QPushButton::clicked, this, [this]() {
		const QString dir = QFileDialog::getExistingDirectory(this, tr("Vertical recording folder"),
								      m_record_folder->text());
		if (!dir.isEmpty()) {
			m_record_folder->setText(dir);
			on_record_settings_changed();
		}
	});
	return tab;
}

void VerticalCanvasDock::on_record_settings_changed()
{
	VerticalRecordSettings s = m_recorder.settings();
	s.record_with_main = m_record_with_main->isChecked();
	s.folder = m_record_folder->text().trimmed().toStdString();
	m_recorder.set_settings(s);
	update_record_ui();
}

QWidget *VerticalCanvasDock::build_replay_group(QWidget *parent)
{
	auto *group = new QGroupBox(tr("Vertical replay buffer"), parent);
	auto *v = new QVBoxLayout(group);
	v->setSpacing(4);

	const VerticalReplaySettings s = m_recorder.settings().replay;

	m_replay_with_main = new QCheckBox(tr("Run with the main replay buffer"), group);
	m_replay_with_main->setChecked(s.enabled_with_main_replay);
	m_replay_with_main->setToolTip(tr("Starting the OBS replay buffer also starts the vertical one; stopping it stops a "
					  "vertical buffer that was started this way."));
	v->addWidget(m_replay_with_main);

	auto *form = new QFormLayout();
	m_replay_seconds = new QSpinBox(group);
	m_replay_seconds->setRange(kVerticalReplayMinSeconds, kVerticalReplayMaxSeconds);
	m_replay_seconds->setValue(s.seconds);
	form->addRow(tr("Length (s)"), m_replay_seconds);
	m_replay_max_mb = new QSpinBox(group);
	m_replay_max_mb->setRange(kVerticalReplayMinSizeMb, kVerticalReplayMaxSizeMb);
	m_replay_max_mb->setSingleStep(64);
	m_replay_max_mb->setValue(s.max_size_mb);
	form->addRow(tr("Max memory (MB)"), m_replay_max_mb);
	v->addLayout(form);

	auto *row = new QHBoxLayout();
	m_replay_folder = new QLineEdit(QString::fromStdString(s.folder), group);
	m_replay_folder->setPlaceholderText(tr("Default: main recording folder/Vertical"));
	auto *browse = new QPushButton(tr("Browse..."), group);
	row->addWidget(m_replay_folder, 1);
	row->addWidget(browse);
	v->addLayout(row);

	m_replay_toggle = new QPushButton(tr("Start vertical replay buffer"), group);
	m_replay_toggle->setToolTip(tr("Save a clip with the hotkeys \"StreamMulticast: Save vertical clip\" or "
				       "\"StreamMulticast: Save main + vertical clip\" (Settings > Hotkeys). Length, "
				       "memory and folder apply at the next start."));
	v->addWidget(m_replay_toggle);

	m_replay_status = new QLabel(group);
	m_replay_status->setWordWrap(true);
	v->addWidget(m_replay_status);
	m_replay_memory = new QLabel(group);
	m_replay_memory->setWordWrap(true);
	v->addWidget(m_replay_memory);

	connect(m_replay_with_main, &QCheckBox::toggled, this, [this](bool) { on_replay_settings_changed(); });
	connect(m_replay_seconds, &QSpinBox::editingFinished, this, [this]() { on_replay_settings_changed(); });
	connect(m_replay_max_mb, &QSpinBox::editingFinished, this, [this]() { on_replay_settings_changed(); });
	connect(m_replay_folder, &QLineEdit::editingFinished, this, [this]() { on_replay_settings_changed(); });
	connect(browse, &QPushButton::clicked, this, [this]() {
		const QString dir =
			QFileDialog::getExistingDirectory(this, tr("Vertical clip folder"), m_replay_folder->text());
		if (!dir.isEmpty()) {
			m_replay_folder->setText(dir);
			on_replay_settings_changed();
		}
	});
	connect(m_replay_toggle, &QPushButton::clicked, this, [this]() {
		m_replay.toggle();
		update_replay_ui(); // a refused start keeps the button on "Start"
	});
	return group;
}

void VerticalCanvasDock::on_replay_settings_changed()
{
	VerticalRecordSettings s = m_recorder.settings();
	s.replay.enabled_with_main_replay = m_replay_with_main->isChecked();
	s.replay.seconds = m_replay_seconds->value();
	s.replay.max_size_mb = m_replay_max_mb->value();
	s.replay.folder = m_replay_folder->text().trimmed().toStdString();
	m_recorder.set_settings(s);
	update_replay_ui();
}

void VerticalCanvasDock::update_replay_ui()
{
	if (!m_replay_toggle)
		return;
	const VerticalReplayState state = m_replay.state();
	m_replay_toggle->setEnabled(state != VerticalReplayState::Stopping);
	m_replay_toggle->setText(state == VerticalReplayState::Stopping    ? tr("Stopping vertical replay buffer...")
				 : state == VerticalReplayState::Buffering ? tr("Stop vertical replay buffer")
									   : tr("Start vertical replay buffer"));

	const VerticalReplaySettings s = m_recorder.settings().replay;
	const VerticalRecordFolder folder = m_recorder.resolve_replay_folder();
	const std::string clip = m_replay.last_clip();
	const std::string error = m_replay.last_error();
	QString text;
	if (!folder.ok)
		text = QString::fromStdString(folder.reason); // the recording's refusal text (never C:)
	else if (state == VerticalReplayState::Buffering)
		text = tr("Buffering the last %1 s for %2").arg(s.seconds).arg(QString::fromStdString(folder.folder));
	else
		text = tr("Not buffering. Clips go to: %1").arg(QString::fromStdString(folder.folder));
	if (!clip.empty())
		text += QStringLiteral("\n") + tr("Last clip: %1").arg(QString::fromStdString(clip));
	if (!error.empty())
		text += QStringLiteral("\n") + tr("Last error: %1").arg(QString::fromStdString(error));
	m_replay_status->setText(text);
	m_replay_status->setProperty("class", folder.ok && error.empty() ? "" : "text-danger");
	m_replay_status->style()->unpolish(m_replay_status);
	m_replay_status->style()->polish(m_replay_status);
	m_replay_memory->setText(tr("Uses about %1 MB of RAM while buffering").arg(s.max_size_mb));
}

void VerticalCanvasDock::update_record_ui()
{
	if (!m_record)
		return;
	const VerticalRecordState state = m_recorder.state();
	const bool recording = state != VerticalRecordState::Idle;

	m_record->blockSignals(true);
	m_record->setChecked(recording);
	m_record->blockSignals(false);
	m_record->setEnabled(state != VerticalRecordState::Stopping);
	m_record->setText(state == VerticalRecordState::Stopping ? tr("Stopping vertical recording...")
				  : recording				  ? tr("Stop vertical recording")
									  : tr("Record vertical"));
	/* Theme class (OBS themes define .text-danger); re-polish so the style follows the property. */
	m_record->setProperty("class", recording ? "text-danger" : "");
	m_record->style()->unpolish(m_record);
	m_record->style()->polish(m_record);

	const std::string file = m_recorder.last_file();
	const std::string error = m_recorder.last_error();
	m_record->setToolTip(!error.empty() ? QString::fromStdString(error)
			     : recording    ? tr("Recording to %1").arg(QString::fromStdString(file))
					    : tr("Record the vertical canvas to a file (hotkey: StreamMulticast: "
						 "Start/Stop vertical recording)"));

	if (m_record_target) {
		const VerticalRecordFolder folder = m_recorder.resolve_folder();
		QString text = folder.ok ? tr("Records to: %1").arg(QString::fromStdString(folder.folder))
					 : QString::fromStdString(folder.reason);
		if (!error.empty())
			text += QStringLiteral("\n") + tr("Last error: %1").arg(QString::fromStdString(error));
		m_record_target->setText(text);
		m_record_target->setProperty("class", folder.ok && error.empty() ? "" : "text-danger");
		m_record_target->style()->unpolish(m_record_target);
		m_record_target->style()->polish(m_record_target);
	}
}

void VerticalCanvasDock::flush_preview()
{
	if (m_preview)
		m_preview->flush();
}

void VerticalCanvasDock::refresh_scenes()
{
	if (m_scenes)
		m_scenes->refresh();
	if (m_items)
		m_items->refresh();
	refresh_links();
}

void VerticalCanvasDock::refresh_links()
{
	if (m_links)
		m_links->refresh();
}

void VerticalCanvasDock::detach_scene_signals()
{
	if (m_scenes)
		m_scenes->detach_signals();
	if (m_items)
		m_items->detach_signals();
}

void VerticalCanvasDock::shutdown_preview()
{
	m_shut_down = true;
	m_canvas.set_active_scene_listener(nullptr);
	if (m_scenes)
		m_scenes->shutdown();
	if (m_items)
		m_items->shutdown();
	if (m_links)
		m_links->shutdown();
	if (m_preview)
		m_preview->shutdown();
}

} // namespace smulti
