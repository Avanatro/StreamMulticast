/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalItemList.hpp"

#include "CanvasPreview.hpp" // read_preview_item
#include "VerticalCanvasDock.hpp" // VerticalSelection
#include "../core/ItemActions.hpp"
#include "../pipeline/OwnedCanvas.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace smulti {

namespace {

constexpr const char *kCanvasSignals[] = {"channel_change", "source_remove", "source_rename"};
constexpr const char *kSceneSignals[] = {"item_add", "item_remove", "reorder", "item_visible", "refresh"};
constexpr const char *kTransformSignal = "item_transform";

constexpr int kIdRole = Qt::UserRole;
constexpr qint64 kReloadIntervalMs = 33; ///< transform panel / preview overlay reload, ~30 Hz

struct ItemRow {
	int64_t id = 0;
	std::string name;
	bool visible = true;
};

struct ItemRows {
	std::vector<ItemRow> rows; ///< bottom to top, the libobs order
};

bool collect_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	ItemRow row;
	row.id = obs_sceneitem_get_id(item);
	obs_source_t *src = obs_sceneitem_get_source(item); // owned by the item
	const char *n = src ? obs_source_get_name(src) : nullptr;
	row.name = n ? n : "";
	row.visible = obs_sceneitem_visible(item);
	static_cast<ItemRows *>(param)->rows.push_back(std::move(row));
	return true;
}

struct FindItem {
	int64_t id = 0;
	obs_sceneitem_t *found = nullptr;
};

/** Runs under the scene lock; the returned item carries its own ref (caller releases). */
bool find_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *f = static_cast<FindItem *>(param);
	if (obs_sceneitem_get_id(item) != f->id)
		return true;
	obs_sceneitem_addref(item);
	f->found = item;
	return false;
}

struct InputNames {
	std::vector<std::string> names;
};

bool collect_input(void *param, obs_source_t *source)
{
	if (obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT)
		return true;
	const char *n = obs_source_get_name(source);
	if (n && *n)
		static_cast<InputNames *>(param)->names.emplace_back(n);
	return true;
}

/* Names of every source and scene (the OBS name space is shared), for unique default names. */
bool collect_any_name(void *param, obs_source_t *source)
{
	const char *n = obs_source_get_name(source);
	if (n && *n)
		static_cast<InputNames *>(param)->names.emplace_back(n);
	return true;
}

std::vector<std::string> all_source_names()
{
	InputNames out;
	obs_enum_sources(collect_any_name, &out);
	obs_enum_scenes(collect_any_name, &out);
	return out.names;
}

bool name_taken(const std::string &name)
{
	obs_source_t *existing = obs_get_source_by_name(name.c_str());
	if (existing)
		obs_source_release(existing);
	return existing != nullptr;
}

struct UsedOutside {
	std::string source_name;
	bool found = false;
};

/* A main-canvas scene that contains the source (by name, recursively). obs_enum_scenes covers the main
 * canvas only (OBS 31), so the plugin's own canvas never shows up here. */
bool scene_uses_source(void *param, obs_source_t *scene_source)
{
	auto *u = static_cast<UsedOutside *>(param);
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (scene && obs_scene_find_source_recursive(scene, u->source_name.c_str())) {
		u->found = true;
		return false;
	}
	return true;
}

struct NewType {
	std::string id;
	QString label;
};

/* Input types a user can create: not disabled, not deprecated, with video or audio (like OBS's Add menu). */
std::vector<NewType> creatable_types()
{
	std::vector<NewType> out;
	const char *id = nullptr;
	const char *unversioned = nullptr;
	for (size_t i = 0; obs_enum_input_types2(i, &id, &unversioned); ++i) {
		if (!id)
			continue;
		const uint32_t flags = obs_get_source_output_flags(id);
		if (flags & (OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_DEPRECATED))
			continue;
		if (!(flags & (OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO)))
			continue;
		const char *dn = obs_source_get_display_name(id);
		out.push_back({id, QString::fromUtf8(dn && *dn ? dn : id)});
	}
	std::sort(out.begin(), out.end(),
		  [](const NewType &a, const NewType &b) { return a.label.compare(b.label, Qt::CaseInsensitive) < 0; });
	/* Two types may share a display name: keep them apart with their id. */
	std::vector<QString> base;
	for (const NewType &t : out)
		base.push_back(t.label);
	for (size_t i = 0; i < out.size(); ++i) {
		const auto same = std::count(base.begin(), base.end(), base[i]);
		if (same > 1)
			out[i].label += QStringLiteral(" (%1)").arg(QString::fromStdString(out[i].id));
	}
	return out;
}

layout::Size owned_canvas_size(OwnedCanvas &owned)
{
	layout::Size s{1080, 1920};
	obs_canvas_t *canvas = owned.get_ref();
	if (!canvas)
		return s;
	obs_video_info ovi = {};
	if (obs_canvas_has_video(canvas) && obs_canvas_get_video_info(canvas, &ovi) && ovi.base_width > 0 &&
	    ovi.base_height > 0)
		s = {ovi.base_width, ovi.base_height};
	obs_canvas_release(canvas); // UI thread
	return s;
}

} // namespace

