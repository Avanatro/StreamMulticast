/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalCanvasDock.hpp"

#include "CanvasPreview.hpp"
#include "VerticalSceneList.hpp"

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

	m_tabs = new QTabWidget(this);
	m_scenes = new VerticalSceneList(canvas, m_tabs);
	m_tabs->addTab(m_scenes, tr("Scenes"));
	/* Reserved for the source controls (later task). */
	m_tabs->addTab(new QWidget(m_tabs), tr("Sources"));
	layout->addWidget(m_tabs, 2);
}

void VerticalCanvasDock::flush_preview()
{
	if (m_preview)
		m_preview->flush();
}

void VerticalCanvasDock::refresh_scenes()
{
	if (m_scenes)
		m_scenes->refresh();
}

void VerticalCanvasDock::detach_scene_signals()
{
	if (m_scenes)
		m_scenes->detach_signals();
}

void VerticalCanvasDock::shutdown_preview()
{
	if (m_scenes)
		m_scenes->shutdown();
	if (m_preview)
		m_preview->shutdown();
}

} // namespace smulti
