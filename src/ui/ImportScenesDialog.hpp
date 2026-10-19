/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <QtWidgets/QDialog>

#include <string>
#include <vector>

class QTreeWidget;
class QLabel;

namespace smulti {

class OwnedCanvas;

/** One scene picked in the dialog: the uuid of its canvas and its name. Plain strings, no libobs refs. */
struct ImportSelection {
	std::string canvas_uuid;
	std::string scene_name;
};

/** Result of importing one scene. */
struct ImportOutcome {
	std::string scene_name;      ///< name in the source canvas
	std::string final_name;      ///< name of the copy in the owned canvas (set when ok)
	bool ok = false;
	std::string message;         ///< human readable reason when not ok
	std::string stranded;        ///< kept for the caller; always empty since nothing is created in the source canvas
	bool source_changed = false; ///< the source canvas scene names differ from before (INV-6 alarm)
};

/**
 * Pick scenes of other canvases (not the owned one, not the main canvas) to copy into the owned canvas.
 *
 * The dialog is modal and keeps no libobs reference: it reads canvases and scenes while filling the
 * list and stores only uuids and names. Scenes that contain groups are shown disabled, because a
 * group source cannot be shared between two scenes: adding it to the copy would pull it out of the
 * original.
 */
class ImportScenesDialog : public QDialog {
	Q_OBJECT

public:
	explicit ImportScenesDialog(const OwnedCanvas &owned, QWidget *parent = nullptr);

	/** Checked scenes, in list order. */
	std::vector<ImportSelection> selection() const;

private:
	void populate(const std::string &owned_uuid);

	QTreeWidget *m_tree = nullptr;
	QLabel *m_hint = nullptr;
};

/**
 * Copy one scene of another canvas into the owned canvas. UI thread only.
 *
 * The original is never moved, renamed or removed (INV-6). A new scene with a free name ("X", "X (2)",
 * ...) is created directly in the owned canvas (obs_canvas_scene_create) and every item of the
 * original is added by reference (obs_scene_add, sources shared) with its item data copied the way
 * libobs duplicate_item_data does; the items get the owned canvas size as scale_ref. No scene is ever
 * created in, or moved out of, a foreign canvas. All references are taken and released inside this call.
 */
ImportOutcome import_scene(OwnedCanvas &owned, const ImportSelection &sel);

} // namespace smulti