VerticalItemList::VerticalItemList(OwnedCanvas &canvas, VerticalSelection &selection, QWidget *parent)
	: QWidget(parent),
	  m_canvas(canvas),
	  m_selection(selection)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);

	m_scene_label = new QLabel(this);
	layout->addWidget(m_scene_label);

	m_list = new QListWidget(this);
	m_list->setSelectionMode(QAbstractItemView::SingleSelection);
	layout->addWidget(m_list, 1);

	auto *buttons = new QHBoxLayout();
	buttons->setSpacing(4);
	m_add_btn = new QPushButton(tr("Add source"), this);
	auto *add_menu = new QMenu(m_add_btn);
	connect(add_menu->addAction(tr("Existing source...")), &QAction::triggered, this,
		&VerticalItemList::on_add_existing);
	connect(add_menu->addAction(tr("New source...")), &QAction::triggered, this, &VerticalItemList::on_add_new);
	m_add_btn->setMenu(add_menu);
	m_remove_btn = new QPushButton(tr("Remove"), this);
	m_up_btn = new QPushButton(tr("Up"), this);
	m_down_btn = new QPushButton(tr("Down"), this);
	for (QPushButton *b : {m_add_btn, m_remove_btn, m_up_btn, m_down_btn})
		buttons->addWidget(b);
	layout->addLayout(buttons);

	auto *actions = new QHBoxLayout();
	actions->setSpacing(4);
	m_props_btn = new QPushButton(tr("Properties..."), this);
	m_filters_btn = new QPushButton(tr("Filters..."), this);
	m_rename_btn = new QPushButton(tr("Rename..."), this);
	for (QPushButton *b : {m_props_btn, m_filters_btn, m_rename_btn})
		actions->addWidget(b);
	layout->addLayout(actions);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	layout->addWidget(m_status);

	m_editor = new TransformEditor(
		[this](const std::string &scene_uuid, int64_t id, const std::function<void(obs_sceneitem_t *)> &fn) {
			return with_item(scene_uuid, id, fn);
		},
		this);
	layout->addWidget(m_editor);

	connect(m_remove_btn, &QPushButton::clicked, this, &VerticalItemList::on_remove);
	connect(m_up_btn, &QPushButton::clicked, this, &VerticalItemList::on_up);
	connect(m_down_btn, &QPushButton::clicked, this, &VerticalItemList::on_down);
	connect(m_props_btn, &QPushButton::clicked, this, [this] { run_item_action(ItemAction::Properties, m_list_scene_uuid, selected_id()); });
	connect(m_filters_btn, &QPushButton::clicked, this, [this] { run_item_action(ItemAction::Filters, m_list_scene_uuid, selected_id()); });
	connect(m_rename_btn, &QPushButton::clicked, this, [this] { run_item_action(ItemAction::Rename, m_list_scene_uuid, selected_id()); });
	m_list->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(m_list, &QListWidget::customContextMenuRequested, this,
		[this](const QPoint &pos) { on_list_context_menu(pos); });
	connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *row) {
		if (row)
			run_item_action(ItemAction::Properties, m_list_scene_uuid,
					static_cast<int64_t>(row->data(kIdRole).toLongLong()));
	});
	connect(m_list, &QListWidget::currentRowChanged, this, [this] { on_selection_changed(); });
	connect(m_list, &QListWidget::itemChanged, this, &VerticalItemList::on_item_changed);
	connect(&m_selection, &VerticalSelection::changed, this, [this] { on_selection_model_changed(); });

	refresh();
}

VerticalItemList::~VerticalItemList()
{
	shutdown();
}

/* ---- libobs access: every reference lives inside one call ------------------ */

bool VerticalItemList::with_scene(const std::string &scene_uuid, const std::function<void(obs_scene_t *)> &fn)
{
	return with_scene_impl(&scene_uuid, [&fn](obs_scene_t *scene, const std::string &) { fn(scene); });
}

bool VerticalItemList::with_active_scene(const SceneFn &fn)
{
	return with_scene_impl(nullptr, fn);
}

