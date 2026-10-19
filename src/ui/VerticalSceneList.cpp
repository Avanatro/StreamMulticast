/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalSceneList.hpp"

#include "../pipeline/CanvasSceneNames.hpp"
#include "../pipeline/OwnedCanvas.hpp"
#include "ImportScenesDialog.hpp"

#include <obs.h>

#include <QtCore/QMetaObject>
#include <QtCore/QStringList>
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

	m_import_btn = new QPushButton(tr("Import scenes from canvas..."), this);
	layout->addWidget(m_import_btn);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	layout->addWidget(m_status);

	connect(m_add_btn, &QPushButton::clicked, this, &VerticalSceneList::on_add);
	connect(m_rename_btn, &QPushButton::clicked, this, &VerticalSceneList::on_rename);
	connect(m_remove_btn, &QPushButton::clicked, this, &VerticalSceneList::on_remove);
	connect(m_active_btn, &QPushButton::clicked, this, &VerticalSceneList::on_set_active);
	connect(m_import_btn, &QPushButton::clicked, this, &VerticalSceneList::on_import);
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
			if (m_shutdown.load())
				return;
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
	if (m_shutdown.load())
		return; // unload may destroy the owned canvas before the dock is removed
	const QString previous = selected_scene();

	std::vector<std::string> scenes;
	std::string active;
	bool have_canvas = false;

	obs_canvas_t *canvas = m_canvas.get_ref();
	if (canvas) {
		have_canvas = true;
		connect_signals(canvas);
		scenes = canvas_scene_names(canvas);
		active = OwnedCanvas::active_scene_name(canvas); // destination while a transition runs
		obs_canvas_release(canvas);
	} else {
		detach_signals();
	}

	m_list->blockSignals(true);
	m_list->clear();
	int select_row = -1;
	for (const std::string &n : scenes) {
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
	m_import_btn->setEnabled(canvas_ok);
	m_rename_btn->setEnabled(canvas_ok && has_sel);
	m_remove_btn->setEnabled(canvas_ok && has_sel);
	m_active_btn->setEnabled(canvas_ok && has_sel);
}

/* ---- actions ---------------------------------------------------------- */

void VerticalSceneList::on_add()
{
	if (m_shutdown.load())
		return;
	obs_canvas_t *canvas = m_canvas.get_ref();
	if (!canvas) {
		refresh();
		return;
	}

	const std::string name = unique_name_in(canvas, kNewSceneBase);
	obs_scene_t *scene = obs_canvas_scene_create(canvas, name.c_str());
	const bool created = scene != nullptr;
	if (scene) {
		/* The canvas keeps its own refs (SCENE_REF); a first scene becomes the active one. */
		obs_source_t *current = OwnedCanvas::active_scene_ref(canvas);
		if (current)
			obs_source_release(current);
		else
			m_canvas.switch_scene(canvas, obs_scene_get_source(scene), SceneSwitch::Instant);
		obs_scene_release(scene);
	}
	obs_canvas_release(canvas);

	refresh();
	if (!created)
		set_status(tr("Could not create a scene."));
}

void VerticalSceneList::on_rename()
{
	if (m_shutdown.load())
		return;
	const QString old_name = selected_scene();
	if (old_name.isEmpty())
		return;

	/* The dialog runs an event loop: no libobs reference may be held across it. */
	bool ok = false;
	const QString entered =
		QInputDialog::getText(this, tr("Rename scene"), tr("Scene name:"), QLineEdit::Normal, old_name, &ok);
	if (m_shutdown.load() || !ok)
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
	if (m_shutdown.load())
		return;
	const QString name = selected_scene();
	if (name.isEmpty())
		return;

	/* Confirmation first (event loop): no libobs reference is held while it is open. */
	const auto answer = QMessageBox::question(this, tr("Remove scene"),
						  tr("Remove the scene \"%1\" from the vertical canvas?").arg(name),
						  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (m_shutdown.load() || answer != QMessageBox::Yes)
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
		const std::vector<std::string> scenes = canvas_scene_names(canvas);
		if (scenes.size() <= 1) {
			error = tr("The last scene cannot be removed; add another scene first.");
		} else {
			const std::string removing = name.toStdString();
			if (OwnedCanvas::active_scene_name(canvas) == removing) {
				/* Hand over to another scene first (instantly) so the preview never loses its scene. */
				for (const std::string &other : scenes) {
					if (other == removing)
						continue;
					obs_scene_t *next = obs_canvas_get_scene_by_name(canvas, other.c_str());
					if (next) {
						m_canvas.switch_scene(canvas, obs_scene_get_source(next),
								      SceneSwitch::Instant);
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
	if (m_shutdown.load())
		return;
	const QString name = selected_scene();
	if (name.isEmpty())
		return;

	obs_canvas_t *canvas = m_canvas.get_ref();
	if (!canvas) {
		refresh();
		return;
	}

	/* Same mechanism OwnedCanvas uses: the active scene is what OwnedCanvas::on_save_load stores in the
	 * collection and ensure() restores. Animated with the vertical transition setting (follow main: the
	 * current main transition and duration; cut: instant; custom: that transition). */
	QString error;
	obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas, name.toUtf8().constData());
	if (scene) {
		m_canvas.switch_scene(canvas, obs_scene_get_source(scene), SceneSwitch::Manual);
		obs_scene_release(scene);
	} else {
		error = tr("The scene no longer exists.");
	}
	obs_canvas_release(canvas);

	refresh();
	set_status(error);
}

void VerticalSceneList::on_import()
{
	if (m_shutdown.load())
		return;

	/* The dialog runs an event loop and keeps no libobs reference; import_scene() takes and releases
	 * its own references per scene. Only plain uuids and names cross the dialog. */
	ImportScenesDialog dialog(m_canvas, this);
	if (dialog.exec() != QDialog::Accepted || m_shutdown.load())
		return;
	const std::vector<ImportSelection> picked = dialog.selection();
	if (picked.empty())
		return;

	int imported = 0;
	QStringList problems;
	for (const ImportSelection &sel : picked) {
		if (m_shutdown.load())
			return;
		const ImportOutcome r = import_scene(m_canvas, sel);
		const QString scene = QString::fromStdString(sel.scene_name);
		if (r.ok)
			++imported;
		else
			problems << tr("\"%1\": %2").arg(scene, QString::fromStdString(r.message));
		if (!r.stranded.empty())
			problems << tr("A temporary copy \"%1\" stayed in the source canvas; remove it there manually.")
					    .arg(QString::fromStdString(r.stranded));
		if (r.source_changed)
			problems << tr("The scene list of the source canvas changed during the import of \"%1\".")
					    .arg(scene);
	}

	refresh();
	QString status = tr("Imported %n scene(s).", nullptr, imported);
	if (!problems.isEmpty())
		status += QStringLiteral(" ") + problems.join(QStringLiteral(" "));
	set_status(status);
}

} // namespace smulti
