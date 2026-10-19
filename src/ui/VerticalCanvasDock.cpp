/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalCanvasDock.hpp"

#include "CanvasPreview.hpp"
#include "VerticalItemList.hpp"
#include "VerticalSceneLink.hpp"
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
	m_items = new VerticalItemList(canvas, m_tabs);
	m_tabs->addTab(m_items, tr("Sources"));
	m_links = new VerticalSceneLink(canvas, m_tabs);
	m_tabs->addTab(m_links, tr("Links"));
	m_tabs->setTabToolTip(0, tr("\"Set active\" is overridden by the next main scene change while links are on."));
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
	if (m_items)
		m_items->refresh();
	refresh_links();
}

void VerticalCanvasDock::refresh_links()
{
	if (m_links)
		m_links->refresh();
}

void VerticalCanvasDock::detach_scene_signals()
{
	if (m_scenes)
		m_scenes->detach_signals();
	if (m_items)
		m_items->detach_signals();
}

void VerticalCanvasDock::shutdown_preview()
{
	if (m_scenes)
		m_scenes->shutdown();
	if (m_items)
		m_items->shutdown();
	if (m_links)
		m_links->shutdown();
	if (m_preview)
		m_preview->shutdown();
}

} // namespace smulti
