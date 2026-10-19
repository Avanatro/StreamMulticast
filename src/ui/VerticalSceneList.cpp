/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalSceneList.hpp"

#include "../pipeline/OwnedCanvas.hpp"

#include <obs.h>

#include <QtCore/QMetaObject>
#include <QtGui/QFont>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <string>
#include <vector>

namespace smulti {

namespace {

constexpr const char *kSignals[] = {"source_add", "source_remove", "source_rename", "channel_change"};
constexpr const char *kNewSceneBase = "V Scene";

struct SceneNames {
	std::vector<std::string> names;
};

bool collect_scene_name(void *param, obs_source_t *scene)
{
	const char *name = obs_source_get_name(scene);
	if (name)
		static_cast<SceneNames *>(param)->names.emplace_back(name);
	return true;
}

/** Name of the scene on channel 0 ("" when none). Releases the ref it takes. */
std::string active_scene_name(obs_canvas_t *canvas)
{
	std::string out;
	obs_source_t *src = obs_canvas_get_channel(canvas, 0);
	if (src) {
		const char *n = obs_source_get_name(src);
		out = n ? n : "";
		obs_source_release(src);
	}
	return out;
}

/** True when any source of the canvas (scene or not: they share one name table) uses the name. */
bool canvas_has_name(obs_canvas_t *canvas, const std::string &name)
{
	obs_source_t *src = obs_canvas_get_source_by_name(canvas, name.c_str());
	if (!src)
		return false;
	obs_source_release(src);
	return true;
}

std::string unique_scene_name(obs_canvas_t *canvas)
{
	if (!canvas_has_name(canvas, kNewSceneBase))
		return kNewSceneBase;
	for (int i = 2; i < 10000; ++i) {
		std::string candidate = std::string(kNewSceneBase) + " " + std::to_string(i);
		if (!canvas_has_name(canvas, candidate))
			return candidate;
	}
	return std::string(kNewSceneBase) + " new";
}

} // namespace

VerticalSceneList::VerticalSceneList(OwnedCanvas &canvas, QWidget *parent) : QWidget(parent), m_canvas(canvas)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);

	m_list = new QListWidget(this);
	m_list->setSelectionMode(QAbstractItemView::SingleSelection);
	layout->addWidget(m_list, 1);

	auto *buttons = new QHBoxLayout();
	buttons->setSpacing(4);
	m_add_btn = new QPushButton(tr("Add"), this);
	m_rename_btn = new QPushButton(tr("Rename"), this);
	m_remove_btn = new QPushButton(tr("Remove"), this);
	m_active_btn = new QPushButton(tr("Set active"), this);
	for (QPushButton *b : {m_add_btn, m_rename_btn, m_remove_btn, m_active_btn})
		buttons->addWidget(b);
	layout->addLayout(buttons);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	layout->addWidget(m_status);

	connect(m_add_btn, &QPushButton::clicked, this, &VerticalSceneList::on_add);
	connect(m_rename_btn, &QPushButton::clicked, this, &VerticalSceneList::on_rename);
	connect(m_remove_btn, &QPushButton::clicked, this, &VerticalSceneList::on_remove);
	connect(m_active_btn, &QPushButton::clicked, this, &VerticalSceneList::on_set_active);
	connect(m_list, &QListWidget::currentRowChanged, this, &VerticalSceneList::update_buttons);
	connect(m_list, &QListWidget::itemDoubleClicked, this, [this] { on_set_active(); });

	refresh();
}

VerticalSceneList::~VerticalSceneList()
{
	shutdown();
}

/* ---- signals ---------------------------------------------------------- */

void VerticalSceneList::on_canvas_signal(void *param, calldata_t *)
{
	/* Any thread, possibly with libobs locks held: only queue a coalesced refresh, touch nothing else. */
	static_cast<VerticalSceneList *>(param)->queue_refresh();
}

void VerticalSceneList::queue_refresh()
{
	if (m_refresh_queued.exchange(true))
		return;
	QMetaObject::invokeMethod(
		this,
		[this] {
			m_refresh_queued = false;
			refresh();
		},
		Qt::QueuedConnection);
}

