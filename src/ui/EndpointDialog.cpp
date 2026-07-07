/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "EndpointDialog.hpp"
#include "../plugin-support.h"
#include "../core/ObsServiceImport.hpp"
#include "../core/TikTokBridgeImport.hpp"

#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtCore/QFileInfo>
#include <QtGui/QStandardItemModel>
#include <QtGui/QStandardItem>

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

	/* Output Orientation (v1.0.5) */
	m_orientation_cb = new QComboBox(this);
	m_orientation_cb->addItem(tr("Source (match OBS canvas)"),
	                          static_cast<int>(OutputOrientation::SourceMatch));
	m_orientation_cb->addItem(tr("Vertical 1080×1920 — Letterbox (TikTok / Shorts / Reels)"),
	                          static_cast<int>(OutputOrientation::Vertical1080x1920Letterbox));
	/* Center-crop reserved for v1.1 — listed disabled-style as a hint */
	m_orientation_cb->addItem(tr("Vertical 1080×1920 — Center-Crop  (v1.1, not yet available)"),
	                          static_cast<int>(OutputOrientation::Vertical1080x1920CenterCrop));
	/* Disable the v1.1 entry */
	auto *model = qobject_cast<QStandardItemModel *>(m_orientation_cb->model());
	if (model && model->item(2)) {
		model->item(2)->setFlags(model->item(2)->flags() & ~Qt::ItemIsEnabled);
	}
	form->addRow(tr("Output Orientation:"), m_orientation_cb);

	/* Linked to main stream */
	m_linked_cb = new QCheckBox(tr("Start/stop with OBS main stream"), this);
	form->addRow(QString(), m_linked_cb);

	outer->addLayout(form);

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

	/* Orientation combo */
	for (int i = 0; i < m_orientation_cb->count(); ++i) {
		if (m_orientation_cb->itemData(i).toInt() == static_cast<int>(m_endpoint.orientation)) {
			m_orientation_cb->setCurrentIndex(i);
			break;
		}
	}

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
	ep.orientation          = static_cast<OutputOrientation>(
	                              m_orientation_cb->currentData().toInt());
	ep.linked_to_main       = m_linked_cb->isChecked();
	ep.encoder_backend      = static_cast<EncoderBackend>(
	                              m_backend_cb->currentData().toInt());
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

	for (int i = 0; i < m_orientation_cb->count(); ++i) {
		if (m_orientation_cb->itemData(i).toInt() ==
		    static_cast<int>(OutputOrientation::Vertical1080x1920Letterbox)) {
			m_orientation_cb->setCurrentIndex(i);
			break;
		}
	}

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
	if (m_server_edit->text().trimmed().isEmpty()) {
		QMessageBox::warning(this, tr("Validation"), tr("Please enter a server URL."));
		return;
	}
	if (m_key_edit->text().trimmed().isEmpty()) {
		QMessageBox::warning(this, tr("Validation"), tr("Please enter a stream key."));
		return;
	}

	m_result = collect_from_form();
	accept();
}

} // namespace smulti
