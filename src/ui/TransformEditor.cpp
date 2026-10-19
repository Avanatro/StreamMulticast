/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "TransformEditor.hpp"

#include "../core/LayoutMath.hpp"
#include "../core/OwnedCanvasPolicy.hpp"

#include <obs.h>

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <initializer_list>
#include <utility>

namespace smulti {

namespace {

constexpr double kPosLimit = 100000.0;
constexpr double kScaleLimit = 1000.0;
constexpr int kCropLimit = 100000;

layout::Size canvas_size()
{
	const VideoGeometry g = derive_vertical_geometry(VideoGeometry{});
	return {g.base_width, g.base_height};
}

layout::Size source_size(obs_sceneitem_t *item)
{
	obs_source_t *src = obs_sceneitem_get_source(item); // owned by the item, no ref taken
	if (!src)
		return {};
	return {obs_source_get_width(src), obs_source_get_height(src)};
}

/** One atomic transform write. Bounds NONE and top-left alignment keep pos/scale/crop literal. */
void write_transform(obs_sceneitem_t *item, const layout::ItemTransform &t)
{
	vec2 pos;
	vec2_set(&pos, t.pos_x, t.pos_y);
	vec2 scale;
	vec2_set(&scale, t.scale_x, t.scale_y);
	obs_sceneitem_crop crop;
	crop.left = t.crop.left;
	crop.top = t.crop.top;
	crop.right = t.crop.right;
	crop.bottom = t.crop.bottom;

	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_NONE);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	obs_sceneitem_set_rot(item, 0.0f);
	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_set_scale(item, &scale);
	obs_sceneitem_set_crop(item, &crop);
	obs_sceneitem_defer_update_end(item);
}

} // namespace

TransformEditor::TransformEditor(ItemAccessor access, QWidget *parent) : QWidget(parent), m_access(std::move(access))
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(4);

	m_info = new QLabel(this);
	root->addWidget(m_info);

	auto *presets = new QHBoxLayout();
	m_fit_btn = new QPushButton(tr("Fit"), this);
	m_fit_btn->setToolTip(tr("Scale the whole source into the canvas, centred, no crop"));
	m_fill_btn = new QPushButton(tr("Fill"), this);
	m_fill_btn->setToolTip(tr("Cover the whole canvas, cropping the overflow evenly"));
	m_apply_btn = new QPushButton(tr("Apply"), this);
	m_apply_btn->setToolTip(tr("Write the numeric values below to the item"));
	presets->addWidget(m_fit_btn);
	presets->addWidget(m_fill_btn);
	presets->addStretch(1);
	presets->addWidget(m_apply_btn);
	root->addLayout(presets);

	auto make_dbl = [this](double lo, double hi, int decimals) {
		auto *s = new QDoubleSpinBox(this);
		s->setRange(lo, hi);
		s->setDecimals(decimals);
		s->setKeyboardTracking(false);
		return s;
	};
	auto make_int = [this]() {
		auto *s = new QSpinBox(this);
		s->setRange(0, kCropLimit);
		s->setKeyboardTracking(false);
		return s;
	};
	m_pos_x = make_dbl(-kPosLimit, kPosLimit, 2);
	m_pos_y = make_dbl(-kPosLimit, kPosLimit, 2);
	m_scale_x = make_dbl(-kScaleLimit, kScaleLimit, 4);
	m_scale_y = make_dbl(-kScaleLimit, kScaleLimit, 4);
	m_crop_l = make_int();
	m_crop_t = make_int();
	m_crop_r = make_int();
	m_crop_b = make_int();

	auto *grid = new QGridLayout();
	grid->setSpacing(4);
	grid->addWidget(new QLabel(tr("Position X / Y"), this), 0, 0);
	grid->addWidget(m_pos_x, 0, 1);
	grid->addWidget(m_pos_y, 0, 2);
	grid->addWidget(new QLabel(tr("Scale X / Y"), this), 1, 0);
	grid->addWidget(m_scale_x, 1, 1);
	grid->addWidget(m_scale_y, 1, 2);
	grid->addWidget(new QLabel(tr("Crop left / top"), this), 2, 0);
	grid->addWidget(m_crop_l, 2, 1);
	grid->addWidget(m_crop_t, 2, 2);
	grid->addWidget(new QLabel(tr("Crop right / bottom"), this), 3, 0);
	grid->addWidget(m_crop_r, 3, 1);
	grid->addWidget(m_crop_b, 3, 2);
	root->addLayout(grid);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	m_status->setVisible(false);
	root->addWidget(m_status);

	connect(m_fit_btn, &QPushButton::clicked, this, &TransformEditor::on_fit);
	connect(m_fill_btn, &QPushButton::clicked, this, &TransformEditor::on_fill);
	connect(m_apply_btn, &QPushButton::clicked, this, &TransformEditor::on_apply);

	m_info->setText(tr("Select an item to edit its transform."));
	set_fields_enabled(false);
}

