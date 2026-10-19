/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalCanvasDock.hpp"

#include "CanvasPreview.hpp"

#include <QVBoxLayout>

namespace smulti {

VerticalCanvasDock::VerticalCanvasDock(OwnedCanvas &canvas, QWidget *parent) : QWidget(parent)
{
	setObjectName("StreamMulticastVerticalDock");
	setMinimumWidth(240);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	m_preview = new CanvasPreview(canvas, this);
	layout->addWidget(m_preview, 3);

	/* Reserved for the scene list and source controls (later tasks). */
	m_tabs = new QTabWidget(this);
	m_tabs->addTab(new QWidget(m_tabs), tr("Scenes"));
	m_tabs->addTab(new QWidget(m_tabs), tr("Sources"));
	layout->addWidget(m_tabs, 2);
}

void VerticalCanvasDock::shutdown_preview()
{
	if (m_preview)
		m_preview->shutdown();
}

} // namespace smulti
