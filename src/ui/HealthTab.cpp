/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "HealthTab.hpp"
#include "../core/EndpointRegistry.hpp"

#include <QVBoxLayout>
#include <QHeaderView>
#include <QColor>
#include <QDesktopServices>
#include <QUrl>
#include <QFont>

namespace smulti {

/* -----------------------------------------------------------------------
 * HealthTableModel
 * ----------------------------------------------------------------------- */
HealthTableModel::HealthTableModel(EndpointRegistry &registry,
                                   HealthSampler    &sampler,
                                   QObject          *parent)
	: QAbstractTableModel(parent)
	, m_registry(registry)
	, m_sampler(sampler)
{
}

int HealthTableModel::rowCount(const QModelIndex &parent) const
{
	Q_UNUSED(parent)
	return static_cast<int>(m_endpoints_cache.size());
}

int HealthTableModel::columnCount(const QModelIndex &parent) const
{
	Q_UNUSED(parent)
	return COL_COUNT;
}

QVariant HealthTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
	if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
		return {};

	switch (section) {
	case COL_NAME:       return tr("Name");
	case COL_STATUS:     return tr("Status");
	case COL_TARGET:     return tr("Target (kbps)");
	case COL_ACTUAL:     return tr("Actual (kbps)");
	case COL_DROPS:      return tr("Drops");
	case COL_RECONNECTS: return tr("Reconnects");
	case COL_UPTIME:     return tr("Uptime");
	default:             return {};
	}
}

QString HealthTableModel::state_label(OutputState s) const
{
	switch (s) {
	case OutputState::Idle:         return tr("Offline");
	case OutputState::Starting:     return tr("Starting...");
	case OutputState::Live:         return tr("Live");
	case OutputState::Reconnecting: return tr("Reconnecting");
	case OutputState::FailedHard:   return tr("Error");
	default:                        return tr("Unknown");
	}
}

QColor HealthTableModel::state_color(OutputState s) const
{
	switch (s) {
	case OutputState::Live:         return QColor(0x2ecc71); // green
	case OutputState::Starting:     return QColor(0xf39c12); // orange
	case OutputState::Reconnecting: return QColor(0xf1c40f); // yellow
	case OutputState::FailedHard:   return QColor(0xe74c3c); // red
	default:                        return QColor(0x95a5a6); // grey
	}
}

QVariant HealthTableModel::data(const QModelIndex &index, int role) const
{
	if (!index.isValid() || index.row() >= rowCount())
		return {};

	const Endpoint &ep = m_endpoints_cache[index.row()];
	auto it = m_snapshot_cache.find(ep.id);
	const HealthSnapshot *snap = (it != m_snapshot_cache.end()) ? &it->second : nullptr;

	OutputState state = snap ? snap->state : OutputState::Idle;

	if (role == Qt::DisplayRole) {
		switch (index.column()) {
		case COL_NAME:
			return QString::fromStdString(ep.name);
		case COL_STATUS:
			return state_label(state);
		case COL_TARGET:
			return snap ? QString::number(snap->target_bitrate) : QStringLiteral("—");
		case COL_ACTUAL:
			if (!snap || state != OutputState::Live) return QStringLiteral("—");
			return QString::number(static_cast<int>(snap->actual_bitrate));
		case COL_DROPS:
			return snap ? QString::number(snap->dropped_frames) : QStringLiteral("—");
		case COL_RECONNECTS:
			return snap ? QString::number(snap->reconnect_count) : QStringLiteral("—");
		case COL_UPTIME:
			if (!snap || state == OutputState::Idle) return QStringLiteral("—");
			{
				int64_t s_total = snap->uptime_sec;
				int     h = static_cast<int>(s_total / 3600);
				int     m = static_cast<int>((s_total % 3600) / 60);
				int     s = static_cast<int>(s_total % 60);
				return QString("%1:%2:%3")
				           .arg(h, 2, 10, QChar('0'))
				           .arg(m, 2, 10, QChar('0'))
				           .arg(s, 2, 10, QChar('0'));
			}
		default: return {};
		}
	}

	if (role == Qt::ForegroundRole && index.column() == COL_STATUS) {
		return QBrush(state_color(state));
	}

	if (role == Qt::ToolTipRole) {
		if (!snap || snap->last_error.empty())
			return {};
		return QString::fromStdString(snap->last_error);
	}

	return {};
}

void HealthTableModel::refresh()
{
	beginResetModel();
	m_endpoints_cache = m_registry.all();
	m_snapshot_cache  = m_sampler.snapshot();
	endResetModel();
}

/* -----------------------------------------------------------------------
 * HealthTab
 * ----------------------------------------------------------------------- */
HealthTab::HealthTab(EndpointRegistry &registry,
                     HealthSampler    &sampler,
                     QWidget          *parent)
	: QWidget(parent)
	, m_registry(registry)
	, m_sampler(sampler)
{
	setup_ui();
}

void HealthTab::setup_ui()
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	/* Live-grid table */
	m_model = new HealthTableModel(m_registry, m_sampler, this);
	m_table = new QTableView(this);
	m_table->setModel(m_model);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::SingleSelection);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setStretchLastSection(true);
	m_table->verticalHeader()->setVisible(false);
	m_table->setAlternatingRowColors(true);

	layout->addWidget(m_table, 1 /* stretch */);

	/* Footer — cross-sell link to Stream Health Doctor plus a discreet
	 * Avanatro credit linking to the homepage.
	 * Plausible tracking via UTM param in the URLs. */
	m_footer = new QLabel(this);
	m_footer->setOpenExternalLinks(true);
	m_footer->setAlignment(Qt::AlignCenter);
	m_footer->setText(
		QStringLiteral(
		    "<small><a href='https://tools.avanatro.com/stream-health/"
		    "?utm_source=streammulticast&utm_medium=dock_footer'>"
		    "Detailed Diagnostics &rarr; Stream Health Doctor</a>"
		    "&nbsp;&nbsp;&middot;&nbsp;&nbsp;"
		    "<span style='color:gray'>by <a href='https://avanatro.com/"
		    "?utm_source=streammulticast&utm_medium=dock_footer'>Avanatro</a>"
		    "</span></small>"
		)
	);
	layout->addWidget(m_footer);

	/* 500 ms refresh timer */
	m_timer = new QTimer(this);
	m_timer->setInterval(500);
	connect(m_timer, &QTimer::timeout, this, &HealthTab::on_refresh_timer);
	m_timer->start();
}

void HealthTab::on_refresh_timer()
{
	/* Already on the Qt main thread — safe to update model directly */
	m_model->refresh();
}

} // namespace smulti
