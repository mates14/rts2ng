#pragma once

#include <string>

namespace rts2web
{

/**
 * Where the dark/flat corrected previews find their masters: asarina's
 * (MJ's image pipeline, which corrects every new frame for the site
 * pages) master frames, chosen by asarina's own rules
 * (asarina/pipeline/image.py, ImageProcessor). [JS 2026-10-28]
 */
struct CalibConfig
{
	std::string root;				//!< --calib-root: <root>/<chip id>/<year>/
	std::string dirTemplate;		//!< --calib-dir-template: legacy, with {year}; + /<CCD_NAME>/ when the frame has one
	std::string chipIdCmd;			//!< --chip-id-cmd: pyrt-chip-id, the camera registry

	bool enabled () const { return !root.empty () || !dirTemplate.empty (); }
};

/**
 * The masters for one frame, and how to apply them.
 */
struct CalibFrames
{
	std::string dark;				//!< path, empty when none was found
	std::string flat;				//!< path, empty when none was found
	long x0 = 0, y0 = 0;			//!< the frame's window in the masters (0-based)
	long w = 0, h = 0;				//!< the frame's size
	// the active area kept of a full frame (asarina's CAMERA_CROPS);
	// the whole frame when the chip has no crop or the frame is a window
	long cropX0 = 0, cropY0 = 0, cropW = 0, cropH = 0;
	std::string text;				//!< what applies, for X-RTS2-Calibration
};

/**
 * Finds the masters for the frame at fullPath. Never fails: what is not
 * found is left out and named in out.text ("dark and flat corrected",
 * "dark only, no flat for halpha", "raw: no dark (chip mi2065, -20 C, bin
 * 1, 60 s), no flat for B"). Thread-safe: the master indexes and the chip
 * ids are cached behind a mutex.
 */
void findCalibration (const CalibConfig &config, const std::string &fullPath, CalibFrames &out);

}
