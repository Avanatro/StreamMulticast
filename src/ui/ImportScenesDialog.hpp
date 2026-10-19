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
	std::string stranded;        ///< name of a copy that stayed in the source canvas (empty when none)
	bool source_changed = false; ///< the source canvas scene names differ from before (INV-6 alarm)
};

/**
 * Pick scenes of other canvases (not the owned one, not the main canvas) to copy into the owned canvas.
 *
 * The dialog is modal and keeps no libobs reference: it reads canvases and scenes while filling the
 * list and stores only uuids and names. Scenes that contain groups are shown disabled, because a
 * group is shared by the copy and obs_canvas_move_scene would pull it out of the original canvas.
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
 * The original is never moved, renamed or removed (INV-6): the scene is duplicated by reference
 * (OBS_SCENE_DUP_REFS, the copy lands in the SOURCE canvas) under a temporary unique name, only the
 * copy is moved to the owned canvas, renamed to a free name there, and then verified. All references
 * are taken and released inside this call.
 */
ImportOutcome import_scene(OwnedCanvas &owned, const ImportSelection &sel);

} // namespace smulti
