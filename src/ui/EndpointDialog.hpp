/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/Endpoint.hpp"
#include "../core/EndpointRegistry.hpp"
#include "../pipeline/EncoderFactory.hpp"

#include <QtWidgets/QDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLabel>

namespace smulti {

/**
 * EndpointDialog — modal settings dialog for a single endpoint.
 *
 * Fields:
 *   - Name
 *   - Server URL (with pre-defined template dropdown)
 *   - Stream Key (password field with Show/Hide toggle)
 *   - Encoder Backend (auto-detected available backends)
 *   - Video Bitrate (500–25000 kbps spinner)
 *   - Keyframe Interval (1–10 s spinner)
 *   - Audio Bitrate (dropdown: 64/96/128/160/192/320)
 *   - Linked to OBS Main Stream (checkbox)
 *
 * Buttons: Save | Cancel | Test Connection
 *
 * "Test Connection" performs a 3-second RTMP handshake probe without
 * actually streaming.  This feature is a stub in v1.0 — it opens a
 * QMessageBox explaining it will be fully implemented in v1.1.
 * TODO(v1.1): Implement real RTMP handshake probe via librtmp / obs_output.
 *
 * result_endpoint() returns the edited endpoint after Accepted.
 */
class EndpointDialog : public QDialog {
	Q_OBJECT

public:
	explicit EndpointDialog(const Endpoint     &ep,
	                        EndpointRegistry   &registry,
	                        QWidget            *parent = nullptr);

	/** Returns the edited Endpoint (valid only after exec() == Accepted) */
	const Endpoint &result_endpoint() const { return m_result; }

private slots:
	void on_template_selected(int index);
	void on_show_key_toggled(bool visible);
	void on_test_connection();
	void on_import_from_obs();   ///< v1.0.6
	void on_save();

private:
	void setup_ui();
	void populate_from_endpoint();
	Endpoint collect_from_form() const;

	Endpoint           m_endpoint; ///< original (input)
	Endpoint           m_result;   ///< edited (output)
	EndpointRegistry  &m_registry;

	/* Form widgets */
	QLineEdit   *m_name_edit    {nullptr};
	QComboBox   *m_template_cb  {nullptr};
	QLineEdit   *m_server_edit  {nullptr};
	QLineEdit   *m_key_edit     {nullptr};
	QPushButton *m_show_key_btn {nullptr};
	QComboBox   *m_backend_cb   {nullptr};
	QSpinBox    *m_bitrate_spin {nullptr};
	QSpinBox    *m_keyint_spin  {nullptr};
	QComboBox   *m_audio_cb     {nullptr};
	QComboBox   *m_orientation_cb {nullptr};   ///< v1.0.5 — Source / Vertical Letterbox
	QCheckBox   *m_linked_cb    {nullptr};
	QPushButton *m_import_btn   {nullptr};   ///< v1.0.6 — Import from active OBS profile
	QPushButton *m_test_btn     {nullptr};
	QPushButton *m_save_btn     {nullptr};
	QPushButton *m_cancel_btn   {nullptr};
	QLabel      *m_status_label {nullptr};

	/* Available backends (auto-detected) */
	std::vector<EncoderBackend> m_available_backends;

	/* Server URL templates */
	struct ServerTemplate {
		QString label;
		QString url;
	};
	static const std::vector<ServerTemplate> s_templates;
};

} // namespace smulti