/* required_uuid null: whatever scene is active. Otherwise only that scene (item ids are per scene). */
bool VerticalItemList::with_scene_impl(const std::string *required_uuid, const SceneFn &fn)
{
	if (m_shutdown.load())
		return false;
	obs_canvas_t *canvas = m_canvas.get_ref();
	if (!canvas)
		return false;

	bool called = false;
	obs_source_t *source = obs_canvas_get_channel(canvas, 0);
	obs_scene_t *scene = source ? obs_scene_from_source(source) : nullptr; // no ref; valid while source is held
	const char *active_uuid_c = source ? obs_source_get_uuid(source) : nullptr;
	const std::string active_uuid = active_uuid_c ? active_uuid_c : "";
	/* Item ids are per scene: refuse when the active scene is not the one the ids come from. */
	if (active_uuid.empty() || (required_uuid && *required_uuid != active_uuid))
		scene = nullptr;
	if (scene) {
		/* INV-6: write only to a scene that belongs to the owned canvas. */
		obs_canvas_t *scene_canvas = obs_source_get_canvas(source);
		const char *canvas_uuid = scene_canvas ? obs_canvas_get_uuid(scene_canvas) : nullptr;
		const bool owned = scene_canvas == canvas && canvas_uuid && m_canvas.uuid() == canvas_uuid;
		if (scene_canvas)
			obs_canvas_release(scene_canvas);
		if (owned) {
			fn(scene, active_uuid);
			called = true;
		}
	}
	if (source)
		obs_source_release(source);
	obs_canvas_release(canvas);
	return called;
}

bool VerticalItemList::with_item(const std::string &scene_uuid, int64_t item_id,
				 const std::function<void(obs_sceneitem_t *)> &fn)
{
	if (item_id == 0)
		return false;
	bool called = false;
	with_scene(scene_uuid, [&](obs_scene_t *scene) {
		FindItem f;
		f.id = item_id;
		obs_scene_enum_items(scene, find_item, &f);
		if (!f.found)
			return;
		fn(f.found);
		called = true;
		obs_sceneitem_release(f.found);
	});
	return called;
}

/* ---- signals ---------------------------------------------------------- */

void VerticalItemList::on_signal(void *param, calldata_t *)
{
	/* Any thread, possibly with libobs locks held: only queue a coalesced refresh, touch nothing else. */
	static_cast<VerticalItemList *>(param)->queue_refresh();
}

void VerticalItemList::on_transform_signal(void *param, calldata_t *)
{
	/* item_transform can fire on every source size change: never rebuild the list for it. */
	static_cast<VerticalItemList *>(param)->queue_editor_reload();
}

void VerticalItemList::queue_editor_reload()
{
	if (m_shutdown.load())
		return;
	if (m_reload_queued.exchange(true))
		return; // one reload pending; it reads the latest values when it runs
	QMetaObject::invokeMethod(this, [this] { editor_reload_throttled(); }, Qt::QueuedConnection);
}

/* UI thread. item_transform can fire every frame (and on every drag step): at most ~30 reloads/s. */
void VerticalItemList::editor_reload_throttled()
{
	if (m_shutdown.load()) {
		m_reload_queued = false;
		return;
	}
	const qint64 since = m_reload_clock.isValid() ? m_reload_clock.elapsed() : kReloadIntervalMs;
	if (since < kReloadIntervalMs) {
		QTimer::singleShot(static_cast<int>(kReloadIntervalMs - since), this, [this] { editor_reload_now(); });
		return;
	}
	editor_reload_now();
}

void VerticalItemList::editor_reload_now()
{
	m_reload_queued = false;
	if (m_shutdown.load())
		return;
	m_reload_clock.restart();
	m_editor->reload(false);
	notify_change();
}

void VerticalItemList::reload_editor(bool force)
{
	if (m_shutdown.load())
		return;
	m_editor->reload(force);
}

void VerticalItemList::notify_change()
{
	if (m_change_listener && !m_shutdown.load())
		m_change_listener();
}

void VerticalItemList::queue_refresh()
{
	if (m_shutdown.load())
		return;
	if (m_refresh_queued.exchange(true))
		return;
	QMetaObject::invokeMethod(
		this,
		[this] {
			m_refresh_queued = false;
			if (m_shutdown.load())
				return;
			refresh();
		},
		Qt::QueuedConnection);
}

void VerticalItemList::connect_canvas_signals(obs_canvas_t *canvas)
{
	if (m_shutdown.load())
		return;
	const char *uuid_c = obs_canvas_get_uuid(canvas);
	const std::string uuid = uuid_c ? uuid_c : "";
	{
		std::lock_guard<std::mutex> lock(m_conn_mutex);
		if (m_conn_canvas && m_conn_canvas_uuid == uuid)
			return;
	}
	detach_signals(); // a different canvas than before (also drops the scene link)

	signal_handler_t *handler = obs_canvas_get_signal_handler(canvas);
	if (!handler)
		return;
	std::lock_guard<std::mutex> lock(m_conn_mutex);
	if (m_shutdown.load())
		return;
	for (const char *sig : kCanvasSignals)
		signal_handler_connect(handler, sig, &VerticalItemList::on_signal, this);
	m_conn_canvas = obs_canvas_get_weak_canvas(canvas);
	m_conn_canvas_uuid = uuid;
}

