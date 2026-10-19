/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalItemList.hpp"

#include "VerticalCanvasDock.hpp" // VerticalSelection
#include "../pipeline/OwnedCanvas.hpp"

#include <obs.h>

#include <QtCore/QMetaObject>
#include <QtCore/QTimer>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

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
	m_remove_btn = new QPushButton(tr("Remove"), this);
	m_up_btn = new QPushButton(tr("Up"), this);
	m_down_btn = new QPushButton(tr("Down"), this);
	for (QPushButton *b : {m_add_btn, m_remove_btn, m_up_btn, m_down_btn})
		buttons->addWidget(b);
	layout->addLayout(buttons);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	layout->addWidget(m_status);

	m_editor = new TransformEditor(
		[this](const std::string &scene_uuid, int64_t id, const std::function<void(obs_sceneitem_t *)> &fn) {
			return with_item(scene_uuid, id, fn);
		},
		this);
	layout->addWidget(m_editor);

	connect(m_add_btn, &QPushButton::clicked, this, &VerticalItemList::on_add);
	connect(m_remove_btn, &QPushButton::clicked, this, &VerticalItemList::on_remove);
	connect(m_up_btn, &QPushButton::clicked, this, &VerticalItemList::on_up);
	connect(m_down_btn, &QPushButton::clicked, this, &VerticalItemList::on_down);
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

void VerticalItemList::on_add()
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
	const QString picked = QInputDialog::getItem(this, tr("Add source"), tr("Existing source:"), choices, 0, false, &ok);
	if (m_shutdown.load() || !ok || picked.isEmpty())
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

} // namespace smulti
