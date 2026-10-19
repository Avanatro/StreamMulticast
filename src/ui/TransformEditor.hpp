/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <QtWidgets/QWidget>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// libobs forward declarations
struct obs_scene_item;
typedef struct obs_scene_item obs_sceneitem_t;

class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace smulti {

/**
 * Runs fn(item) with a scene item of the owned canvas' active scene, on the calling (UI) thread.
 * The implementation takes every canvas, scene and item reference itself and releases all of them
 * before it returns, so fn must not keep the item. Returns false (fn not called) when the canvas,
 * the scene or the item is gone, the active scene is not the one with scene_uuid (item ids are per
 * scene), or the scene does not belong to the owned canvas (INV-6).
 */
using ItemAccessor = std::function<bool(const std::string &scene_uuid, int64_t item_id,
					const std::function<void(obs_sceneitem_t *)> &fn)>;

/**
 * Transform panel for one scene item: Fit / Fill to the vertical canvas (smulti::layout) and
 * numeric position, scale and crop. Holds only the item's numeric id, never a libobs reference.
 * Multi-field writes are wrapped in obs_sceneitem_defer_update_begin/end; bounds are always
 * OBS_BOUNDS_NONE and the alignment top-left so that pos/scale/crop mean what the fields say.
 */
class TransformEditor : public QWidget {
	Q_OBJECT

public:
	explicit TransformEditor(ItemAccessor access, QWidget *parent = nullptr);

	/** Select the item to edit (0 = none) of the scene with scene_uuid and load its values. UI thread. */
	void set_item(const std::string &scene_uuid, int64_t item_id);

	/** Re-read the values from the item; skipped while a field has keyboard focus, unless force. */
	void reload(bool force = false);

	/** Never touch libobs again (OBS exit, module unload). Any thread, idempotent. */
	void shutdown();

private slots:
	void on_fit();
	void on_fill();
	void on_apply();

private:
	void apply_preset(bool fill);
	void set_status(const QString &text);
	void set_fields_enabled(bool enabled);
	bool any_field_focused() const;

	ItemAccessor m_access;
	int64_t m_item_id = 0;
	std::string m_scene_uuid; ///< scene the id belongs to (ids are only unique per scene)

	QLabel *m_info = nullptr;
	QLabel *m_status = nullptr;
	QPushButton *m_fit_btn = nullptr;
	QPushButton *m_fill_btn = nullptr;
	QPushButton *m_apply_btn = nullptr;
	QDoubleSpinBox *m_pos_x = nullptr;
	QDoubleSpinBox *m_pos_y = nullptr;
	QDoubleSpinBox *m_scale_x = nullptr;
	QDoubleSpinBox *m_scale_y = nullptr;
	QSpinBox *m_crop_l = nullptr;
	QSpinBox *m_crop_t = nullptr;
	QSpinBox *m_crop_r = nullptr;
	QSpinBox *m_crop_b = nullptr;

	std::atomic<bool> m_shutdown{false};
};

} // namespace smulti