void VerticalItemList::connect_scene_signals(obs_source_t *scene_source)
{
	if (m_shutdown.load())
		return;
	const char *uuid_c = obs_source_get_uuid(scene_source);
	const std::string uuid = uuid_c ? uuid_c : "";
	{
		std::lock_guard<std::mutex> lock(m_conn_mutex);
		if (m_conn_scene && m_conn_scene_uuid == uuid)
			return;
		detach_scene_signals_locked(); // another scene is active now
	}

	signal_handler_t *handler = obs_source_get_signal_handler(scene_source);
	if (!handler)
		return;
	std::lock_guard<std::mutex> lock(m_conn_mutex);
	if (m_shutdown.load() || m_conn_scene)
		return;
	for (const char *sig : kSceneSignals)
		signal_handler_connect(handler, sig, &VerticalItemList::on_signal, this);
	signal_handler_connect(handler, kTransformSignal, &VerticalItemList::on_transform_signal, this);
	m_conn_scene = obs_source_get_weak_source(scene_source);
	m_conn_scene_uuid = uuid;
}

void VerticalItemList::shutdown()
{
	m_shutdown = true;
	detach_signals();
	if (m_editor)
		m_editor->shutdown();
}

void VerticalItemList::detach_signals()
{
	std::lock_guard<std::mutex> lock(m_conn_mutex);
	detach_scene_signals_locked();
	detach_canvas_signals_locked();
}

/* Only touch a handler while its owner is provably alive. When the owner is already gone, its handler
 * (and our registrations with it) are gone too. Called from the canvas_remove signal the remover still
 * holds its own ref, so the release below is never the last one. */
void VerticalItemList::detach_scene_signals_locked()
{
	if (!m_conn_scene)
		return;
	obs_source_t *s = obs_weak_source_get_source(m_conn_scene);
	if (s) {
		signal_handler_t *handler = obs_source_get_signal_handler(s);
		if (handler) {
			for (const char *sig : kSceneSignals)
				signal_handler_disconnect(handler, sig, &VerticalItemList::on_signal, this);
			signal_handler_disconnect(handler, kTransformSignal, &VerticalItemList::on_transform_signal, this);
		}
		obs_source_release(s);
	}
	obs_weak_source_release(m_conn_scene);
	m_conn_scene = nullptr;
	m_conn_scene_uuid.clear();
}

void VerticalItemList::detach_canvas_signals_locked()
{
	if (!m_conn_canvas)
		return;
	obs_canvas_t *c = obs_weak_canvas_get_canvas(m_conn_canvas);
	if (c) {
		signal_handler_t *handler = obs_canvas_get_signal_handler(c);
		if (handler) {
			for (const char *sig : kCanvasSignals)
				signal_handler_disconnect(handler, sig, &VerticalItemList::on_signal, this);
		}
		obs_canvas_release(c);
	}
	obs_weak_canvas_release(m_conn_canvas);
	m_conn_canvas = nullptr;
	m_conn_canvas_uuid.clear();
}

/* ---- list ------------------------------------------------------------- */

void VerticalItemList::refresh()
{
	if (m_shutdown.load())
		return;

	const int64_t pending = m_pending_select;
	m_pending_select = 0;

	ItemRows items;
	std::string scene_name;
	std::string scene_uuid;
	bool have_canvas = false;
	bool have_scene = false;

	obs_canvas_t *canvas = m_canvas.get_ref();
	if (canvas) {
		have_canvas = true;
		connect_canvas_signals(canvas);
		obs_source_t *source = obs_canvas_get_channel(canvas, 0);
		obs_scene_t *scene = source ? obs_scene_from_source(source) : nullptr;
		if (scene) {
			have_scene = true;
			const char *n = obs_source_get_name(source);
			scene_name = n ? n : "";
			const char *u = obs_source_get_uuid(source);
			scene_uuid = u ? u : "";
			connect_scene_signals(source);
			obs_scene_enum_items(scene, collect_item, &items);
		}
		if (source)
			obs_source_release(source);
		obs_canvas_release(canvas);
	} else {
		detach_signals();
	}
	m_has_scene = have_scene;
	/* The selection model carries its scene uuid: a selection from another (old) scene yields 0 here, so a
	 * scene change drops it, because ids of the old scene mean other items in this one. */
	const int64_t previous = pending != 0 ? pending : m_selection.item_in(scene_uuid);
	m_list_scene_uuid = scene_uuid;
	if (!have_scene) {
		std::lock_guard<std::mutex> lock(m_conn_mutex);
		detach_scene_signals_locked();
	}

	m_scene_label->setText(have_scene ? tr("Active scene: %1").arg(QString::fromStdString(scene_name))
					  : tr("No active scene."));

	m_list->blockSignals(true);
	m_list->clear();
	int select_row = -1;
	/* libobs lists bottom to top; show the top item first, like the OBS source list. */
	for (auto it = items.rows.rbegin(); it != items.rows.rend(); ++it) {
		auto *row = new QListWidgetItem(QString::fromStdString(it->name), m_list);
		row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
		row->setCheckState(it->visible ? Qt::Checked : Qt::Unchecked);
		row->setData(kIdRole, QVariant::fromValue<qlonglong>(it->id));
		if (it->id == previous)
			select_row = m_list->count() - 1;
	}
	if (select_row >= 0)
		m_list->setCurrentRow(select_row);
	m_list->blockSignals(false);

	set_status(have_canvas ? m_pending_status : tr("The vertical canvas is not available yet."));
	m_pending_status.clear();
	update_buttons();

	const int64_t now = selected_id();
	if (now != previous || now == 0)
		m_editor->set_item(m_list_scene_uuid, now);
	else
		m_editor->reload(false);
	/* An item that is gone (or a scene change) clears the shared selection; the preview follows. */
	m_selection.set(m_list_scene_uuid, now);
	notify_change();
}

