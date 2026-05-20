/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../pipeline/HealthSampler.hpp"

#include <QtWidgets/QWidget>
#include <QtWidgets/QTableView>
#include <QtCore/QAbstractTableModel>
#include <QtCore/QTimer>
#include <QtWidgets/QLabel>
#include <vector>
#include <string>

namespace smulti {

class EndpointRegistry;

/**
 * HealthTableModel — Qt model feeding the live-grid QTableView.
 *
 * Columns (in order):
 *   0: Name
 *   1: Status (LED icon + text)
 *   2: Target kbps
 *   3: Actual kbps (rolling 5s avg)
 *   4: Dropped Frames
 *   5: Reconnects
 *   6: Uptime
 *
 * refresh() fetches a snapshot from HealthSampler and calls
 * beginResetModel/endResetModel.  Called from HealthTab's 500 ms QTimer.
 *
 * Thread safety: refresh() is always called from the Qt main thread
 * (via QTimer → queued connection ensures this).
 */
class HealthTableModel : public QAbstractTableModel {
	Q_OBJECT

public:
	explicit HealthTableModel(EndpointRegistry &registry,
	                          HealthSampler    &sampler,
	                          QObject          *parent = nullptr);

	/* QAbstractTableModel overrides */
	int      rowCount   (const QModelIndex &parent = {}) const override;
	int      columnCount(const QModelIndex &parent = {}) const override;
	QVariant data       (const QModelIndex &index, int role = Qt::DisplayRole) const override;
	QVariant headerData (int section, Qt::Orientation orientation,
	                     int role = Qt::DisplayRole)  const override;

	/** Refresh the snapshot data — call from UI timer */
	void refresh();

private:
	EndpointRegistry &m_registry;
	HealthSampler    &m_sampler;

	std::vector<Endpoint>                            m_endpoints_cache;
	std::unordered_map<std::string, HealthSnapshot>  m_snapshot_cache;

	static constexpr int COL_NAME       = 0;
	static constexpr int COL_STATUS     = 1;
	static constexpr int COL_TARGET     = 2;
	static constexpr int COL_ACTUAL     = 3;
	static constexpr int COL_DROPS      = 4;
	static constexpr int COL_RECONNECTS = 5;
	static constexpr int COL_UPTIME     = 6;
	static constexpr int COL_COUNT      = 7;

	QString state_label(OutputState s) const;
	QColor  state_color(OutputState s) const;
};

/**
 * HealthTab — Tab 1 of the MultistreamDock.
 *
 * Contains:
 *   - QTableView backed by HealthTableModel
 *   - Footer label with cross-sell link to Stream Health Doctor
 *   - 500 ms QTimer that calls HealthTableModel::refresh()
 */
class HealthTab : public QWidget {
	Q_OBJECT

public:
	explicit HealthTab(EndpointRegistry &registry,
	                   HealthSampler    &sampler,
	                   QWidget          *parent = nullptr);
	~HealthTab() override = default;

private slots:
	void on_refresh_timer();

private:
	void setup_ui();

	EndpointRegistry   &m_registry;
	HealthSampler      &m_sampler;

	QTableView         *m_table      {nullptr};
	HealthTableModel   *m_model      {nullptr};
	QLabel             *m_footer     {nullptr};
	QTimer             *m_timer      {nullptr};
};

} // namespace smulti
