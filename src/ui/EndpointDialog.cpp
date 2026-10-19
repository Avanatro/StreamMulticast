/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "EndpointDialog.hpp"
#include "../plugin-support.h"
#include "../core/ObsServiceImport.hpp"
#include "../core/TikTokBridgeImport.hpp"
#include "../core/EndpointValidation.hpp"
#include "../core/MainServiceTarget.hpp"
#include "../core/EndpointRegistry.hpp"
#include "../core/CanvasMigration.hpp"
#include "../pipeline/CanvasLookup.hpp"
#ifdef SMULTI_STREAMLABS_TIKTOK
#include "../core/StreamlabsTikTok.hpp"
#include "../pipeline/StreamlabsClient.hpp"
#include "../pipeline/StreamlabsTokenVault.hpp"
#include <QtCore/QPointer>
#endif

#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtCore/QFileInfo>

namespace smulti {

/* -----------------------------------------------------------------------
 * Static server URL templates
 * ----------------------------------------------------------------------- */
const std::vector<EndpointDialog::ServerTemplate> EndpointDialog::s_templates = {
	{ QStringLiteral("Custom RTMP"),           QStringLiteral("") },
	{ QStringLiteral("Twitch"),                QStringLiteral("rtmp://live.twitch.tv/app") },
	{ QStringLiteral("YouTube"),               QStringLiteral("rtmp://a.rtmp.youtube.com/live2") },
	{ QStringLiteral("Facebook"),              QStringLiteral("rtmps://live-api-s.facebook.com:443/rtmp") },
	{ QStringLiteral("TikTok"),                QStringLiteral("rtmp://push-rtmp.tiktokcdn.com/live") },
	{ QStringLiteral("Kick"),                  QStringLiteral("rtmp://ingest.global-contribute.live-video.net/app") },
	{ QStringLiteral("Trovo"),                 QStringLiteral("rtmp://livepush.trovo.live/push") },
};

/* -----------------------------------------------------------------------
 * Constructor
 * ----------------------------------------------------------------------- */
EndpointDialog::EndpointDialog(const Endpoint   &ep,
                               EndpointRegistry &registry,
                               QWidget          *parent)
	: QDialog(parent)
	, m_endpoint(ep)
	, m_result(ep)
	, m_registry(registry)
{
	setWindowTitle(tr("Endpoint Settings"));
	setModal(true);
	setMinimumWidth(480);

	m_available_backends = EncoderFactory::available_backends();

	setup_ui();
	populate_from_endpoint();
}

/* -----------------------------------------------------------------------
 * setup_ui
 * ----------------------------------------------------------------------- */
void EndpointDialog::setup_ui()
{
	auto *outer = new QVBoxLayout(this);
	outer->setSpacing(8);
	outer->setContentsMargins(12, 12, 12, 12);

	auto *form = new QFormLayout();
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	form->setHorizontalSpacing(12);
	form->setVerticalSpacing(6);

	/* Name */
	m_name_edit = new QLineEdit(this);
	m_name_edit->setPlaceholderText(tr("e.g. Twitch Main"));
	form->addRow(tr("Name:"), m_name_edit);

#ifdef SMULTI_STREAMLABS_TIKTOK
	/* Key source */
	m_key_source_cb = new QComboBox(this);
	m_key_source_cb->addItem(tr("Manual"), QString());
	m_key_source_cb->addItem(tr("Streamlabs TikTok"), QString::fromUtf8(kKeyProviderStreamlabsTikTok));
	m_key_source_cb->setToolTip(tr("Streamlabs TikTok fetches the TikTok server URL and stream key at go-live. "
	                               "It uses an unofficial Streamlabs API that may stop working."));
	form->addRow(tr("Key source:"), m_key_source_cb);
#endif

	/* Server template dropdown */
	m_template_cb = new QComboBox(this);
	for (const auto &tmpl : s_templates)
		m_template_cb->addItem(tmpl.label);
	form->addRow(tr("Template:"), m_template_cb);

	/* Server URL */
	m_server_edit = new QLineEdit(this);
	m_server_edit->setPlaceholderText(tr("rtmp://... or rtmps://..."));
	form->addRow(tr("Server URL:"), m_server_edit);

	/* Stream Key */
	auto *key_row = new QHBoxLayout();
	m_key_edit = new QLineEdit(this);
	m_key_edit->setEchoMode(QLineEdit::Password);
	m_key_edit->setPlaceholderText(tr("Stream key (stored plain)"));
	m_show_key_btn = new QPushButton(tr("Show"), this);
	m_show_key_btn->setCheckable(true);
	m_show_key_btn->setFixedWidth(48);
	key_row->addWidget(m_key_edit);
	key_row->addWidget(m_show_key_btn);
	form->addRow(tr("Stream Key:"), key_row);

	/* Encoder Backend */
	m_backend_cb = new QComboBox(this);
	for (auto backend : m_available_backends)
		m_backend_cb->addItem(
			QString::fromStdString(EncoderFactory::backend_label(backend)),
			static_cast<int>(backend)
		);
	form->addRow(tr("Encoder:"), m_backend_cb);

	/* Video Bitrate */
	m_bitrate_spin = new QSpinBox(this);
	m_bitrate_spin->setRange(500, 25000);
	m_bitrate_spin->setSingleStep(500);
	m_bitrate_spin->setSuffix(tr(" kbps"));
	form->addRow(tr("Video Bitrate:"), m_bitrate_spin);

	/* Keyframe Interval */
	m_keyint_spin = new QSpinBox(this);
	m_keyint_spin->setRange(1, 10);
	m_keyint_spin->setSuffix(tr(" s"));
	form->addRow(tr("Keyframe Interval:"), m_keyint_spin);

	/* Audio Bitrate */
	m_audio_cb = new QComboBox(this);
	const int audio_rates[] = {64, 96, 128, 160, 192, 320};
	for (int rate : audio_rates)
		m_audio_cb->addItem(QString("%1 kbps").arg(rate), rate);
	form->addRow(tr("Audio Bitrate:"), m_audio_cb);

	/* Video canvas: empty ref = OBS main canvas, otherwise uuid + name of a canvas */
	m_canvas_cb = new QComboBox(this);
	m_canvas_cb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	m_canvas_cb->setMinimumContentsLength(16);
	populate_canvas_combo();
	form->addRow(tr("Video canvas:"), m_canvas_cb);

	/* Linked to main stream */
	m_linked_cb = new QCheckBox(tr("Start/stop with OBS main stream"), this);
	form->addRow(QString(), m_linked_cb);

	outer->addLayout(form);

#ifdef SMULTI_STREAMLABS_TIKTOK
	setup_streamlabs_ui(outer);
#endif

	/* Status label (for test connection result) */
	m_status_label = new QLabel(this);
	m_status_label->setVisible(false);
	m_status_label->setWordWrap(true);
	outer->addWidget(m_status_label);

	/* Import buttons */
	auto *import_row = new QHBoxLayout();
	m_import_btn = new QPushButton(tr("Import from OBS"), this);
	m_import_btn->setToolTip(tr("Read server URL and stream key from the active "
	                            "OBS profile (whatever you've connected via OBS's "
	                            "native 'Connect Account' for Twitch/YouTube/etc.)"));
	m_tiktok_bridge_btn = new QPushButton(tr("Import TikTok Bridge"), this);
	m_tiktok_bridge_btn->setToolTip(tr("Read locally supplied TikTok RTMP data from "
	                                   "a Bridge JSON file. StreamMulticast does not "
	                                   "generate keys or perform TikTok login."));
	import_row->addWidget(m_import_btn);
	import_row->addWidget(m_tiktok_bridge_btn);
	import_row->addStretch();
	outer->addLayout(import_row);

	/* Dialog buttons */
	auto *btn_row = new QHBoxLayout();
	m_test_btn   = new QPushButton(tr("Test Connection"), this);
	m_save_btn   = new QPushButton(tr("Save"), this);
	m_cancel_btn = new QPushButton(tr("Cancel"), this);
	m_save_btn->setDefault(true);

	btn_row->addWidget(m_test_btn);
	btn_row->addStretch();
	btn_row->addWidget(m_save_btn);
	btn_row->addWidget(m_cancel_btn);
	outer->addLayout(btn_row);

	setLayout(outer);

	/* Connections */
	connect(m_template_cb, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, &EndpointDialog::on_template_selected);
	connect(m_show_key_btn, &QPushButton::toggled,
	        this, &EndpointDialog::on_show_key_toggled);
	connect(m_import_btn, &QPushButton::clicked, this, &EndpointDialog::on_import_from_obs);
	connect(m_tiktok_bridge_btn, &QPushButton::clicked, this, &EndpointDialog::on_import_tiktok_bridge);
	connect(m_test_btn,   &QPushButton::clicked, this, &EndpointDialog::on_test_connection);
	connect(m_save_btn,   &QPushButton::clicked, this, &EndpointDialog::on_save);
	connect(m_cancel_btn, &QPushButton::clicked, this, &QDialog::reject);
}

/* -----------------------------------------------------------------------
 * populate_from_endpoint — fill form widgets from m_endpoint
 * ----------------------------------------------------------------------- */
void EndpointDialog::populate_from_endpoint()
{
	m_name_edit->setText(QString::fromStdString(m_endpoint.name));
	m_server_edit->setText(QString::fromStdString(m_endpoint.server_url));
	m_key_edit->setText(QString::fromStdString(m_endpoint.stream_key));
	m_bitrate_spin->setValue(m_endpoint.video_bitrate_kbps);
	m_keyint_spin->setValue(m_endpoint.keyframe_interval_sec);
	m_linked_cb->setChecked(m_endpoint.linked_to_main);

#ifdef SMULTI_STREAMLABS_TIKTOK
	m_streamlabs_title_edit->setText(QString::fromStdString(m_endpoint.streamlabs_title));
	m_streamlabs_category_cb->clear();
	m_streamlabs_category_cb->addItem(m_endpoint.streamlabs_category_name.empty()
	                                          ? tr("Other")
	                                          : QString::fromStdString(m_endpoint.streamlabs_category_name),
	                                  QString::fromStdString(m_endpoint.streamlabs_category_id));
	m_key_source_cb->blockSignals(true);
	m_key_source_cb->setCurrentIndex(m_endpoint.key_provider == kKeyProviderStreamlabsTikTok ? 1 : 0);
	m_key_source_cb->blockSignals(false);
#endif

	/* Backend combo */
	for (int i = 0; i < m_backend_cb->count(); ++i) {
		if (m_backend_cb->itemData(i).toInt() == static_cast<int>(m_endpoint.encoder_backend)) {
			m_backend_cb->setCurrentIndex(i);
			break;
		}
	}

	/* Audio bitrate combo */
	for (int i = 0; i < m_audio_cb->count(); ++i) {
		if (m_audio_cb->itemData(i).toInt() == m_endpoint.audio_bitrate_kbps) {
			m_audio_cb->setCurrentIndex(i);
			break;
		}
	}

	/* Canvas combo */
	select_canvas(m_endpoint.canvas_uuid, m_endpoint.canvas_name);

	/* Try to match server URL to a template */
	m_template_cb->blockSignals(true);
	m_template_cb->setCurrentIndex(0); // "Custom RTMP"
	for (int i = 1; i < static_cast<int>(s_templates.size()); ++i) {
		if (m_server_edit->text().startsWith(s_templates[i].url)) {
			m_template_cb->setCurrentIndex(i);
			break;
		}
	}
	m_template_cb->blockSignals(false);

#ifdef SMULTI_STREAMLABS_TIKTOK
	apply_key_source();
#endif
}

/* -----------------------------------------------------------------------
 * Canvas selector
 * ----------------------------------------------------------------------- */
namespace {
constexpr int kCanvasUuidRole = Qt::UserRole + 1;
constexpr int kCanvasNameRole = Qt::UserRole + 2;
}

void EndpointDialog::populate_canvas_combo()
{
	m_canvas_cb->clear();

	/* Main canvas: both refs empty */
	m_canvas_cb->addItem(tr("Main canvas"));
	m_canvas_cb->setItemData(0, QString(), kCanvasUuidRole);
	m_canvas_cb->setItemData(0, QString(), kCanvasNameRole);

	m_canvases = snapshot_canvases();

	auto add_canvas = [this](const CanvasInfo &c) {
		const QString name = QString::fromStdString(c.name);
		m_canvas_cb->addItem(name);
		const int row = m_canvas_cb->count() - 1;
		m_canvas_cb->setItemData(row, QString::fromStdString(c.uuid), kCanvasUuidRole);
		m_canvas_cb->setItemData(row, name, kCanvasNameRole);
	};

	/* Owned canvas first after Main, then the others in enumeration order */
	for (const CanvasInfo &c : m_canvases)
		if (!c.is_main && c.name == kOwnedVerticalCanvasName)
			add_canvas(c);
	for (const CanvasInfo &c : m_canvases)
		if (!c.is_main && c.name != kOwnedVerticalCanvasName)
			add_canvas(c);
}

void EndpointDialog::select_canvas(const std::string &uuid, const std::string &name)
{
	if (uuid.empty() && name.empty()) {
		m_canvas_cb->setCurrentIndex(0);
		return;
	}

	/* A stored ref that points at the main canvas selects "Main canvas" */
	for (const CanvasInfo &c : m_canvases) {
		if (c.is_main && ((!uuid.empty() && c.uuid == uuid) ||
		                  (uuid.empty() && !name.empty() && c.name == name))) {
			m_canvas_cb->setCurrentIndex(0);
			return;
		}
	}

	const QString q_uuid = QString::fromStdString(uuid);
	const QString q_name = QString::fromStdString(name);

	/* uuid first, then name (same order as resolve_canvas) */
	int found = -1;
	if (!q_uuid.isEmpty()) {
		for (int i = 1; i < m_canvas_cb->count(); ++i) {
			if (m_canvas_cb->itemData(i, kCanvasUuidRole).toString() == q_uuid &&
			    !m_canvas_cb->itemData(i, kCanvasUuidRole).toString().isEmpty()) {
				found = i;
				break;
			}
		}
	}
	if (found < 0 && !q_name.isEmpty()) {
		for (int i = 1; i < m_canvas_cb->count(); ++i) {
			if (m_canvas_cb->itemData(i, kCanvasNameRole).toString() == q_name &&
			    !m_canvas_cb->itemData(i, kCanvasUuidRole).toString().isEmpty()) {
				found = i;
				break;
			}
		}
	}

	if (found < 0) {
		/* Reuse an earlier "(missing)" entry for the same ref */
		for (int i = 1; i < m_canvas_cb->count(); ++i) {
			if (m_canvas_cb->itemData(i, kCanvasUuidRole).toString() == q_uuid &&
			    m_canvas_cb->itemData(i, kCanvasNameRole).toString() == q_name) {
				found = i;
				break;
			}
		}
	}

	if (found < 0) {
		/* Canvas is not in the current list: keep the stored ref selectable */
		const QString shown = q_name.isEmpty() ? q_uuid : q_name;
		m_canvas_cb->addItem(tr("%1 (missing)").arg(shown));
		found = m_canvas_cb->count() - 1;
		m_canvas_cb->setItemData(found, q_uuid, kCanvasUuidRole);
		m_canvas_cb->setItemData(found, q_name, kCanvasNameRole);
	}
	m_canvas_cb->setCurrentIndex(found);
}

/* -----------------------------------------------------------------------
 * collect_from_form — read form widgets into an Endpoint
 * ----------------------------------------------------------------------- */
Endpoint EndpointDialog::collect_from_form() const
{
	Endpoint ep = m_endpoint; // preserve id and sort_order

	ep.name                 = m_name_edit->text().toStdString();
	ep.server_url           = m_server_edit->text().toStdString();
	ep.stream_key           = m_key_edit->text().toStdString();
	ep.video_bitrate_kbps   = m_bitrate_spin->value();
	ep.keyframe_interval_sec = m_keyint_spin->value();
	ep.audio_bitrate_kbps   = m_audio_cb->currentData().toInt();
	/* Framing is chosen by the canvas now; never write a legacy orientation back. */
	ep.orientation          = OutputOrientation::SourceMatch;
	ep.canvas_uuid          = m_canvas_cb->currentData(kCanvasUuidRole).toString().toStdString();
	ep.canvas_name          = m_canvas_cb->currentData(kCanvasNameRole).toString().toStdString();
	ep.linked_to_main       = m_linked_cb->isChecked();
	ep.encoder_backend      = static_cast<EncoderBackend>(
	                              m_backend_cb->currentData().toInt());
#ifdef SMULTI_STREAMLABS_TIKTOK
	ep.key_provider = streamlabs_selected() ? kKeyProviderStreamlabsTikTok : "";
	if (streamlabs_selected()) {
		/* The greyed fields are empty placeholders; keep the manual values as they were. */
		ep.server_url = m_manual_server.toStdString();
		ep.stream_key = m_manual_key.toStdString();
	}
	ep.streamlabs_title = m_streamlabs_title_edit->text().trimmed().toStdString();
	ep.streamlabs_category_id = m_streamlabs_category_cb->currentData().toString().toStdString();
	ep.streamlabs_category_name = ep.streamlabs_category_id.empty()
	                                      ? std::string()
	                                      : m_streamlabs_category_cb->currentText().toStdString();
#endif
	return ep;
}

/* -----------------------------------------------------------------------
 * Slot handlers
 * ----------------------------------------------------------------------- */
void EndpointDialog::on_template_selected(int index)
{
	if (index < 0 || index >= static_cast<int>(s_templates.size()))
		return;
	const QString &url = s_templates[index].url;
	if (!url.isEmpty())
		m_server_edit->setText(url);
}

void EndpointDialog::on_show_key_toggled(bool visible)
{
	m_key_edit->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
	m_show_key_btn->setText(visible ? tr("Hide") : tr("Show"));
}

void EndpointDialog::on_import_from_obs()
{
	/* v1.0.6 — read active OBS profile's stream config and fill the form.
	 * Avoids the OAuth maintenance tax by piggybacking on OBS's native
	 * "Connect Account" flow which already populated the profile config. */
	ObsServiceConfig cfg = import_from_active_obs_profile();
	if (!cfg.ok) {
		QMessageBox::warning(this, tr("Import from OBS"),
		    tr("Could not import from active OBS profile:\n\n%1\n\n"
		       "Tip: open OBS → Settings → Stream, connect your account, "
		       "and try again.")
		    .arg(QString::fromStdString(cfg.error_message)));
		return;
	}

	m_server_edit->setText(QString::fromStdString(cfg.server_url));
	m_key_edit->setText(QString::fromStdString(cfg.stream_key));

	/* If the Name field is empty/default, auto-fill from service name */
	if (m_name_edit->text().trimmed().isEmpty() ||
	    m_name_edit->text() == tr("New Endpoint")) {
		QString svc = QString::fromStdString(cfg.service_name);
		if (!svc.isEmpty()) {
			m_name_edit->setText(svc + tr(" (imported)"));
		}
	}

	/* Update template dropdown if server matches a known template */
	for (int i = 1; i < static_cast<int>(s_templates.size()); ++i) {
		if (QString::fromStdString(cfg.server_url).startsWith(s_templates[i].url)) {
			m_template_cb->blockSignals(true);
			m_template_cb->setCurrentIndex(i);
			m_template_cb->blockSignals(false);
			break;
		}
	}

	m_status_label->setText(
	    tr("✓ Imported %1 from active OBS profile.")
	    .arg(QString::fromStdString(cfg.service_name)));
	m_status_label->setStyleSheet("color: #2ecc71;");
	m_status_label->setVisible(true);
}

void EndpointDialog::on_import_tiktok_bridge()
{
	std::string default_path = default_tiktok_bridge_path();
	QString path = QString::fromStdString(default_path);

	if (path.isEmpty() || !QFileInfo::exists(path)) {
		path = QFileDialog::getOpenFileName(
			this,
			tr("Import TikTok Bridge JSON"),
			path,
			tr("JSON files (*.json);;All files (*)")
		);
		if (path.isEmpty())
			return;
	}

	TikTokBridgeConfig cfg = import_tiktok_bridge_file(path.toStdString());
	if (!cfg.ok) {
		QMessageBox::warning(this, tr("Import TikTok Bridge"),
		    tr("Could not import TikTok Bridge data:\n\n%1\n\n"
		       "Expected JSON fields: server_url/server and stream_key/key.")
		    .arg(QString::fromStdString(cfg.error_message)));
		return;
	}

	m_server_edit->setText(QString::fromStdString(cfg.server_url));
	m_key_edit->setText(QString::fromStdString(cfg.stream_key));

	if (m_name_edit->text().trimmed().isEmpty() ||
	    m_name_edit->text() == tr("New Endpoint")) {
		QString bridge_name = QString::fromStdString(cfg.name).trimmed();
		m_name_edit->setText(bridge_name.isEmpty() ? tr("TikTok Bridge") : bridge_name);
	}

	/* Update template dropdown if server matches a known template */
	for (int i = 1; i < static_cast<int>(s_templates.size()); ++i) {
		if (QString::fromStdString(cfg.server_url).startsWith(s_templates[i].url)) {
			m_template_cb->blockSignals(true);
			m_template_cb->setCurrentIndex(i);
			m_template_cb->blockSignals(false);
			break;
		}
	}

	select_canvas(std::string(), kOwnedVerticalCanvasName);

	m_bitrate_spin->setValue(2500);
	for (int i = 0; i < m_audio_cb->count(); ++i) {
		if (m_audio_cb->itemData(i).toInt() == 128) {
			m_audio_cb->setCurrentIndex(i);
			break;
		}
	}

	QString status = tr("Imported TikTok Bridge data from %1.")
		.arg(QFileInfo(path).fileName());
	if (!cfg.expires_at.empty()) {
		status += tr(" Expires: %1.")
			.arg(QString::fromStdString(cfg.expires_at));
	}

	m_status_label->setText(status);
	m_status_label->setStyleSheet("color: #2ecc71;");
	m_status_label->setVisible(true);
}

void EndpointDialog::on_test_connection()
{
	/* v1.0 stub: full RTMP handshake probe is planned for v1.1.
	 * TODO(v1.1): Implement 3-second RTMP handshake probe without streaming.
	 * Use obs_output_t "rtmp_output" with a custom signal handler, or
	 * a separate librtmp call.  Hard-stop after 3s regardless of result. */
	m_status_label->setText(
		tr("Test Connection is planned for v1.1.  "
		   "To verify your stream key, try starting the endpoint "
		   "and checking the Health tab.")
	);
	m_status_label->setVisible(true);
}

void EndpointDialog::on_save()
{
	if (m_name_edit->text().trimmed().isEmpty()) {
		QMessageBox::warning(this, tr("Validation"), tr("Please enter a name for this endpoint."));
		return;
	}
#ifdef SMULTI_STREAMLABS_TIKTOK
	/* Server URL and key are fetched at go-live: nothing to validate or compare here. */
	if (streamlabs_selected()) {
		m_result = collect_from_form();
		accept();
		return;
	}
#endif
	if (m_server_edit->text().trimmed().isEmpty()) {
		QMessageBox::warning(this, tr("Validation"), tr("Please enter a server URL."));
		return;
	}
	if (m_key_edit->text().trimmed().isEmpty()) {
		QMessageBox::warning(this, tr("Validation"), tr("Please enter a stream key."));
		return;
	}

	Endpoint candidate = collect_from_form();

	/* Warn about a duplicate target (same server + key as another enabled
	 * endpoint or the OBS main stream). The user may still save. The stream
	 * key is never shown or logged. */
	DuplicateResult dup = find_duplicate(candidate, m_registry.all(), read_main_service_target());
	if (dup.kind != DuplicateKind::None) {
		QString what = dup.kind == DuplicateKind::MainService
				       ? tr("the OBS main stream")
				       : tr("the endpoint \"%1\"").arg(QString::fromStdString(dup.other_name));
		auto answer = QMessageBox::warning(
			this, tr("Duplicate target"),
			tr("This endpoint uses the same server and stream key as %1. "
			   "Streaming twice to the same target usually fails or interrupts the stream. "
			   "Save anyway?").arg(what),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
	}

	m_result = candidate;
	accept();
}

#ifdef SMULTI_STREAMLABS_TIKTOK
/* -----------------------------------------------------------------------
 * Streamlabs TikTok key source (optional build flag)
 * ----------------------------------------------------------------------- */
void EndpointDialog::setup_streamlabs_ui(QVBoxLayout *outer)
{
	m_streamlabs_box = new QGroupBox(tr("Streamlabs TikTok"), this);
	auto *form = new QFormLayout(m_streamlabs_box);
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);

	m_streamlabs_status = new QLabel(tr("Not connected"), m_streamlabs_box);
	m_streamlabs_status->setWordWrap(true);
	form->addRow(tr("Status:"), m_streamlabs_status);

	auto *token_row = new QHBoxLayout();
	m_streamlabs_import_btn = new QPushButton(tr("Import token from generator config…"), m_streamlabs_box);
	m_streamlabs_import_btn->setToolTip(tr("Select the config.json of the StreamLabs TikTok Stream Key Generator. "
	                                       "Only its token is read; it is stored encrypted for your Windows user."));
	m_streamlabs_disconnect_btn = new QPushButton(tr("Disconnect"), m_streamlabs_box);
	m_streamlabs_disconnect_btn->setToolTip(tr("Delete the stored Streamlabs token."));
	token_row->addWidget(m_streamlabs_import_btn);
	token_row->addWidget(m_streamlabs_disconnect_btn);
	token_row->addStretch();
	form->addRow(QString(), token_row);

	m_streamlabs_title_edit = new QLineEdit(m_streamlabs_box);
	m_streamlabs_title_edit->setPlaceholderText(tr("TikTok stream title (default: endpoint name)"));
	form->addRow(tr("Title:"), m_streamlabs_title_edit);

	auto *search_row = new QHBoxLayout();
	m_streamlabs_search_edit = new QLineEdit(m_streamlabs_box);
	m_streamlabs_search_edit->setPlaceholderText(tr("Search a category"));
	m_streamlabs_search_btn = new QPushButton(tr("Search"), m_streamlabs_box);
	search_row->addWidget(m_streamlabs_search_edit);
	search_row->addWidget(m_streamlabs_search_btn);
	form->addRow(tr("Category:"), search_row);

	m_streamlabs_category_cb = new QComboBox(m_streamlabs_box);
	form->addRow(QString(), m_streamlabs_category_cb);

	m_streamlabs_box->setVisible(false);
	outer->addWidget(m_streamlabs_box);

	connect(m_key_source_cb, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
	        [this](int) { apply_key_source(); });
	connect(m_streamlabs_import_btn, &QPushButton::clicked, this, [this]() { on_streamlabs_import(); });
	connect(m_streamlabs_disconnect_btn, &QPushButton::clicked, this, [this]() { on_streamlabs_disconnect(); });
	connect(m_streamlabs_search_btn, &QPushButton::clicked, this, [this]() { on_streamlabs_search(); });
	connect(m_streamlabs_search_edit, &QLineEdit::returnPressed, this, [this]() { on_streamlabs_search(); });
}

bool EndpointDialog::streamlabs_selected() const
{
	return m_key_source_cb && m_key_source_cb->currentIndex() == 1;
}

void EndpointDialog::apply_key_source()
{
	const bool sl = streamlabs_selected();
	if (sl && !m_streamlabs_shown) {
		/* Keep the manual values out of sight; they are saved unchanged. */
		m_manual_server = m_server_edit->text();
		m_manual_key = m_key_edit->text();
		m_server_edit->clear();
		m_key_edit->clear();
	} else if (!sl && m_streamlabs_shown) {
		m_server_edit->setText(m_manual_server);
		m_key_edit->setText(m_manual_key);
	}
	m_streamlabs_shown = sl;

	m_server_edit->setReadOnly(sl);
	m_key_edit->setReadOnly(sl);
	m_server_edit->setEnabled(!sl);
	m_key_edit->setEnabled(!sl);
	m_server_edit->setPlaceholderText(sl ? tr("fetched at go-live") : tr("rtmp://... or rtmps://..."));
	m_key_edit->setPlaceholderText(sl ? tr("fetched at go-live") : tr("Stream key (stored plain)"));
	m_template_cb->setEnabled(!sl);
	m_show_key_btn->setEnabled(!sl);
	m_import_btn->setEnabled(!sl);
	m_tiktok_bridge_btn->setEnabled(!sl);
	m_test_btn->setEnabled(!sl);
	m_streamlabs_box->setVisible(sl);
	if (sl)
		refresh_streamlabs_status();
}

void EndpointDialog::refresh_streamlabs_status()
{
	StreamlabsClient *client = streamlabs_client();
	if (!client) {
		m_streamlabs_status->setText(tr("Streamlabs TikTok is not available."));
		return;
	}
	if (!streamlabs_vault::exists()) {
		m_streamlabs_status->setText(tr("Not connected. Import the token from the generator config.json."));
		m_streamlabs_disconnect_btn->setEnabled(false);
		return;
	}
	m_streamlabs_disconnect_btn->setEnabled(true);
	m_streamlabs_status->setText(tr("Checking…"));
	QPointer<EndpointDialog> guard(this);
	client->info([guard](const StreamlabsResult &r) {
		if (!guard)
			return;
		if (!r.ok) {
			guard->m_streamlabs_status->setText(QString::fromStdString(r.error_message));
			return;
		}
		const QString user = QString::fromStdString(r.username);
		const QString live = r.can_be_live ? EndpointDialog::tr("yes") : EndpointDialog::tr("no");
		guard->m_streamlabs_status->setText(
			user.isEmpty() ? EndpointDialog::tr("Connected — can go live: %1").arg(live)
			               : EndpointDialog::tr("Connected as @%1 — can go live: %2").arg(user, live));
	});
}

void EndpointDialog::on_streamlabs_import()
{
	const QString path = QFileDialog::getOpenFileName(this, tr("Import token from generator config"), QString(),
	                                                  tr("Generator config (config.json);;JSON files (*.json)"));
	if (path.isEmpty())
		return;

	StreamlabsTokenImport imported = streamlabs_import_generator_config_file(path.toStdString());
	if (!imported.ok) {
		QMessageBox::warning(this, tr("Import token"), QString::fromStdString(imported.error_message));
		return;
	}
	std::string error;
	const bool saved = streamlabs_vault::save(imported.token, error);
	streamlabs_vault::wipe(imported.token);
	if (!saved) {
		QMessageBox::warning(this, tr("Import token"), QString::fromStdString(error));
		return;
	}
	obs_log(LOG_INFO, "streamlabs tiktok: token imported");
	refresh_streamlabs_status();
}

void EndpointDialog::on_streamlabs_disconnect()
{
	if (!streamlabs_vault::remove()) {
		QMessageBox::warning(this, tr("Disconnect"), tr("Could not delete the stored token."));
		return;
	}
	obs_log(LOG_INFO, "streamlabs tiktok: token deleted");
	refresh_streamlabs_status();
}

void EndpointDialog::on_streamlabs_search()
{
	StreamlabsClient *client = streamlabs_client();
	const QString query = m_streamlabs_search_edit->text().trimmed();
	if (!client || query.isEmpty())
		return;
	m_streamlabs_search_btn->setEnabled(false);
	QPointer<EndpointDialog> guard(this);
	client->search(query.toStdString(), [guard](const StreamlabsResult &r) {
		if (!guard)
			return;
		guard->m_streamlabs_search_btn->setEnabled(true);
		if (!r.ok) {
			guard->m_streamlabs_status->setText(QString::fromStdString(r.error_message));
			return;
		}
		guard->m_streamlabs_category_cb->clear();
		for (const auto &c : r.categories)
			guard->m_streamlabs_category_cb->addItem(QString::fromStdString(c.full_name),
			                                         QString::fromStdString(c.game_mask_id));
		guard->m_streamlabs_category_cb->setCurrentIndex(0);
	});
}
#endif

} // namespace smulti