int64_t VerticalItemList::selected_id() const
{
	const QListWidgetItem *item = m_list ? m_list->currentItem() : nullptr;
	return item ? static_cast<int64_t>(item->data(kIdRole).toLongLong()) : 0;
}

void VerticalItemList::set_status(const QString &text)
{
	m_status->setText(text);
	m_status->setVisible(!text.isEmpty());
}

void VerticalItemList::update_buttons()
{
	const bool scene_ok = m_has_scene && !m_canvas.uuid().empty();
	const bool has_sel = selected_id() != 0;
	m_add_btn->setEnabled(scene_ok);
	m_remove_btn->setEnabled(scene_ok && has_sel);
	m_up_btn->setEnabled(scene_ok && has_sel);
	m_down_btn->setEnabled(scene_ok && has_sel);
	for (QPushButton *b : {m_props_btn, m_filters_btn, m_rename_btn})
		b->setEnabled(scene_ok && has_sel);
}

void VerticalItemList::on_selection_changed()
{
	if (m_shutdown.load())
		return;
	update_buttons();
	m_editor->set_item(m_list_scene_uuid, selected_id());
	m_selection.set(m_list_scene_uuid, selected_id());
}

/* The selection changed elsewhere (preview click): mirror it in the list and the transform panel. */
void VerticalItemList::on_selection_model_changed()
{
	if (m_shutdown.load())
		return;
	if (!m_selection.scene_uuid().empty() && m_selection.scene_uuid() != m_list_scene_uuid) {
		queue_refresh(); // the preview saw a newer active scene than the rows; refresh picks the id up
		return;
	}
	const int64_t id = m_selection.item_in(m_list_scene_uuid);
	if (id == selected_id())
		return;
	int row = -1;
	for (int i = 0; i < m_list->count(); ++i) {
		if (static_cast<int64_t>(m_list->item(i)->data(kIdRole).toLongLong()) == id) {
			row = i;
			break;
		}
	}
	m_list->blockSignals(true);
	if (row >= 0) {
		m_list->setCurrentRow(row);
		m_list->scrollToItem(m_list->item(row));
	} else {
		m_list->setCurrentRow(-1);
		m_list->clearSelection();
	}
	m_list->blockSignals(false);
	update_buttons();
	m_editor->set_item(m_list_scene_uuid, selected_id());
	if (row < 0 && id != 0) {
		m_pending_select = id; // an item the rows do not show yet (just added): rebuild and keep it
		queue_refresh();
	}
}

/* ---- actions ---------------------------------------------------------- */

void VerticalItemList::on_add_existing()
{
	if (m_shutdown.load())
		return;

	/* Collect names only (no source ref outlives obs_enum_sources), then ask with no libobs ref held. */
	InputNames inputs;
	obs_enum_sources(collect_input, &inputs);
	if (inputs.names.empty()) {
		set_status(tr("There are no existing sources to add. Create one in the main OBS window first."));
		return;
	}
	const std::string list_scene = m_list_scene_uuid; // the scene the user was looking at
	QStringList choices;
	for (const std::string &n : inputs.names)
		choices << QString::fromStdString(n);
	choices.sort(Qt::CaseInsensitive);

	bool ok = false;
	QPointer<VerticalItemList> self(this);
	const QString picked = QInputDialog::getItem(this, tr("Add source"), tr("Existing source:"), choices, 0, false, &ok);
	if (!self || m_shutdown.load() || !ok || picked.isEmpty())
		return;

	QString error;
	int64_t new_id = 0;
	const bool in_scene = with_scene(list_scene, [&](obs_scene_t *scene) {
		obs_source_t *source = obs_get_source_by_name(picked.toUtf8().constData());
		if (!source || obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT) {
			error = tr("The source no longer exists.");
		} else {
			/* Adds a reference to the existing input; the input itself is not modified. */
			obs_sceneitem_t *item = obs_scene_add(scene, source); // not ref'd for the caller
			if (item)
				new_id = obs_sceneitem_get_id(item);
			else
				error = tr("Could not add the source to the scene.");
		}
		if (source)
			obs_source_release(source);
	});
	if (!in_scene && error.isEmpty())
		error = tr("The active scene changed or is missing; nothing was added.");

	m_pending_select = new_id;
	refresh();
	set_status(error);
}

