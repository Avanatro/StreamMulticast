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
#include "../pipeline/VerticalRecorder.hpp"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QPointer>
#include <QStyle>
#include <QVBoxLayout>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>

namespace smulti {

VerticalCanvasDock::VerticalCanvasDock(OwnedCanvas &canvas, VerticalRecorder &recorder, QWidget *parent)
	: QWidget(parent),
	  m_recorder(recorder)
{
	setObjectName("StreamMulticastVerticalDock");
	setMinimumWidth(240);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	/* Parent set at the end of the constructor: QObject deletes children in order, so the selection goes
	 * last, after the preview and the list that reference it. */
	m_selection = new VerticalSelection();

	m_preview = new CanvasPreview(canvas, *m_selection, this);
	layout->addWidget(m_preview, 3);

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
	m_preview->set_edit_listener([this]() {
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
	m_recorder.set_state_listener([self]() {
		if (self)
			self->update_record_ui();
	});
	update_record_ui();
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
