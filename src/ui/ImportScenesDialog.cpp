/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "ImportScenesDialog.hpp"

#include "../core/CanvasScaleRef.hpp"
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
#include <cmath>
#include <cstring>
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

namespace {

/** Per-scene state of one import: the new scene in the owned canvas and the original's saved items. */
struct ItemCopy {
	obs_scene_t *dst = nullptr;
	obs_data_array_t *saved_items = nullptr; // relative transforms as libobs scene_save wrote them
	uint32_t canvas_w = 0;
	uint32_t canvas_h = 0;
	size_t copied = 0;
	size_t failed = 0;
	size_t without_saved_transform = 0;
};

/** The saved entry of the item with this id, or nullptr (caller releases). */
obs_data_t *saved_item_by_id(obs_data_array_t *items, int64_t id)
{
	const size_t count = items ? obs_data_array_count(items) : 0;
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *it = obs_data_array_item(items, i);
		if (it && obs_data_get_int(it, "id") == id)
			return it;
		obs_data_release(it);
	}
	return nullptr;
}

/**
 * Adds the original's source to the new scene and copies the item data the way libobs duplicate_item_data
 * (obs-scene.c 31.1.1, ~1924-1997) does, through public API only. The new item's scale_ref comes from
 * obs_scene_add, i.e. the owned canvas size. Position, scale and bounds are taken from the original's
 * RELATIVE values (its saved settings), because obs_sceneitem_get_info2 converts them with the source
 * canvas size, which is 0 when that canvas has no video (Aitum Vertical while Aitum is disabled).
 * Not copied, as in the libobs scene duplicate: hotkeys; also not the UI selection flag.
 */
void copy_item(ItemCopy *ctx, obs_sceneitem_t *src_item)
{
	obs_source_t *source = obs_sceneitem_get_source(src_item); // no ref; the original scene holds one
	obs_sceneitem_t *dst_item = source ? obs_scene_add(ctx->dst, source) : nullptr; // owned by the scene
	if (!dst_item) {
		++ctx->failed;
		return;
	}

	obs_sceneitem_defer_update_begin(dst_item);

	/* libobs order: visibility first, before any show/hide transition is attached. */
	if (!obs_sceneitem_visible(src_item))
		obs_sceneitem_set_visible(dst_item, false);

	obs_transform_info info = {};
	obs_sceneitem_get_info2(src_item, &info); // rot, alignment, bounds type/alignment, crop_to_bounds
	obs_data_t *saved = saved_item_by_id(ctx->saved_items, obs_sceneitem_get_id(src_item));
	RelativeItemTransform rel;
	if (saved && read_relative_transform(saved, rel)) {
		const char *id = obs_source_get_id(source);
		rel.nested_scene = id && std::strcmp(id, "scene") == 0;
		const AbsoluteItemTransform abs = relative_to_absolute(rel, ctx->canvas_w, ctx->canvas_h);
		vec2_set(&info.pos, static_cast<float>(abs.pos.x), static_cast<float>(abs.pos.y));
		vec2_set(&info.scale, static_cast<float>(abs.scale.x), static_cast<float>(abs.scale.y));
		vec2_set(&info.bounds, static_cast<float>(abs.bounds.x), static_cast<float>(abs.bounds.y));
	} else {
		/* Absolute-coordinate mode (get_info2 returns the raw values) or no saved entry. */
		++ctx->without_saved_transform;
	}
	obs_data_release(saved);
	obs_sceneitem_set_info2(dst_item, &info);

	obs_sceneitem_crop crop = {};
	obs_sceneitem_get_crop(src_item, &crop);
	obs_sceneitem_set_crop(dst_item, &crop);
	obs_sceneitem_set_scale_filter(dst_item, obs_sceneitem_get_scale_filter(src_item));
	obs_sceneitem_set_blending_method(dst_item, obs_sceneitem_get_blending_method(src_item));
	obs_sceneitem_set_blending_mode(dst_item, obs_sceneitem_get_blending_mode(src_item));

	for (const bool show : {true, false}) {
		obs_source_t *t = obs_sceneitem_get_transition(src_item, show); // no ref
		if (t) {
			/* Private duplicate, as libobs does: the original keeps its own transition. */
			obs_source_t *dup = obs_source_duplicate(t, obs_source_get_name(t), true);
			obs_sceneitem_set_transition(dst_item, show, dup); // takes its own ref
			obs_source_release(dup);
		}
		obs_sceneitem_set_transition_duration(dst_item, show,
						      obs_sceneitem_get_transition_duration(src_item, show));
	}

	obs_sceneitem_set_locked(dst_item, obs_sceneitem_locked(src_item));

	obs_data_t *src_priv = obs_sceneitem_get_private_settings(src_item);
	obs_data_t *dst_priv = obs_sceneitem_get_private_settings(dst_item);
	if (src_priv && dst_priv)
		obs_data_apply(dst_priv, src_priv);
	obs_data_release(dst_priv);
	obs_data_release(src_priv);

	obs_sceneitem_defer_update_end(dst_item);
	++ctx->copied;
}

/** Item refs of a scene, bottom to top. Taken under the scene lock, used after it (as libobs does). */
bool collect_item_ref(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	obs_sceneitem_addref(item);
	static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
	return true;
}