void VerticalItemList::on_remove()
{
	if (m_shutdown.load())
		return;
	const int64_t id = selected_id();
	if (id == 0)
		return;
	const std::string list_scene = m_list_scene_uuid; // the id belongs to this scene only
	const QListWidgetItem *cur = m_list->currentItem();
	const QString name = cur ? cur->text() : QString();

	const auto answer = QMessageBox::question(this, tr("Remove item"),
						  tr("Remove \"%1\" from this scene? The source itself is kept.").arg(name),
						  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (m_shutdown.load() || answer != QMessageBox::Yes)
		return;

	QString error;
	const bool ok = with_item(list_scene, id, [](obs_sceneitem_t *item) { obs_sceneitem_remove(item); });
	if (!ok)
		error = tr("The item no longer exists or the active scene changed; nothing was removed.");
	refresh();
	set_status(error);
}

void VerticalItemList::move_selected(bool up)
{
	if (m_shutdown.load())
		return;
	const int64_t id = selected_id();
	if (id == 0)
		return;
	QString error;
	const bool ok = with_item(m_list_scene_uuid, id, [up](obs_sceneitem_t *item) {
		obs_sceneitem_set_order(item, up ? OBS_ORDER_MOVE_UP : OBS_ORDER_MOVE_DOWN);
	});
	if (!ok)
		error = tr("The item no longer exists.");
	m_pending_select = id;
	refresh();
	set_status(error);
}

void VerticalItemList::on_up()
{
	move_selected(true);
}

void VerticalItemList::on_down()
{
	move_selected(false);
}

void VerticalItemList::on_item_changed(QListWidgetItem *row)
{
	if (m_shutdown.load() || !row)
		return;
	const int64_t id = static_cast<int64_t>(row->data(kIdRole).toLongLong());
	const bool visible = row->checkState() == Qt::Checked;
	const bool ok = with_item(m_list_scene_uuid, id,
				  [visible](obs_sceneitem_t *item) { obs_sceneitem_set_visible(item, visible); });
	if (!ok) {
		/* Never rebuild (clear) the list from inside its own itemChanged emission. */
		m_pending_status = tr("The item no longer exists or the active scene changed.");
		queue_refresh();
	}
}

/* ---- new source ---------------------------------------------------------- */

void VerticalItemList::on_add_new()
{
	if (m_shutdown.load())
		return;
	const std::string list_scene = m_list_scene_uuid; // the scene the user was looking at

	const std::vector<NewType> types = creatable_types();
	if (types.empty()) {
		set_status(tr("OBS reports no source types to create."));
		return;
	}
	QStringList labels;
	for (const NewType &t : types)
		labels << t.label;
	bool ok = false;
	QPointer<VerticalItemList> self(this);
	const QString picked = QInputDialog::getItem(this, tr("New source"), tr("Source type:"), labels, 0, false, &ok);
	if (!self || m_shutdown.load() || !ok || picked.isEmpty())
		return;
	const int type_index = labels.indexOf(picked);
	if (type_index < 0)
		return;
	const NewType type = types[static_cast<size_t>(type_index)];

	/* No libobs ref is held while the dialogs are open; a duplicate or empty name asks again. */
	QString display = type.label;
	const char *dn = obs_source_get_display_name(type.id.c_str());
	if (dn && *dn)
		display = QString::fromUtf8(dn);
	std::string name = item_actions::unique_name(display.toStdString(), all_source_names());
	QString message = tr("Name of the new source (it is created as a normal, global OBS source and added to "
			     "the vertical scene only):");
	for (;;) {
		const QString typed = QInputDialog::getText(this, tr("New source"), message, QLineEdit::Normal,
							    QString::fromStdString(name), &ok);
		if (!self || m_shutdown.load() || !ok)
			return;
		name = typed.trimmed().toStdString();
		if (name.empty()) {
			message = tr("The name must not be empty. Name of the new source:");
			name = item_actions::unique_name(display.toStdString(), all_source_names());
			continue;
		}
		if (name_taken(name)) {
			message = tr("A source named \"%1\" already exists. Choose another name:")
					  .arg(QString::fromStdString(name));
			name = item_actions::unique_name(name, all_source_names());
			continue;
		}
		break;
	}

	QString error;
	int64_t new_id = 0;
	const bool in_scene = with_scene(list_scene, [&](obs_scene_t *scene) {
		obs_source_t *source = obs_source_create(type.id.c_str(), name.c_str(), nullptr, nullptr);
		if (!source) {
			error = tr("OBS could not create a source of this type.");
			return;
		}
		/* Same default as OBS's own Add menu: sources that ask for it start monitored. */
		if (obs_source_get_output_flags(source) & OBS_SOURCE_MONITOR_BY_DEFAULT)
			obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_MONITOR_ONLY);
		obs_sceneitem_t *item = obs_scene_add(scene, source); // not ref'd for the caller
		if (item)
			new_id = obs_sceneitem_get_id(item);
		else
			error = tr("Could not add the new source to the scene.");
		obs_source_release(source); // the scene item keeps it alive (an unadded source is destroyed)
	});
	if (!in_scene && error.isEmpty())
		error = tr("The active scene changed or is missing; nothing was added.");

	m_pending_select = new_id;
	refresh();
	set_status(error);
	if (new_id != 0)
		run_item_action(ItemAction::Properties, list_scene, new_id);
}

