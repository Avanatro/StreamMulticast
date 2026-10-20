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
#include <QCheckBox>
#include <QStringList>

namespace smulti {

namespace {

QString alert_reason_text(const std::string &reason)
{
	if (reason == kAlertReasonReconnecting)
		return QObject::tr("Reconnecting");
	if (reason == kAlertReasonFailed)
		return QObject::tr("Output failed");
	/* Numbers from the policy defaults, so the text cannot drift from the rule. */
	const AlertThresholds t{};
	if (reason == kAlertReasonDropped)
		return QObject::tr("Dropped frames: at least %1 % of the last %2 seconds")
			.arg(t.drop_ratio * 100.0, 0, 'g', 3)
			.arg(t.drop_window_ms / 1000);
	if (reason == kAlertReasonLowBitrate)
		return QObject::tr("Bitrate below %1 % of target for %2 seconds")
			.arg(t.low_bitrate_fraction * 100.0, 0, 'g', 3)
			.arg(t.low_bitrate_hold_ms / 1000);
	return QString::fromStdString(reason);
}

bool same_alert(const AlertStatus &a, const AlertStatus &b)
{
	return a.level == b.level && a.reasons == b.reasons;
}

} // namespace

/* -----------------------------------------------------------------------
 * HealthTableModel
 * ----------------------------------------------------------------------- */
HealthTableModel::HealthTableModel(EndpointRegistry &registry,
                                   HealthSampler    &sampler,
                                   AlertMonitor     &alerts,
                                   QObject          *parent)
	: QAbstractTableModel(parent)
	, m_registry(registry)
	, m_sampler(sampler)
	, m_alerts(alerts)
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

	auto alert_it = m_alert_cache.find(ep.id);
	const AlertStatus *alert = alert_it != m_alert_cache.end() ? &alert_it->second : nullptr;

	if (role == Qt::BackgroundRole && alert)
		return QBrush(QColor(192, 57, 43, 110)); // red, translucent so it works on light and dark themes

	if (role == Qt::ToolTipRole) {
		QStringList lines;
		if (alert) {
			QStringList reasons;
			for (const auto &r : alert->reasons)
				reasons << alert_reason_text(r);
			lines << tr("Alert: %1").arg(reasons.join(QStringLiteral("; ")));
		}
		if (snap && !snap->last_error.empty())
			lines << QString::fromStdString(snap->last_error);
		if (lines.isEmpty())
			return {};
		return lines.join(QChar(0x0A));
	}

	return {};
}