void VerticalSceneList::connect_signals(obs_canvas_t *canvas)
{
	if (m_shutdown.load())
		return;
	const char *uuid_c = obs_canvas_get_uuid(canvas);
	const std::string uuid = uuid_c ? uuid_c : "";
	{
		std::lock_guard<std::mutex> lock(m_conn_mutex);
		if (m_conn_weak && m_conn_uuid == uuid)
			return; // already connected to this canvas
	}
	detach_signals(); // a different canvas than before

	signal_handler_t *handler = obs_canvas_get_signal_handler(canvas);
	if (!handler)
		return;
	std::lock_guard<std::mutex> lock(m_conn_mutex);
	if (m_shutdown.load())
		return;
	for (const char *sig : kSignals)
		signal_handler_connect(handler, sig, &VerticalSceneList::on_canvas_signal, this);
	m_conn_weak = obs_canvas_get_weak_canvas(canvas);
	m_conn_uuid = uuid;
}

void VerticalSceneList::shutdown()
{
	m_shutdown = true;
	detach_signals();
}

void VerticalSceneList::detach_signals()
{
	std::lock_guard<std::mutex> lock(m_conn_mutex);
	if (!m_conn_weak)
		return;
	/* Only touch the handler while the canvas is provably alive. When it is already gone, its handler
	 * (and our registrations with it) are gone too. When called from the canvas_remove signal the
	 * remover still holds its own ref, so the release below is never the last one. */
	obs_canvas_t *c = obs_weak_canvas_get_canvas(m_conn_weak);
	if (c) {
		signal_handler_t *handler = obs_canvas_get_signal_handler(c);
		if (handler) {
			for (const char *sig : kSignals)
				signal_handler_disconnect(handler, sig, &VerticalSceneList::on_canvas_signal, this);
		}
		obs_canvas_release(c);
	}
	obs_weak_canvas_release(m_conn_weak);
	m_conn_weak = nullptr;
	m_conn_uuid.clear();
}

/* ---- list ------------------------------------------------------------- */

void VerticalSceneList::refresh()
{
	const QString previous = selected_scene();

	SceneNames scenes;
	std::string active;
	bool have_canvas = false;

	obs_canvas_t *canvas = m_canvas.get_ref();
	if (canvas) {
		have_canvas = true;
		connect_signals(canvas);
		obs_canvas_enum_scenes(canvas, collect_scene_name, &scenes);
		active = active_scene_name(canvas);
		obs_canvas_release(canvas);
	} else {
		detach_signals();
	}

	m_list->blockSignals(true);
	m_list->clear();
	int select_row = -1;
	for (const std::string &n : scenes.names) {
		const QString name = QString::fromStdString(n);
		const bool is_active = (n == active);
		auto *item = new QListWidgetItem(is_active ? tr("%1  (active)").arg(name) : name, m_list);
		item->setData(Qt::UserRole, name);
		if (is_active) {
			QFont f = item->font();
			f.setBold(true);
			item->setFont(f);
		}
		if (name == previous)
			select_row = m_list->count() - 1;
	}
	if (select_row >= 0)
		m_list->setCurrentRow(select_row);
	m_list->blockSignals(false);

	set_status(have_canvas ? QString() : tr("The vertical canvas is not available yet."));
	update_buttons();
}

QString VerticalSceneList::selected_scene() const
{
	const QListWidgetItem *item = m_list ? m_list->currentItem() : nullptr;
	return item ? item->data(Qt::UserRole).toString() : QString();
}

void VerticalSceneList::set_status(const QString &text)
{
	m_status->setText(text);
	m_status->setVisible(!text.isEmpty());
}

void VerticalSceneList::update_buttons()
{
	const bool canvas_ok = !m_canvas.uuid().empty();
	const bool has_sel = !selected_scene().isEmpty();
	m_add_btn->setEnabled(canvas_ok);
	m_rename_btn->setEnabled(canvas_ok && has_sel);
	m_remove_btn->setEnabled(canvas_ok && has_sel);
	m_active_btn->setEnabled(canvas_ok && has_sel);
}

/* ---- actions ---------------------------------------------------------- */

void VerticalSceneList::on_add()
{
	obs_canvas_t *canvas = m_canvas.get_ref();
	if (!canvas) {
		refresh();
		return;
	}

	const std::string name = unique_scene_name(canvas);
	obs_scene_t *scene = obs_canvas_scene_create(canvas, name.c_str());
	const bool created = scene != nullptr;
	if (scene) {
		/* The canvas keeps its own refs (SCENE_REF); a first scene becomes the active one. */
		obs_source_t *current = obs_canvas_get_channel(canvas, 0);
		if (current)
			obs_source_release(current);
		else
			obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
		obs_scene_release(scene);
	}
	obs_canvas_release(canvas);

	refresh();
	if (!created)
		set_status(tr("Could not create a scene."));
}

