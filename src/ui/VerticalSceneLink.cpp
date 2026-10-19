/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalSceneLink.hpp"

#include "../core/SceneLink.hpp"
#include "../pipeline/CanvasSceneNames.hpp"
#include "../pipeline/OwnedCanvas.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <algorithm>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <string>
#include <vector>

namespace smulti {

namespace {

enum Kind { KindAuto = 0, KindKeep = 1, KindScene = 2 };
constexpr int kSceneNameRole = Qt::UserRole + 1;

/** Name of the current program scene ("" when none). Releases the ref it takes. */
std::string program_scene_name()
{
	std::string out;
	obs_source_t *program = obs_frontend_get_current_scene();
	if (program) {
		const char *n = obs_source_get_name(program);
		out = n ? n : "";
		obs_source_release(program);
	}
	return out;
}

/** Scene names of the main canvas. Releases every ref it takes. */
std::vector<std::string> main_scene_names()
{
	std::vector<std::string> out;
	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; ++i) {
		const char *name = obs_source_get_name(list.sources.array[i]);
		if (name)
			out.emplace_back(name);
	}
	obs_frontend_source_list_free(&list);
	return out;
}

} // namespace

VerticalSceneLink::VerticalSceneLink(OwnedCanvas &canvas, QWidget *parent) : QWidget(parent), m_canvas(canvas)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);

	const QString tip = tr("When the main program scene changes, the vertical canvas switches to the linked scene. "
			       "A scene chosen manually with \"Set active\" stays until the next program scene change.");

	m_follow = new QCheckBox(tr("Follow main scene"), this);
	m_follow->setToolTip(tip);
	layout->addWidget(m_follow);

	m_wait_stinger = new QCheckBox(tr("Wait for stinger transition point"), this);
	m_wait_stinger->setToolTip(tr("Off: the vertical canvas switches at the same moment the main transition starts. "
				      "On: with a stinger transition it switches at the stinger's transition point instead."));
	layout->addWidget(m_wait_stinger);

	m_table = new QTableWidget(0, 2, this);
	m_table->setHorizontalHeaderLabels({tr("Main scene"), tr("Vertical scene")});
	m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	m_table->horizontalHeader()->setMinimumSectionSize(120); // keep the dropdown text from being clipped
	m_table->verticalHeader()->setVisible(false);
	m_table->setSelectionMode(QAbstractItemView::NoSelection);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setToolTip(tip);
	layout->addWidget(m_table, 1);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	m_status->setVisible(false);
	layout->addWidget(m_status);

	connect(m_follow, &QCheckBox::toggled, this, &VerticalSceneLink::on_follow_toggled);
	connect(m_wait_stinger, &QCheckBox::toggled, this, &VerticalSceneLink::on_wait_stinger_toggled);

	refresh();
}

VerticalSceneLink::~VerticalSceneLink()
{
	shutdown();
}

void VerticalSceneLink::shutdown()
{
	m_shutdown = true;
}

void VerticalSceneLink::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	refresh();
}

