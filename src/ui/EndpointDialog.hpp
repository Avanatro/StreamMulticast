/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/Endpoint.hpp"
#include "../core/EndpointRegistry.hpp"
#include "../pipeline/EncoderFactory.hpp"
#include "../core/CanvasResolve.hpp"

#include <QtWidgets/QDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLabel>

#include <memory>

class QGroupBox;
class QVBoxLayout;
class QToolButton;
class QAction;

namespace smulti {

class StreamlabsLoginFlow;

/**
 * EndpointDialog — modal settings dialog for a single endpoint.
 *
 * Fields:
 *   - Name
 *   - Server URL (with pre-defined template dropdown)
 *   - Stream Key (password field with Show/Hide toggle)
 *   - Video canvas (Main canvas or any OBS canvas)
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
	~EndpointDialog() override;

	/** Returns the edited Endpoint (valid only after exec() == Accepted) */
	const Endpoint &result_endpoint() const { return m_result; }

private slots:
	void on_template_selected(int index);
	void on_show_key_toggled(bool visible);
	void on_test_connection();
	void on_import_from_obs();   ///< v1.0.6
	void on_import_tiktok_bridge(); ///< v1.0.7
	void on_save();

private:
	void setup_ui();
	void populate_from_endpoint();
	Endpoint collect_from_form() const;
	void populate_canvas_combo();
	/** Select the entry for this ref; a ref missing from the list gets a "(missing)" entry. */
	void select_canvas(const std::string &uuid, const std::string &name);

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
	QComboBox   *m_canvas_cb    {nullptr};   ///< Video canvas: Main (empty ref) or a canvas uuid + name
	QCheckBox   *m_linked_cb    {nullptr};
	QPushButton *m_import_btn   {nullptr};   ///< v1.0.6 — Import from active OBS profile
	QPushButton *m_tiktok_bridge_btn {nullptr}; ///< v1.0.7 — Import local TikTok Bridge JSON
	QPushButton *m_test_btn     {nullptr};
	QPushButton *m_save_btn     {nullptr};
	QPushButton *m_cancel_btn   {nullptr};
	QLabel      *m_status_label {nullptr};

#ifdef SMULTI_STREAMLABS_TIKTOK
	/* Key source "Streamlabs TikTok" (optional build flag): the server URL and key
	 * are fetched at go-live. The token comes from the browser login (primary),
	 * the Streamlabs Desktop login, or the generator's config.json (under More);
	 * it is stored in the DPAPI vault and never shown. */
	void setup_streamlabs_ui(QVBoxLayout *outer);
	bool streamlabs_selected() const;
	void apply_key_source();
	void refresh_streamlabs_status();
	void on_streamlabs_login();
	void on_streamlabs_login_cancel();
	void on_streamlabs_desktop_import();
	void on_streamlabs_import();
	void on_streamlabs_disconnect();
	void set_streamlabs_waiting(bool waiting);
	void on_streamlabs_search();

	QComboBox   *m_key_source_cb            {nullptr};
	QGroupBox   *m_streamlabs_box           {nullptr};
	QLabel      *m_streamlabs_status        {nullptr};
	QPushButton *m_streamlabs_login_btn     {nullptr};
	QPushButton *m_streamlabs_cancel_btn    {nullptr};
	QPushButton *m_streamlabs_desktop_btn   {nullptr};
	QToolButton *m_streamlabs_more_btn      {nullptr};
	QAction     *m_streamlabs_import_action {nullptr};
	QPushButton *m_streamlabs_disconnect_btn{nullptr};
	QLineEdit   *m_streamlabs_title_edit    {nullptr};
	QLineEdit   *m_streamlabs_search_edit   {nullptr};
	QPushButton *m_streamlabs_search_btn    {nullptr};
	QComboBox   *m_streamlabs_category_cb   {nullptr};
	/* Manual server/key kept while "Streamlabs TikTok" is selected, saved unchanged. */
	QString      m_manual_server;
	QString      m_manual_key;
	bool         m_streamlabs_shown         {false};
	/* Browser login in progress; cancelled when the dialog closes or is destroyed. */
	std::unique_ptr<StreamlabsLoginFlow> m_streamlabs_login;
#endif

	/* Available backends (auto-detected) */
	std::vector<EncoderBackend> m_available_backends;

	/* Canvas snapshot taken when the dialog was built */
	std::vector<CanvasInfo> m_canvases;

	/* Server URL templates */
	struct ServerTemplate {
		QString label;
		QString url;
	};
	static const std::vector<ServerTemplate> s_templates;
};

} // namespace smulti