void VerticalSceneList::on_rename()
{
	const QString old_name = selected_scene();
	if (old_name.isEmpty())
		return;

	/* The dialog runs an event loop: no libobs reference may be held across it. */
	bool ok = false;
	const QString entered =
		QInputDialog::getText(this, tr("Rename scene"), tr("Scene name:"), QLineEdit::Normal, old_name, &ok);
	if (!ok)
		return;
	const QString new_name = entered.trimmed();
	if (new_name.isEmpty()) {
		set_status(tr("The scene name must not be empty."));
		return;
	}
	if (new_name == old_name)
		return;

	obs_canvas_t *canvas = m_canvas.get_ref();
	if (!canvas) {
		refresh();
		return;
	}

	QString error;
	obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas, old_name.toUtf8().constData());
	if (!scene) {
		error = tr("The scene no longer exists.");
	} else if (canvas_has_name(canvas, new_name.toStdString())) {
		error = tr("A scene or source named \"%1\" already exists.").arg(new_name);
	} else {
		obs_source_set_name(obs_scene_get_source(scene), new_name.toUtf8().constData());
	}
	if (scene)
		obs_scene_release(scene);
	obs_canvas_release(canvas);

	refresh();
	set_status(error);
}

void VerticalSceneList::on_remove()
{
	const QString name = selected_scene();
	if (name.isEmpty())
		return;

	/* Confirmation first (event loop): no libobs reference is held while it is open. */
	const auto answer = QMessageBox::question(this, tr("Remove scene"),
						  tr("Remove the scene \"%1\" from the vertical canvas?").arg(name),
						  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (answer != QMessageBox::Yes)
		return;

	obs_canvas_t *canvas = m_canvas.get_ref();
	if (!canvas) {
		refresh();
		return;
	}

	QString error;
	obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas, name.toUtf8().constData());
	obs_source_t *source = scene ? obs_scene_get_source(scene) : nullptr;
	/* New strong ref to the canvas the scene belongs to (INV-6 guard). */
	obs_canvas_t *scene_canvas = source ? obs_source_get_canvas(source) : nullptr;
	const char *scene_canvas_uuid = scene_canvas ? obs_canvas_get_uuid(scene_canvas) : nullptr;

	if (!scene) {
		error = tr("The scene no longer exists.");
	} else if (!scene_canvas || scene_canvas != canvas || !scene_canvas_uuid ||
		   m_canvas.uuid() != scene_canvas_uuid) {
		/* Never remove a scene of another canvas (Aitum Vertical, main). */
		error = tr("The scene does not belong to the vertical canvas; nothing was removed.");
	} else {
		SceneNames scenes;
		obs_canvas_enum_scenes(canvas, collect_scene_name, &scenes);
		if (scenes.names.size() <= 1) {
			error = tr("The last scene cannot be removed; add another scene first.");
		} else {
			const std::string removing = name.toStdString();
			if (active_scene_name(canvas) == removing) {
				/* Hand channel 0 to another scene first so the preview never loses its scene. */
				for (const std::string &other : scenes.names) {
					if (other == removing)
						continue;
					obs_scene_t *next = obs_canvas_get_scene_by_name(canvas, other.c_str());
					if (next) {
						obs_canvas_set_channel(canvas, 0, obs_scene_get_source(next));
						obs_scene_release(next);
						break;
					}
				}
			}
			obs_canvas_scene_remove(scene);
			obs_source_remove(source); // marks it removed and signals "remove" to its listeners
		}
	}

	if (scene_canvas)
		obs_canvas_release(scene_canvas);
	if (scene)
		obs_scene_release(scene);
	obs_canvas_release(canvas);

	refresh();
	set_status(error);
}

void VerticalSceneList::on_set_active()
{
	const QString name = selected_scene();
	if (name.isEmpty())
		return;

	obs_canvas_t *canvas = m_canvas.get_ref();
	if (!canvas) {
		refresh();
		return;
	}

	/* Same mechanism OwnedCanvas uses: the scene on channel 0 is what OwnedCanvas::on_save_load
	 * stores in the collection and ensure() restores. */
	QString error;
	obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas, name.toUtf8().constData());
	if (scene) {
		obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
		obs_scene_release(scene);
	} else {
		error = tr("The scene no longer exists.");
	}
	obs_canvas_release(canvas);

	refresh();
	set_status(error);
}

} // namespace smulti