void TransformEditor::shutdown()
{
	m_shutdown = true;
}

void TransformEditor::set_status(const QString &text)
{
	m_status->setText(text);
	m_status->setVisible(!text.isEmpty());
}

void TransformEditor::set_fields_enabled(bool enabled)
{
	for (QWidget *w : std::initializer_list<QWidget *>{m_fit_btn, m_fill_btn, m_apply_btn, m_pos_x, m_pos_y,
							    m_scale_x, m_scale_y, m_crop_l, m_crop_t, m_crop_r,
							    m_crop_b})
		w->setEnabled(enabled);
}

bool TransformEditor::any_field_focused() const
{
	for (const QWidget *w : std::initializer_list<const QWidget *>{m_pos_x, m_pos_y, m_scale_x, m_scale_y, m_crop_l,
								       m_crop_t, m_crop_r, m_crop_b})
		if (w->hasFocus() || (w->focusWidget() && w->focusWidget()->hasFocus()))
			return true;
	return false;
}

void TransformEditor::set_item(int64_t item_id)
{
	if (m_shutdown.load())
		return;
	if (item_id != m_item_id)
		set_status(QString());
	m_item_id = item_id;
	reload(true);
}

void TransformEditor::reload(bool force)
{
	if (m_shutdown.load())
		return;
	if (!force && any_field_focused())
		return; // never overwrite what the user is typing

	bool found = false;
	if (m_item_id != 0 && m_access) {
		found = m_access(m_item_id, [this](obs_sceneitem_t *item) {
			vec2 pos;
			vec2 scale;
			obs_sceneitem_crop crop;
			obs_sceneitem_get_pos(item, &pos);
			obs_sceneitem_get_scale(item, &scale);
			obs_sceneitem_get_crop(item, &crop);
			const layout::Size sz = source_size(item);

			const QSignalBlocker b1(m_pos_x), b2(m_pos_y), b3(m_scale_x), b4(m_scale_y), b5(m_crop_l),
				b6(m_crop_t), b7(m_crop_r), b8(m_crop_b);
			m_pos_x->setValue(pos.x);
			m_pos_y->setValue(pos.y);
			m_scale_x->setValue(scale.x);
			m_scale_y->setValue(scale.y);
			m_crop_l->setValue(crop.left);
			m_crop_t->setValue(crop.top);
			m_crop_r->setValue(crop.right);
			m_crop_b->setValue(crop.bottom);
			m_info->setText(tr("Source size: %1 x %2").arg(sz.w).arg(sz.h));
		});
	}
	if (!found) {
		m_item_id = 0;
		m_info->setText(tr("Select an item to edit its transform."));
	}
	set_fields_enabled(found);
}

void TransformEditor::apply_preset(bool fill)
{
	if (m_shutdown.load() || m_item_id == 0)
		return;
	QString error;
	const bool ok = m_access(m_item_id, [&](obs_sceneitem_t *item) {
		const layout::Size src = source_size(item);
		if (src.w == 0 || src.h == 0) {
			error = tr("The source has no size yet (not active or not rendered); nothing was changed.");
			return;
		}
		layout::ItemTransform t = fill ? layout::fill(src, canvas_size()) : layout::fit(src, canvas_size());
		t.crop = layout::clamp_crop(t.crop, src); // fill rounding can overshoot by < 1 px
		write_transform(item, t);
	});
	if (!ok)
		error = tr("The item no longer exists.");
	reload(true);
	set_status(error);
}

void TransformEditor::on_fit()
{
	apply_preset(false);
}

void TransformEditor::on_fill()
{
	apply_preset(true);
}

void TransformEditor::on_apply()
{
	if (m_shutdown.load() || m_item_id == 0)
		return;

	layout::ItemTransform t;
	t.pos_x = static_cast<float>(m_pos_x->value());
	t.pos_y = static_cast<float>(m_pos_y->value());
	t.scale_x = static_cast<float>(m_scale_x->value());
	t.scale_y = static_cast<float>(m_scale_y->value());
	t.crop = {m_crop_l->value(), m_crop_t->value(), m_crop_r->value(), m_crop_b->value()};

	QString error;
	const bool ok = m_access(m_item_id, [&](obs_sceneitem_t *item) {
		const layout::Size src = source_size(item);
		if (src.w != 0 && src.h != 0)
			t.crop = layout::clamp_crop(t.crop, src);
		else
			t.crop = {}; // unknown source size: do not crop blindly
		write_transform(item, t);
	});
	if (!ok)
		error = tr("The item no longer exists.");
	reload(true); // shows the clamped crop
	set_status(error);
}

} // namespace smulti