void VerticalSceneLink::refresh()
{
	if (m_shutdown.load() || m_refreshing)
		return;
	m_refreshing = true;

	const SceneLinkSettings settings = m_canvas.link_settings();

	std::vector<std::string> verticals;
	bool have_canvas = false;
	obs_canvas_t *canvas = m_canvas.get_ref();
	if (canvas) {
		have_canvas = true;
		verticals = canvas_scene_names(canvas);
		obs_canvas_release(canvas);
	}
	const std::vector<std::string> mains = main_scene_names();

	m_follow->blockSignals(true);
	m_follow->setChecked(settings.enabled);
	m_follow->blockSignals(false);
	m_wait_stinger->blockSignals(true);
	m_wait_stinger->setChecked(settings.wait_for_stinger);
	m_wait_stinger->setEnabled(settings.enabled);
	m_wait_stinger->blockSignals(false);

	m_table->setRowCount(0);
	m_table->setRowCount(static_cast<int>(mains.size()));
	int row = 0;
	for (const std::string &main : mains) {
		const QString main_name = QString::fromStdString(main);
		m_table->setItem(row, 0, new QTableWidgetItem(main_name));

		auto *combo = new QComboBox(m_table);
		/* No colours here: the OBS theme styles the combo. Only guarantee it has room for its text. */
		combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		combo->setMinimumContentsLength(12);
		combo->addItem(tr("(auto)"), KindAuto);
		combo->addItem(tr("(keep current)"), KindKeep);
		for (const std::string &v : verticals) {
			combo->addItem(QString::fromStdString(v), KindScene);
			combo->setItemData(combo->count() - 1, QString::fromStdString(v), kSceneNameRole);
		}

		int selected = 0;
		const auto it = settings.explicit_map.find(main);
		if (it != settings.explicit_map.end()) {
			if (it->second.empty()) {
				selected = KindKeep;
			} else {
				const QString target = QString::fromStdString(it->second);
				int found = -1;
				for (int i = 2; i < combo->count(); ++i) {
					if (combo->itemData(i, kSceneNameRole).toString() == target)
						found = i;
				}
				if (found < 0) {
					/* Entry names a deleted scene: keep it visible; resolution falls back to auto. */
					combo->addItem(tr("%1 (missing)").arg(target), KindScene);
					combo->setItemData(combo->count() - 1, target, kSceneNameRole);
					found = combo->count() - 1;
				}
				selected = found;
			}
		}
		combo->setCurrentIndex(selected);
		combo->setEnabled(settings.enabled);

		connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			[this, combo, main_name](int index) {
				if (index < 0)
					return;
				on_choice_changed(main_name, combo->itemData(index).toInt(),
						  combo->itemData(index, kSceneNameRole).toString());
			});
		m_table->setCellWidget(row, 1, combo);
		/* The theme gives combos a fixed height plus margins; a default-height row would squash the text. */
		m_table->setRowHeight(row, std::max(m_table->verticalHeader()->defaultSectionSize(),
						    combo->sizeHint().height() + 4));
		++row;
	}

	m_status->setText(have_canvas ? QString() : tr("The vertical canvas is not available yet."));
	m_status->setVisible(!have_canvas);
	m_refreshing = false;
}

void VerticalSceneLink::on_follow_toggled(bool checked)
{
	if (m_shutdown.load() || m_refreshing)
		return;
	SceneLinkSettings settings = m_canvas.link_settings();
	settings.enabled = checked;
	m_canvas.set_link_settings(settings);
	m_wait_stinger->setEnabled(checked);
	for (int r = 0; r < m_table->rowCount(); ++r) {
		if (QWidget *w = m_table->cellWidget(r, 1))
			w->setEnabled(checked);
	}
	m_canvas.follow_program_scene(); // takes effect now, not only at the next scene change
}

void VerticalSceneLink::on_wait_stinger_toggled(bool checked)
{
	if (m_shutdown.load() || m_refreshing)
		return;
	SceneLinkSettings settings = m_canvas.link_settings();
	settings.wait_for_stinger = checked;
	m_canvas.set_link_settings(settings); // applies from the next transition; persisted with the collection
}

void VerticalSceneLink::on_choice_changed(const QString &main_scene, int kind, const QString &vertical_scene)
{
	if (m_shutdown.load() || m_refreshing)
		return;
	SceneLinkSettings settings = m_canvas.link_settings();
	const std::string key = main_scene.toStdString();
	switch (kind) {
	case KindAuto:
		settings.explicit_map.erase(key);
		break;
	case KindKeep:
		settings.explicit_map[key] = "";
		break;
	default:
		settings.explicit_map[key] = vertical_scene.toStdString();
		break;
	}
	m_canvas.set_link_settings(settings);
	/* Only the row of the current program scene can change what is on screen; editing another row
	 * must not undo a manual "Set active". */
	if (key == program_scene_name())
		m_canvas.follow_program_scene();
}

} // namespace smulti
