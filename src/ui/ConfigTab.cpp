/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "ConfigTab.hpp"
#include "EndpointDialog.hpp"
#include "../plugin-support.h"
#include "../pipeline/OutputController.hpp"

#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QListWidgetItem>
#include <QtCore/QMetaObject>
#include <QtWidgets/QMessageBox>

namespace smulti {

/* -----------------------------------------------------------------------
 * EndpointCard
 * ----------------------------------------------------------------------- */
EndpointCard::EndpointCard(const Endpoint &ep, QWidget *parent)
	: QWidget(parent)
	, m_id(ep.id)
	, m_ep(ep)
{
	setup_ui();
}

void EndpointCard::setup_ui()
{
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(8, 6, 8, 6);
	layout->setSpacing(10);

	/* Status LED (colour square) */
	m_status_led = new QLabel(this);
	m_status_led->setFixedSize(14, 14);
	m_status_led->setStyleSheet("background: #95a5a6; border-radius: 7px;");
	layout->addWidget(m_status_led);

	/* Name */
	m_name_label = new QLabel(QString::fromStdString(m_ep.name), this);
	m_name_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	layout->addWidget(m_name_label);

	/* Enabled toggle */
	m_enabled_cb = new QCheckBox(tr("On"), this);
	m_enabled_cb->setChecked(m_ep.enabled);
	layout->addWidget(m_enabled_cb);

	/* Edit button — readable size, no longer cramped */
	m_edit_btn = new QPushButton(tr("Edit"), this);
	m_edit_btn->setMinimumWidth(80);
	m_edit_btn->setMinimumHeight(28);
	layout->addWidget(m_edit_btn);

	/* Delete button — full word, not a single 'X' */
	m_delete_btn = new QPushButton(tr("Delete"), this);
	m_delete_btn->setMinimumWidth(80);
	m_delete_btn->setMinimumHeight(28);
	m_delete_btn->setToolTip(tr("Remove endpoint"));
	layout->addWidget(m_delete_btn);

	/* Minimum card height ensures buttons aren't clipped at the bottom */
	setMinimumHeight(44);

	setLayout(layout);

	/* Connections */
	connect(m_edit_btn, &QPushButton::clicked, this, [this]() {
		emit editRequested(m_id);
	});
	connect(m_enabled_cb, &QCheckBox::toggled, this, [this](bool checked) {
		emit enableToggled(m_id, checked);
	});
	connect(m_delete_btn, &QPushButton::clicked, this, [this]() {
		emit deleteRequested(m_id);
	});
}

void EndpointCard::update_state(const Endpoint &ep)
{
	m_ep = ep;
	m_name_label->setText(QString::fromStdString(ep.name));
	m_enabled_cb->blockSignals(true);
	m_enabled_cb->setChecked(ep.enabled);
	m_enabled_cb->blockSignals(false);
}

/* -----------------------------------------------------------------------
 * ConfigTab
 * ----------------------------------------------------------------------- */
ConfigTab::ConfigTab(EndpointRegistry &registry, QWidget *parent)
	: QWidget(parent)
	, m_registry(registry)
{
	setup_ui();

	/* Register as observer for live updates from the registry.
	 * Observer callbacks may come from non-Qt threads — use invokeMethod to
	 * marshal onto the Qt thread. */
	m_observer_token = m_registry.register_observer(
		[this](ChangeKind kind, const Endpoint &ep) {
			/* Post to Qt main thread */
			QMetaObject::invokeMethod(
				this,
				[this, kind, ep]() { on_registry_changed(kind, ep); },
				Qt::QueuedConnection
			);
		}
	);
}

ConfigTab::~ConfigTab()
{
	if (m_observer_token >= 0)
		m_registry.unregister_observer(m_observer_token);
}

void ConfigTab::setup_ui()
{
	auto *outer_layout = new QVBoxLayout(this);
	outer_layout->setContentsMargins(4, 4, 4, 4);
	outer_layout->setSpacing(4);

	/* Endpoint list — drag-to-reorder enabled */
	m_list = new QListWidget(this);
	m_list->setDragDropMode(QAbstractItemView::InternalMove);
	m_list->setDefaultDropAction(Qt::MoveAction);
	m_list->setSelectionMode(QAbstractItemView::NoSelection);
	m_list->setSpacing(2);
	outer_layout->addWidget(m_list, 1);

	/* Add Endpoint button */
	m_add_btn = new QPushButton(tr("+ Add Endpoint"), this);
	outer_layout->addWidget(m_add_btn);

	setLayout(outer_layout);

	connect(m_add_btn, &QPushButton::clicked, this, &ConfigTab::on_add_endpoint);

	// AVANATRO-VERIFY: QListWidget rowsMoved signal — fired after internal drag-to-reorder.
	// In Qt6 this is QAbstractItemModel::rowsMoved — connect via model().
	connect(m_list->model(), &QAbstractItemModel::rowsMoved, this, &ConfigTab::on_list_reorder);

	/* Populate initial list */
	rebuild_list();
}

void ConfigTab::rebuild_list()
{
	m_list->clear();

	auto endpoints = m_registry.all();
	for (const auto &ep : endpoints) {
		auto *card = new EndpointCard(ep, nullptr);
		auto *item = new QListWidgetItem(m_list);
		item->setSizeHint(card->sizeHint());
		item->setData(Qt::UserRole, QString::fromStdString(ep.id));
		m_list->addItem(item);
		m_list->setItemWidget(item, card);

		connect(card, &EndpointCard::editRequested,   this, &ConfigTab::on_edit_endpoint);
		connect(card, &EndpointCard::enableToggled,   this, &ConfigTab::on_toggle_endpoint);
		connect(card, &EndpointCard::deleteRequested, this, &ConfigTab::on_delete_endpoint);
	}
}

/* -----------------------------------------------------------------------
 * Slot handlers
 * ----------------------------------------------------------------------- */
void ConfigTab::on_add_endpoint()
{
	Endpoint new_ep = Endpoint::make_default("New Endpoint");

	EndpointDialog dlg(new_ep, m_registry, this);
	if (dlg.exec() == QDialog::Accepted) {
		m_registry.add(dlg.result_endpoint());
	}
}

void ConfigTab::on_edit_endpoint(const std::string &id)
{
	const Endpoint *ep_ptr = m_registry.find(id);
	if (!ep_ptr)
		return;

	Endpoint ep = *ep_ptr; // copy
	EndpointDialog dlg(ep, m_registry, this);
	if (dlg.exec() == QDialog::Accepted) {
		m_registry.update(dlg.result_endpoint());
	}
}

void ConfigTab::on_toggle_endpoint(const std::string &id, bool enabled)
{
	const Endpoint *ep_ptr = m_registry.find(id);
	if (!ep_ptr)
		return;

	Endpoint updated = *ep_ptr;
	updated.enabled  = enabled;

	if (!enabled) {
		/* Stop the output if it was running */
		auto ctrl = m_registry.controller_for(id);
		if (ctrl && ctrl->is_running())
			ctrl->stop();
	}

	m_registry.update(updated);
}

void ConfigTab::on_delete_endpoint(const std::string &id)
{
	const Endpoint *ep_ptr = m_registry.find(id);
	if (!ep_ptr)
		return;

	QString name = QString::fromStdString(ep_ptr->name);
	auto reply = QMessageBox::question(
		this,
		tr("Remove Endpoint"),
		tr("Remove endpoint \"%1\"?").arg(name),
		QMessageBox::Yes | QMessageBox::No
	);
	if (reply == QMessageBox::Yes)
		m_registry.remove(id);
}

void ConfigTab::on_list_reorder()
{
	/* Collect the new ordering from QListWidget */
	std::vector<std::string> ordered_ids;
	ordered_ids.reserve(m_list->count());
	for (int i = 0; i < m_list->count(); ++i) {
		auto *item = m_list->item(i);
		ordered_ids.push_back(item->data(Qt::UserRole).toString().toStdString());
	}
	m_registry.reorder(ordered_ids);
}

void ConfigTab::on_registry_changed(ChangeKind kind, const Endpoint &ep)
{
	Q_UNUSED(ep)
	/* Simplest approach: always rebuild the full list.
	 * For v1.1: diff-based update to preserve scroll position. */
	if (kind == ChangeKind::Added || kind == ChangeKind::Removed)
		rebuild_list();
	else {
		/* Updated — refresh the matching card */
		for (int i = 0; i < m_list->count(); ++i) {
			auto *item = m_list->item(i);
			if (item->data(Qt::UserRole).toString().toStdString() == ep.id) {
				auto *card = qobject_cast<EndpointCard *>(m_list->itemWidget(item));
				if (card)
					card->update_state(ep);
				break;
			}
		}
	}
}

} // namespace smulti