/* ---- item actions ---------------------------------------------------------- */

void VerticalItemList::on_list_context_menu(const QPoint &list_pos)
{
	QListWidgetItem *row = m_list->itemAt(list_pos);
	if (!row)
		return;
	m_list->setCurrentItem(row); // writes the shared selection
	show_item_menu(m_list->viewport()->mapToGlobal(list_pos), static_cast<int64_t>(row->data(kIdRole).toLongLong()),
		       m_list_scene_uuid);
}

void VerticalItemList::show_item_menu(const QPoint &global_pos, int64_t item_id, const std::string &scene_uuid)
{
	if (m_shutdown.load() || item_id == 0 || scene_uuid.empty())
		return;

	bool locked = false;
	bool interactive = false;
	const bool exists = with_item(scene_uuid, item_id, [&](obs_sceneitem_t *item) {
		locked = obs_sceneitem_locked(item);
		obs_source_t *src = obs_sceneitem_get_source(item);
		interactive = src && (obs_source_get_output_flags(src) & OBS_SOURCE_INTERACTION) != 0;
	});
	if (!exists) {
		set_status(tr("The item no longer exists or the active scene changed."));
		return;
	}

	QMenu menu(this);
	std::vector<std::pair<QAction *, ItemAction>> entries;
	auto add = [&](QMenu &m, const QString &text, ItemAction a) { entries.emplace_back(m.addAction(text), a); };
	add(menu, tr("Properties..."), ItemAction::Properties);
	add(menu, tr("Filters..."), ItemAction::Filters);
	if (interactive)
		add(menu, tr("Interact..."), ItemAction::Interact);
	add(menu, tr("Rename..."), ItemAction::Rename);
	menu.addSeparator();
	add(menu, locked ? tr("Unlock") : tr("Lock"), ItemAction::ToggleLock);
	QMenu *transform = menu.addMenu(tr("Transform"));
	add(*transform, tr("Fit to canvas"), ItemAction::Fit);
	add(*transform, tr("Fill canvas"), ItemAction::Fill);
	add(*transform, tr("Reset transform"), ItemAction::Reset);
	transform->addSeparator();
	add(*transform, tr("Center horizontally"), ItemAction::CenterH);
	add(*transform, tr("Center vertically"), ItemAction::CenterV);
	transform->setEnabled(!locked);

	/* The menu is closed before anything runs: the action opens dialogs and resolves the item afresh. */
	QPointer<VerticalItemList> self(this);
	QAction *chosen = menu.exec(global_pos);
	if (!self || m_shutdown.load() || !chosen)
		return;
	/* The scene uuid was captured before the menu's event loop (a refresh may have switched scenes since):
	 * run_item_action refuses the access when the active scene is no longer that one. */
	for (const auto &e : entries) {
		if (e.first == chosen) {
			run_item_action(e.second, scene_uuid, item_id);
			return;
		}
	}
}

void VerticalItemList::run_item_action(ItemAction action, const std::string &scene_uuid, int64_t id)
{
	if (m_shutdown.load() || id == 0 || scene_uuid.empty())
		return;
	/* ids belong to scene_uuid only: with_item / apply_transform / rename_item refuse (and say so in the
	 * status line) when the active scene is another one, so nothing is done to a same-id item elsewhere. */
	QString error;

	switch (action) {
	case ItemAction::Properties:
	case ItemAction::Filters:
	case ItemAction::Interact: {
		bool refused = false;
		const bool ok = with_item(scene_uuid, id, [&](obs_sceneitem_t *item) {
			obs_source_t *src = obs_sceneitem_get_source(item); // owned by the item
			if (!src)
				return;
			if (action == ItemAction::Interact && !(obs_source_get_output_flags(src) & OBS_SOURCE_INTERACTION)) {
				refused = true;
				return;
			}
			/* The frontend opens OBS's own non-modal dialog and holds its own source reference. */
			if (action == ItemAction::Properties)
				obs_frontend_open_source_properties(src);
			else if (action == ItemAction::Filters)
				obs_frontend_open_source_filters(src);
			else
				obs_frontend_open_source_interaction(src);
		});
		if (!ok)
			error = tr("The item no longer exists or the active scene changed.");
		else if (refused)
			error = tr("This source cannot be interacted with.");
		break;
	}
	case ItemAction::Rename:
		rename_item(scene_uuid, id);
		return;
	case ItemAction::ToggleLock: {
		const bool ok = with_item(scene_uuid, id, [](obs_sceneitem_t *item) {
			obs_sceneitem_set_locked(item, !obs_sceneitem_locked(item));
		});
		if (!ok)
			error = tr("The item no longer exists or the active scene changed.");
		else
			notify_change(); // item_locked is not a connected signal: redraw the preview handles now
		break;
	}
	case ItemAction::Fit:
	case ItemAction::Fill:
	case ItemAction::Reset:
	case ItemAction::CenterH:
	case ItemAction::CenterV:
		apply_transform(action, scene_uuid, id);
		return;
	}
	if (!error.isEmpty())
		set_status(error);
}