bool count_non_finite_scale(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	vec2 scale = {};
	obs_sceneitem_get_scale(item, &scale);
	if (!std::isfinite(scale.x) || !std::isfinite(scale.y))
		++*static_cast<size_t *>(param);
	return true;
}

} // namespace

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
	obs_data_t *orig_settings = nullptr;
	obs_data_array_t *orig_items = nullptr;

	do {
		/* Never import from the owned or the main canvas. */
		if (src == dst || sel.canvas_uuid == dst_uuid ||
		    (obs_canvas_get_flags(src) & static_cast<uint32_t>(MAIN)) != 0) {
			result = failed(out, "This canvas cannot be an import source.");
			break;
		}

		/* The new items take their scale_ref from the owned canvas size: it must be known. */
		obs_video_info dst_ovi = {};
		if (!obs_canvas_get_video_info(dst, &dst_ovi) || dst_ovi.base_width == 0 || dst_ovi.base_height == 0) {
			result = failed(out, "The vertical canvas has no video yet; nothing was imported.");
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
			result = failed(out, "The scene contains a group; the group would be shared with the original.");
			break;
		}

		const std::vector<std::string> before = sorted_scene_names(src);

		/* Relative item transforms of the original. obs_source_save only refreshes the original's settings
		 * snapshot (the call OBS makes on every collection save); no scene or item state changes. */
		obs_source_save(orig_src);
		orig_settings = obs_source_get_settings(orig_src);
		orig_items = orig_settings ? obs_data_get_array(orig_settings, "items") : nullptr;

		/* Created directly in the owned canvas. Never the libobs scene duplicate (it builds the copy in the source
		 * canvas, whose size gives every item scale_ref {0,0} when that canvas has no video) and never the
		 * libobs canvas move (it leaves the scene's context mutex pointing into the destination canvas,
		 * which obs_canvas_destroy frees before the scene's deferred destroy: use-after-free on close). */
		const std::string wanted = unique_name_in(dst, sel.scene_name, " (", ")");
		copy = obs_canvas_scene_create(dst, wanted.c_str());
		if (!copy) {
			result = failed(out, "Could not create the scene in the vertical canvas.");
			break;
		}
		obs_source_t *copy_src = obs_scene_get_source(copy);
		if (copy_src == orig_src || !canvas_is(copy_src, dst, dst_uuid)) {
			result = failed(out, "Internal error: the new scene is not in the vertical canvas.");
			obs_log(LOG_ERROR, "import: new scene \"%s\" is not in the owned canvas", wanted.c_str());
			break;
		}

		/* Scene level, as the libobs scene duplicate: filters (duplicated) and private settings. */
		obs_source_copy_filters(copy_src, orig_src);
		obs_data_t *src_priv = obs_source_get_private_settings(orig_src);
		obs_data_t *dst_priv = obs_source_get_private_settings(copy_src);
		if (src_priv && dst_priv)
			obs_data_apply(dst_priv, src_priv);
		obs_data_release(dst_priv);
		obs_data_release(src_priv);

		/* Bottom to top, so obs_scene_add (appends on top) keeps the item order. The items are snapshotted
		 * first so the original's scene lock is not held while the new scene is written. */
		ItemCopy ctx;
		ctx.dst = copy;
		ctx.saved_items = orig_items;
		ctx.canvas_w = dst_ovi.base_width;
		ctx.canvas_h = dst_ovi.base_height;
		std::vector<obs_sceneitem_t *> orig_item_refs;
		obs_scene_enum_items(orig, collect_item_ref, &orig_item_refs);
		for (obs_sceneitem_t *item : orig_item_refs) {
			copy_item(&ctx, item);
			obs_sceneitem_release(item);
		}

		size_t non_finite = 0;
		obs_scene_enum_items(copy, count_non_finite_scale, &non_finite);

		const char *actual = obs_source_get_name(copy_src);
		result.ok = true;
		result.final_name = actual ? actual : wanted;
		obs_log(LOG_INFO, "import: scene \"%s\" of canvas \"%s\" copied as \"%s\" (%zu items)",
			sel.scene_name.c_str(), obs_canvas_get_name(src), result.final_name.c_str(), ctx.copied);
		if (ctx.failed > 0)
			obs_log(LOG_WARNING, "import: %zu item(s) of \"%s\" could not be added", ctx.failed,
				sel.scene_name.c_str());
		if (ctx.without_saved_transform > 0)
			obs_log(LOG_INFO, "import: %zu item(s) of \"%s\" copied with absolute transform values",
				ctx.without_saved_transform, sel.scene_name.c_str());
		if (non_finite > 0)
			obs_log(LOG_WARNING, "import: %zu item(s) of \"%s\" have a non-finite scale", non_finite,
				result.final_name.c_str());

		/* INV-6: the source canvas must list exactly the scenes it listed before. */
		if (sorted_scene_names(src) != before) {
			result.source_changed = true;
			obs_log(LOG_ERROR, "import: scene names of canvas \"%s\" differ after importing \"%s\"",
				obs_canvas_get_name(src), sel.scene_name.c_str());
		}
	} while (false);

	obs_data_array_release(orig_items);
	obs_data_release(orig_settings);
	if (copy)
		obs_scene_release(copy); // the owned canvas keeps its own ref (SCENE_REF)
	if (orig)
		obs_scene_release(orig);
	obs_canvas_release(src);
	obs_canvas_release(dst);
	return result;
}

} // namespace smulti
