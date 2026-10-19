/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "ImportScenesDialog.hpp"

#include "../pipeline/CanvasLookup.hpp"
#include "../pipeline/CanvasSceneNames.hpp"
#include "../pipeline/OwnedCanvas.hpp"
#include "../plugin-support.h"

#include <obs.h>

#include <QtCore/QString>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <string>
#include <vector>

namespace smulti {

namespace {

constexpr int kRoleUuid = Qt::UserRole;
constexpr int kRoleScene = Qt::UserRole + 1;

std::vector<std::string> sorted_scene_names(obs_canvas_t *canvas)
{
	std::vector<std::string> names = canvas_scene_names(canvas);
	std::sort(names.begin(), names.end());
	return names;
}

bool find_group_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	if (obs_sceneitem_is_group(item)) {
		*static_cast<bool *>(param) = true;
		return false;
	}
	return true;
}

bool scene_has_group(obs_scene_t *scene)
{
	bool found = false;
	obs_scene_enum_items(scene, find_group_item, &found);
	return found;
}

/** True when the source belongs to exactly this canvas (pointer and uuid). */
bool canvas_is(obs_source_t *source, obs_canvas_t *expected, const std::string &expected_uuid)
{
	obs_canvas_t *c = source ? obs_source_get_canvas(source) : nullptr; // strong ref
	bool same = false;
	if (c) {
		const char *u = obs_canvas_get_uuid(c);
		same = (c == expected) && u && expected_uuid == u;
		obs_canvas_release(c);
	}
	return same;
}

ImportOutcome failed(ImportOutcome out, const std::string &msg)
{
	out.ok = false;
	out.message = msg;
	return out;
}

} // namespace

/* ---- dialog ----------------------------------------------------------- */

ImportScenesDialog::ImportScenesDialog(const OwnedCanvas &owned, QWidget *parent) : QDialog(parent)
{
	setWindowTitle(tr("Import scenes from canvas"));
	resize(420, 460);

	auto *layout = new QVBoxLayout(this);
	auto *intro = new QLabel(tr("Select the scenes to copy into the vertical canvas. The original scenes are not "
				    "changed. Sources are shared with the original, not duplicated."),
				 this);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	m_tree = new QTreeWidget(this);
	m_tree->setHeaderHidden(true);
	layout->addWidget(m_tree, 1);

	m_hint = new QLabel(this);
	m_hint->setWordWrap(true);
	layout->addWidget(m_hint);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	buttons->button(QDialogButtonBox::Ok)->setText(tr("Import"));
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);

	populate(owned.uuid());
}

void ImportScenesDialog::populate(const std::string &owned_uuid)
{
	m_tree->clear();
	bool any_canvas = false;
	bool any_group_scene = false;

	for (const CanvasInfo &info : snapshot_canvases()) {
		if (info.is_main || info.uuid.empty() || info.uuid == owned_uuid)
			continue;
		obs_canvas_t *canvas = acquire_canvas(info.uuid);
		if (!canvas)
			continue;

		auto *parent = new QTreeWidgetItem(m_tree);
		parent->setText(0, QString::fromStdString(info.name));
		parent->setFlags(Qt::ItemIsEnabled);
		any_canvas = true;

		for (const std::string &scene_name : sorted_scene_names(canvas)) {
			bool has_group = false;
			obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas, scene_name.c_str());
			if (scene) {
				has_group = scene_has_group(scene);
				obs_scene_release(scene);
			}
			auto *item = new QTreeWidgetItem(parent);
			item->setText(0, QString::fromStdString(scene_name));
			item->setData(0, kRoleUuid, QString::fromStdString(info.uuid));
			item->setData(0, kRoleScene, QString::fromStdString(scene_name));
			if (has_group) {
				any_group_scene = true;
				item->setFlags(Qt::NoItemFlags);
				item->setText(0, tr("%1  (contains a group, not importable)").arg(item->text(0)));
			} else {
				item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
				item->setCheckState(0, Qt::Unchecked);
			}
		}
		obs_canvas_release(canvas);
	}
	m_tree->expandAll();

	QString hint;
	if (!any_canvas)
		hint = tr("There is no other canvas to import from.");
	else if (any_group_scene)
		hint = tr("Scenes with groups cannot be imported safely: a group is shared with the original.");
	m_hint->setText(hint);
	m_hint->setVisible(!hint.isEmpty());
}

