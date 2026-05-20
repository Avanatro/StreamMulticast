/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/EndpointRegistry.hpp"

#include <QtWidgets/QWidget>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QCheckBox>

namespace smulti {

/**
 * EndpointCard — compact widget representing one endpoint in the ConfigTab.
 *
 * Shows: name, status LED (colour square), enabled toggle, Edit button.
 * Emits editRequested(id) when user clicks Edit.
 * Emits enableToggled(id, enabled) when toggle changes.
 */
class EndpointCard : public QWidget {
	Q_OBJECT

public:
	explicit EndpointCard(const Endpoint &ep, QWidget *parent = nullptr);

	void update_state(const Endpoint &ep);
	const std::string &endpoint_id() const { return m_id; }

signals:
	void editRequested(const std::string &id);
	void enableToggled(const std::string &id, bool enabled);
	void deleteRequested(const std::string &id);

private:
	void setup_ui();

	std::string  m_id;
	Endpoint     m_ep;

	QLabel      *m_name_label   {nullptr};
	QLabel      *m_status_led   {nullptr};
	QCheckBox   *m_enabled_cb   {nullptr};
	QPushButton *m_edit_btn     {nullptr};
	QPushButton *m_delete_btn   {nullptr};
};

/**
 * ConfigTab — Tab 2 of the MultistreamDock.
 *
 * Displays a vertical list of EndpointCard widgets in a QScrollArea.
 * A QListWidget wrapper handles drag-to-reorder (drops are mapped to
 * EndpointRegistry::reorder() calls).
 *
 * "Add Endpoint" button at the bottom opens EndpointDialog with a fresh endpoint.
 *
 * Subscribes to EndpointRegistry observer notifications for live updates.
 *
 * AVANATRO-VERIFY: Drag-to-reorder via QListWidget — we use a QListWidget
 * whose items are mapped to EndpointCard widgets via QListWidgetItem::setSizeHint
 * + setItemWidget.  This pattern works but prevents custom drag visuals.
 * An alternative is QAbstractItemModel with DragDropMode.  Current impl is
 * functional but basic — upgrade noted for v1.1.
 */
class ConfigTab : public QWidget {
	Q_OBJECT

public:
	explicit ConfigTab(EndpointRegistry &registry, QWidget *parent = nullptr);
	~ConfigTab() override;

private slots:
	void on_add_endpoint();
	void on_edit_endpoint(const std::string &id);
	void on_toggle_endpoint(const std::string &id, bool enabled);
	void on_delete_endpoint(const std::string &id);
	void on_list_reorder();

private:
	void setup_ui();
	void rebuild_list();
	void on_registry_changed(ChangeKind kind, const Endpoint &ep);

	EndpointRegistry &m_registry;

	QListWidget  *m_list       {nullptr};
	QPushButton  *m_add_btn    {nullptr};

	int m_observer_token = -1;
};

} // namespace smulti