void HealthTableModel::refresh()
{
	auto new_endpoints = m_registry.all();
	auto new_snapshots = m_sampler.snapshot();
	auto new_alerts    = m_alerts.statuses();

	/* H2 fix: beginResetModel()/endResetModel() on every 500 ms poll tick
	 * invalidated selection, scroll position and every persistent model
	 * index, forcing the view to re-query every cell each tick even when
	 * nothing changed. Only fall back to a full reset when the endpoint set
	 * or its order actually changed (add/remove/reorder) — routine polls of
	 * an unchanged endpoint list are diffed cell-by-cell below instead. Rows
	 * are matched by endpoint ID (not positional index) so a reorder is
	 * detected as a topology change rather than misattributing changed
	 * cells to the wrong row. */
	bool topology_changed = new_endpoints.size() != m_endpoints_cache.size();
	if (!topology_changed) {
		for (size_t i = 0; i < new_endpoints.size(); ++i) {
			if (new_endpoints[i].id != m_endpoints_cache[i].id) {
				topology_changed = true;
				break;
			}
		}
	}

	if (topology_changed) {
		beginResetModel();
		m_endpoints_cache = std::move(new_endpoints);
		m_snapshot_cache  = std::move(new_snapshots);
		m_alert_cache     = std::move(new_alerts);
		endResetModel();
		return;
	}

	/* Same endpoint set and order — keep the previous cache to diff against,
	 * then swap in the new data BEFORE emitting dataChanged() so that any
	 * data() call triggered by the signal (synchronous or deferred) already
	 * observes the new values. */
	auto old_endpoints_cache = std::move(m_endpoints_cache);
	auto old_snapshot_cache  = std::move(m_snapshot_cache);
	auto old_alert_cache     = std::move(m_alert_cache);

	m_endpoints_cache = std::move(new_endpoints);
	m_snapshot_cache  = std::move(new_snapshots);
	m_alert_cache     = std::move(new_alerts);

	for (int row = 0; row < static_cast<int>(m_endpoints_cache.size()); ++row) {
		const Endpoint &old_ep = old_endpoints_cache[static_cast<size_t>(row)];
		const Endpoint &new_ep = m_endpoints_cache[static_cast<size_t>(row)];

		auto old_it = old_snapshot_cache.find(old_ep.id);
		auto new_it = m_snapshot_cache.find(new_ep.id);
		const HealthSnapshot *old_snap = old_it != old_snapshot_cache.end() ? &old_it->second : nullptr;
		const HealthSnapshot *new_snap = new_it != m_snapshot_cache.end()   ? &new_it->second   : nullptr;

		int first_changed = -1;
		int last_changed  = -1;
		auto mark_changed = [&](int col) {
			if (first_changed < 0 || col < first_changed)
				first_changed = col;
			if (col > last_changed)
				last_changed = col;
		};

		if (old_ep.name != new_ep.name)
			mark_changed(COL_NAME);

		const bool old_present = old_snap != nullptr;
		const bool new_present = new_snap != nullptr;

		if (old_present != new_present) {
			/* Snapshot appeared/disappeared for this endpoint (e.g. the very
			 * first poll tick after add()) — every data-derived column may
			 * flip between its "—" placeholder and a real value. */
			mark_changed(COL_STATUS);
			mark_changed(COL_TARGET);
			mark_changed(COL_ACTUAL);
			mark_changed(COL_DROPS);
			mark_changed(COL_RECONNECTS);
			mark_changed(COL_UPTIME);
		} else if (old_present && new_present) {
			if (old_snap->state != new_snap->state)
				mark_changed(COL_STATUS);

			if (old_snap->target_bitrate != new_snap->target_bitrate)
				mark_changed(COL_TARGET);

			const bool old_actual_shown = old_snap->state == OutputState::Live;
			const bool new_actual_shown = new_snap->state == OutputState::Live;
			if (old_actual_shown != new_actual_shown ||
			    (new_actual_shown &&
			     static_cast<int>(old_snap->actual_bitrate) != static_cast<int>(new_snap->actual_bitrate)))
				mark_changed(COL_ACTUAL);

			if (old_snap->dropped_frames != new_snap->dropped_frames)
				mark_changed(COL_DROPS);

			if (old_snap->reconnect_count != new_snap->reconnect_count)
				mark_changed(COL_RECONNECTS);

			const bool old_uptime_shown = old_snap->state != OutputState::Idle;
			const bool new_uptime_shown = new_snap->state != OutputState::Idle;
			if (old_uptime_shown != new_uptime_shown ||
			    (new_uptime_shown && old_snap->uptime_sec != new_snap->uptime_sec))
				mark_changed(COL_UPTIME);
		}

		/* data()'s Qt::ToolTipRole is not column-scoped — it reports
		 * snap->last_error for every column in the row (see data() above).
		 * If the error text changed, widen the emission to the full row so
		 * every column's tooltip actually refreshes. */
		const std::string old_err = old_snap ? old_snap->last_error : std::string();
		const std::string new_err = new_snap ? new_snap->last_error : std::string();
		if (old_err != new_err) {
			first_changed = 0;
			last_changed  = COL_COUNT - 1;
		}

		/* An alert raised or cleared changes the row background and tooltip in every column. */
		auto old_alert = old_alert_cache.find(old_ep.id);
		auto new_alert = m_alert_cache.find(new_ep.id);
		const bool old_has = old_alert != old_alert_cache.end();
		const bool new_has = new_alert != m_alert_cache.end();
		if (old_has != new_has || (old_has && !same_alert(old_alert->second, new_alert->second))) {
			first_changed = 0;
			last_changed  = COL_COUNT - 1;
		}

		if (first_changed >= 0) {
			QModelIndex top_left     = index(row, first_changed);
			QModelIndex bottom_right = index(row, last_changed);
			emit dataChanged(top_left, bottom_right,
			                  {Qt::DisplayRole, Qt::ForegroundRole, Qt::ToolTipRole, Qt::BackgroundRole});
		}
	}
}

/* -----------------------------------------------------------------------
 * HealthTab
 * ----------------------------------------------------------------------- */
HealthTab::HealthTab(EndpointRegistry &registry,
                     HealthSampler    &sampler,
                     AlertMonitor     &alerts,
                     QWidget          *parent)
	: QWidget(parent)
	, m_registry(registry)
	, m_sampler(sampler)
	, m_alerts(alerts)
{
	setup_ui();
}

void HealthTab::setup_ui()
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	/* Live-grid table */
	m_model = new HealthTableModel(m_registry, m_sampler, m_alerts, this);
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

	/* Alert settings (persisted by AlertMonitor in alerts.json). Set the state before connecting. */
	const AlertSettings alert_settings = m_alerts.settings();
	m_sound_check = new QCheckBox(tr("Play a sound on alerts"), this);
	m_sound_check->setChecked(alert_settings.sound_enabled);
	m_ws_check = new QCheckBox(tr("Send alerts to obs-websocket (vendor event)"), this);
	m_ws_check->setChecked(alert_settings.websocket_enabled);
	layout->addWidget(m_sound_check);
	layout->addWidget(m_ws_check);
	auto apply_alert_settings = [this] {
		AlertSettings s;
		s.sound_enabled     = m_sound_check->isChecked();
		s.websocket_enabled = m_ws_check->isChecked();
		m_alerts.set_settings(s);
	};
	connect(m_sound_check, &QCheckBox::toggled, this, apply_alert_settings);
	connect(m_ws_check, &QCheckBox::toggled, this, apply_alert_settings);

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