std::vector<ImportSelection> ImportScenesDialog::selection() const
{
	std::vector<ImportSelection> out;
	for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
		const QTreeWidgetItem *parent = m_tree->topLevelItem(i);
		for (int j = 0; j < parent->childCount(); ++j) {
			const QTreeWidgetItem *c = parent->child(j);
			if (c->checkState(0) != Qt::Checked)
				continue;
			out.push_back({c->data(0, kRoleUuid).toString().toStdString(),
				       c->data(0, kRoleScene).toString().toStdString()});
		}
	}
	return out;
}

/* ---- import ----------------------------------------------------------- */

ImportOutcome import_scene(OwnedCanvas &owned, const ImportSelection &sel)
{
	ImportOutcome out;
	out.scene_name = sel.scene_name;

	obs_canvas_t *dst = owned.get_ref();
	if (!dst)
		return failed(out, "The vertical canvas is not available.");
	const std::string dst_uuid = owned.uuid();
	const char *dst_uuid_c = obs_canvas_get_uuid(dst);
	if (dst_uuid.empty() || !dst_uuid_c || dst_uuid != dst_uuid_c) {
		obs_canvas_release(dst);
		return failed(out, "The vertical canvas changed; nothing was imported.");
	}

	obs_canvas_t *src = acquire_canvas(sel.canvas_uuid);
	if (!src) {
		obs_canvas_release(dst);
		return failed(out, "The source canvas no longer exists.");
	}

	ImportOutcome result = out;
	obs_scene_t *orig = nullptr;
	obs_scene_t *copy = nullptr;

	do {
		/* Never import from the owned or the main canvas. */
		if (src == dst || sel.canvas_uuid == dst_uuid ||
		    (obs_canvas_get_flags(src) & static_cast<uint32_t>(MAIN)) != 0) {
			result = failed(out, "This canvas cannot be an import source.");
			break;
		}

		orig = obs_canvas_get_scene_by_name(src, sel.scene_name.c_str());
		if (!orig) {
			result = failed(out, "The scene no longer exists.");
			break;
		}
		obs_source_t *orig_src = obs_scene_get_source(orig);
		if (!canvas_is(orig_src, src, sel.canvas_uuid)) {
			result = failed(out, "The scene does not belong to the selected canvas.");
			break;
		}
		if (scene_has_group(orig)) {
			result = failed(
				out,
				"The scene contains a group; the group is shared with the original and cannot be moved safely.");
			break;
		}

		const std::vector<std::string> before = sorted_scene_names(src);
		const std::string tmp = unique_name_in(src, sel.scene_name + " [smulti import]");

		/* The copy is created in the SOURCE canvas (libobs: create_id on the original's canvas) and shares
		 * the child sources. Only the copy is ever handed to move/rename; the original is read-only here. */
		copy = obs_scene_duplicate(orig, tmp.c_str(), OBS_SCENE_DUP_REFS);
		if (!copy) {
			result = failed(out, "Could not duplicate the scene.");
			break;
		}
		obs_source_t *copy_src = obs_scene_get_source(copy);
		if (copy_src == orig_src) {
			result = failed(out, "Internal error: the duplicate is the original; nothing was moved.");
			break;
		}

		obs_canvas_move_scene(copy, dst); // returns void: verify by looking the copy up
		if (!canvas_is(copy_src, dst, dst_uuid)) {
			result = failed(out, "The copy could not be moved to the vertical canvas.");
			result.stranded = tmp; // reported, never removed from the foreign canvas
			obs_log(LOG_WARNING, "import: copy \"%s\" stayed in canvas \"%s\"; not removed", tmp.c_str(),
				obs_canvas_get_name(src));
			break;
		}

		const std::string wanted = unique_name_in(dst, sel.scene_name, " (", ")");
		obs_source_set_name(copy_src, wanted.c_str());
		const char *actual = obs_source_get_name(copy_src);
		result.ok = true;
		result.final_name = actual ? actual : wanted;
		obs_log(LOG_INFO, "import: scene \"%s\" of canvas \"%s\" copied as \"%s\"", sel.scene_name.c_str(),
			obs_canvas_get_name(src), result.final_name.c_str());

		/* INV-6: the source canvas must list exactly the scenes it listed before. */
		if (sorted_scene_names(src) != before) {
			result.source_changed = true;
			obs_log(LOG_ERROR, "import: scene names of canvas \"%s\" differ after importing \"%s\"",
				obs_canvas_get_name(src), sel.scene_name.c_str());
		}
	} while (false);

	if (copy)
		obs_scene_release(copy);
	if (orig)
		obs_scene_release(orig);
	obs_canvas_release(src);
	obs_canvas_release(dst);
	return result;
}

} // namespace smulti