void VerticalItemList::apply_transform(ItemAction action, const std::string &scene_uuid, int64_t id)
{
	const layout::Size canvas = owned_canvas_size(m_canvas);
	bool locked = false;
	const bool ok = with_item(scene_uuid, id, [&](obs_sceneitem_t *item) {
		if (obs_sceneitem_locked(item)) {
			locked = true;
			return;
		}
		const preview_edit::Item model = read_preview_item(item);
		obs_transform_info info = {};
		obs_sceneitem_get_info2(item, &info);
		obs_sceneitem_crop crop = {};
		obs_sceneitem_get_crop(item, &crop);

		if (action == ItemAction::CenterH || action == ItemAction::CenterV) {
			const preview_edit::Vec2 p = action == ItemAction::CenterH
							     ? item_actions::center_horizontally(model, canvas)
							     : item_actions::center_vertically(model, canvas);
			vec2_set(&info.pos, p.x, p.y);
		} else {
			const item_actions::Transform t = action == ItemAction::Fit    ? item_actions::fit_canvas(model, canvas)
							   : action == ItemAction::Fill ? item_actions::fill_canvas(model, canvas)
											: item_actions::reset_transform();
			vec2_set(&info.pos, t.pos.x, t.pos.y);
			info.rot = t.rot;
			vec2_set(&info.scale, t.scale.x, t.scale.y);
			info.alignment = t.alignment;
			info.bounds_type = t.bounds_type == item_actions::BoundsType::ScaleInner   ? OBS_BOUNDS_SCALE_INNER
					   : t.bounds_type == item_actions::BoundsType::ScaleOuter ? OBS_BOUNDS_SCALE_OUTER
												   : OBS_BOUNDS_NONE;
			vec2_set(&info.bounds, t.bounds.x, t.bounds.y);
			info.bounds_alignment = t.bounds_alignment;
			crop.left = t.crop.left;
			crop.top = t.crop.top;
			crop.right = t.crop.right;
			crop.bottom = t.crop.bottom;
		}
		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_info2(item, &info);
		obs_sceneitem_set_crop(item, &crop);
		obs_sceneitem_defer_update_end(item);
	});
	if (!ok)
		set_status(tr("The item no longer exists or the active scene changed."));
	else if (locked)
		set_status(tr("The item is locked. Unlock it first."));
	else
		set_status(QString());
	reload_editor(true);
	notify_change();
}

void VerticalItemList::rename_item(const std::string &scene_uuid, int64_t id)
{
	std::string old_name;
	bool shared = false;
	const bool exists = with_item(scene_uuid, id, [&](obs_sceneitem_t *item) {
		obs_source_t *src = obs_sceneitem_get_source(item);
		const char *n = src ? obs_source_get_name(src) : nullptr;
		old_name = n ? n : "";
		UsedOutside u;
		u.source_name = old_name;
		obs_enum_scenes(scene_uses_source, &u);
		shared = u.found;
	});
	if (!exists || old_name.empty()) {
		set_status(tr("The item no longer exists or the active scene changed."));
		return;
	}

	/* No libobs ref is held while the dialog is open. */
	QString text = tr("New name for the source:");
	if (shared)
		text += tr("\n\nThis source is shared with the main canvas: renaming it renames it there too.");
	else
		text += tr("\n\nSources are global in OBS: the new name shows everywhere the source is used.");
	bool ok = false;
	QPointer<VerticalItemList> self(this);
	const QString typed = QInputDialog::getText(this, tr("Rename source"), text, QLineEdit::Normal,
						    QString::fromStdString(old_name), &ok);
	if (!self || m_shutdown.load() || !ok)
		return;
	const std::string name = typed.trimmed().toStdString();
	if (name.empty()) {
		set_status(tr("The name must not be empty; nothing was renamed."));
		return;
	}
	if (name == old_name)
		return;
	if (name_taken(name)) {
		set_status(tr("A source named \"%1\" already exists; nothing was renamed.").arg(QString::fromStdString(name)));
		return;
	}
	const bool renamed = with_item(scene_uuid, id, [&](obs_sceneitem_t *item) {
		obs_source_t *src = obs_sceneitem_get_source(item);
		if (src)
			obs_source_set_name(src, name.c_str());
	});
	if (!renamed)
		set_status(tr("The item no longer exists or the active scene changed; nothing was renamed."));
	else
		m_pending_select = id;
	refresh();
}

} // namespace smulti
