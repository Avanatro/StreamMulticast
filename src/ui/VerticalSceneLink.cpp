/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalSceneLink.hpp"

#include "../core/SceneLink.hpp"
#include "../core/VerticalTransition.hpp"
#include "../pipeline/Autosave.hpp"
#include "../pipeline/CanvasSceneNames.hpp"
#include "../pipeline/OwnedCanvas.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <algorithm>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <string>
#include <vector>

namespace smulti {

namespace {

enum Kind { KindAuto = 0, KindKeep = 1, KindScene = 2 };
constexpr int kSceneNameRole = Qt::UserRole + 1;
constexpr int kTransitionModeRole = Qt::UserRole;     ///< VerticalTransitionMode as int
constexpr int kTransitionNameRole = Qt::UserRole + 1; ///< frontend transition name (custom mode)

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

/**
 * The 16:9 file of the stinger a chosen vertical video belongs to: the custom transition when it is a
 * stinger, else the current frontend transition when it is one, else the first stinger. "" when none.
 * Releases every ref it takes.
 */
std::string target_stinger_file(const smulti::VerticalTransitionSettings &settings)
{
	auto stinger_path = [](obs_source_t *tr) -> std::string {
		const char *id = tr ? obs_source_get_id(tr) : nullptr;
		if (!id || std::string(id) != smulti::kStingerTransitionId)
			return {};
		obs_data_t *s = obs_source_get_settings(tr);
		const char *p = s ? obs_data_get_string(s, "path") : nullptr;
		std::string out = p ? p : "";
		obs_data_release(s);
		return out;
	};
	std::string custom, first;
	obs_frontend_source_list list = {};
	obs_frontend_get_transitions(&list);
	for (size_t i = 0; i < list.sources.num; ++i) {
		obs_source_t *tr = list.sources.array[i];
		const std::string path = stinger_path(tr);
		if (path.empty())
			continue;
		if (first.empty())
			first = path;
		const char *name = obs_source_get_name(tr);
		if (settings.mode == smulti::VerticalTransitionMode::Custom && name && settings.custom_name == name)
			custom = path;
	}
	obs_frontend_source_list_free(&list);
	if (!custom.empty())
		return custom;
	obs_source_t *current = obs_frontend_get_current_transition();
	std::string out = stinger_path(current);
	obs_source_release(current);
	return out.empty() ? first : out;
}

/** Names of the frontend transitions. Releases every ref it takes. */
std::vector<std::string> frontend_transition_names()
{
	std::vector<std::string> out;
	obs_frontend_source_list list = {};
	obs_frontend_get_transitions(&list);
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

	auto *form = new QFormLayout();
	form->setContentsMargins(0, 0, 0, 0);
	m_transition = new QComboBox(this);
	m_transition->setToolTip(tr("Follow main: the vertical canvas uses the same transition (type, settings and "
				    "duration) as the main canvas and starts it at the same moment. Cut: always instant. "
				    "A named transition: always that one."));
	form->addRow(tr("Transition:"), m_transition);

	auto *stinger_row = new QHBoxLayout();
	m_stinger_path = new QLineEdit(this);
	m_stinger_path->setPlaceholderText(tr("(auto)"));
	m_stinger_path->setClearButtonEnabled(true);
	m_stinger_path->setToolTip(tr("Video for a stinger transition on the vertical canvas. Empty (auto): "
				      "\"<stinger>-vertical.<ext>\" or \"stinger-vertical.<ext>\" next to the main stinger "
				      "file (same extension); without one the vertical canvas fades instead of stretching the "
				      "16:9 video. A chosen file applies only to the stinger shown below."));
	auto *browse = new QPushButton(tr("Browse..."), this);
	stinger_row->addWidget(m_stinger_path, 1);
	stinger_row->addWidget(browse);
	form->addRow(tr("Vertical stinger video:"), stinger_row);
	m_stinger_for = new QLabel(this);
	m_stinger_for->setWordWrap(true);
	form->addRow(QString(), m_stinger_for);
	layout->addLayout(form);

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
	connect(m_transition, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&VerticalSceneLink::on_transition_changed);
	connect(m_stinger_path, &QLineEdit::editingFinished, this, &VerticalSceneLink::on_stinger_path_changed);
	connect(browse, &QPushButton::clicked, this, &VerticalSceneLink::on_browse_stinger);

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

	const VerticalTransitionSettings transition = m_canvas.transition_settings();
	m_transition->blockSignals(true);
	m_transition->clear();
	m_transition->addItem(tr("Follow main (default)"), static_cast<int>(VerticalTransitionMode::Follow));
	m_transition->addItem(tr("Cut"), static_cast<int>(VerticalTransitionMode::Cut));
	int transition_index = transition.mode == VerticalTransitionMode::Cut ? 1 : 0;
	for (const std::string &name : frontend_transition_names()) {
		m_transition->addItem(QString::fromStdString(name), static_cast<int>(VerticalTransitionMode::Custom));
		m_transition->setItemData(m_transition->count() - 1, QString::fromStdString(name), kTransitionNameRole);
		if (transition.mode == VerticalTransitionMode::Custom && transition.custom_name == name)
			transition_index = m_transition->count() - 1;
	}
	if (transition.mode == VerticalTransitionMode::Custom && transition_index == 0) {
		/* Saved transition no longer exists: keep it visible; the vertical canvas follows main meanwhile. */
		const QString missing = QString::fromStdString(transition.custom_name);
		m_transition->addItem(tr("%1 (missing)").arg(missing), static_cast<int>(VerticalTransitionMode::Custom));
		m_transition->setItemData(m_transition->count() - 1, missing, kTransitionNameRole);
		transition_index = m_transition->count() - 1;
	}
	m_transition->setCurrentIndex(transition_index);
	m_transition->blockSignals(false);
	m_stinger_path->setText(QString::fromStdString(transition.stinger_path));
	m_stinger_for->setText(transition.stinger_path.empty() || transition.stinger_source.empty()
				       ? QString()
				       : tr("For the stinger %1").arg(QString::fromStdString(transition.stinger_source)));
	m_stinger_for->setVisible(!m_stinger_for->text().isEmpty());

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
	request_autosave("link settings");
	for (int r = 0; r < m_table->rowCount(); ++r) {
		if (QWidget *w = m_table->cellWidget(r, 1))
			w->setEnabled(checked);
	}
	m_canvas.follow_program_scene(); // takes effect now, not only at the next scene change
}

void VerticalSceneLink::on_transition_changed(int index)
{
	if (m_shutdown.load() || m_refreshing || index < 0)
		return;
	VerticalTransitionSettings settings = m_canvas.transition_settings();
	settings.mode = static_cast<VerticalTransitionMode>(m_transition->itemData(index, kTransitionModeRole).toInt());
	settings.custom_name = settings.mode == VerticalTransitionMode::Custom
				       ? m_transition->itemData(index, kTransitionNameRole).toString().toStdString()
				       : std::string();
	m_canvas.set_transition_settings(settings); // persisted with the collection
	request_autosave("transition settings");
	m_canvas.prepare_transition();             // load it before the next switch
}

void VerticalSceneLink::on_stinger_path_changed()
{
	if (m_shutdown.load() || m_refreshing)
		return;
	VerticalTransitionSettings settings = m_canvas.transition_settings();
	const std::string path = m_stinger_path->text().trimmed().toStdString();
	if (path == settings.stinger_path)
		return;
	settings.stinger_path = path; // "" = auto
	settings.stinger_source = path.empty() ? std::string() : target_stinger_file(settings);
	m_canvas.set_transition_settings(settings);
	request_autosave("stinger path");
	m_canvas.prepare_transition();
	refresh(); // shows which stinger it applies to
}

void VerticalSceneLink::on_browse_stinger()
{
	if (m_shutdown.load())
		return;
	/* The dialog runs an event loop: no libobs reference is held across it. */
	const QString file = QFileDialog::getOpenFileName(this, tr("Vertical stinger video"), m_stinger_path->text(),
							  tr("Video files (*.webm *.mov *.mp4 *.mkv *.avi);;All files (*)"));
	if (m_shutdown.load() || file.isEmpty())
		return;
	m_stinger_path->setText(file);
	on_stinger_path_changed();
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
	request_autosave("scene link");
	/* Only the row of the current program scene can change what is on screen; editing another row
	 * must not undo a manual "Set active". */
	if (key == program_scene_name())
		m_canvas.follow_program_scene();
}

} // namespace smulti
