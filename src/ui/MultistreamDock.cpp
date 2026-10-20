/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "MultistreamDock.hpp"
#include "HealthTab.hpp"
#include "ConfigTab.hpp"
#include "../pipeline/AlertMonitor.hpp"

#include <QVBoxLayout>
#include <QTabWidget>

namespace smulti {

MultistreamDock::MultistreamDock(EndpointRegistry &registry,
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

void MultistreamDock::setup_ui()
{
	setObjectName("StreamMulticastDock");
	setMinimumWidth(320);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(0);

	m_tabs = new QTabWidget(this);
	m_tabs->setDocumentMode(false);

	m_health_tab = new HealthTab(m_registry, m_sampler, m_alerts, m_tabs);
	m_config_tab = new ConfigTab(m_registry, m_tabs);

	m_tabs->addTab(m_health_tab, tr("Health"));
	m_tabs->addTab(m_config_tab, tr("Configure"));

	/* Default to Health tab */
	m_tabs->setCurrentIndex(0);

	connect(&m_alerts, &AlertMonitor::levels_changed, this, [this] { update_health_tab_title(); });
	update_health_tab_title();

	layout->addWidget(m_tabs);
	setLayout(layout);
}

void MultistreamDock::update_health_tab_title()
{
	QString title = tr("Health");
	switch (m_alerts.max_level()) {
	case AlertLevel::Warning:  title += QStringLiteral(" (!)"); break;
	case AlertLevel::Critical: title += QStringLiteral(" (!!)"); break;
	default: break;
	}
	m_tabs->setTabText(m_tabs->indexOf(m_health_tab), title);
}

} // namespace smulti
