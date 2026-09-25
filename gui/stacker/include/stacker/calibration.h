#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace stacker
{

/**
 * One master calibration frame found in the calibration directory - only
 * its header is read at scan time, the pixels are loaded on first use
 * (CalibrationLibrary::pixels()).
 */
struct CalibFrame
{
	std::string path;
	std::string name;     // basename, for logs and the stack's FITS header
	double exptime = 0;   // darks only
	std::string filter;   // flats only
	long width = 0;
	long height = 0;
};

/**
 * Master darks and flats for one camera, read from a single directory in
 * the layout the calibration pipeline already produces:
 *
 *   dark-050-001.0-1x1-1024x1024.fits   (master dark, one per exposure time)
 *   flat-R-1x1.fits                     (master flat, one per filter)
 *
 * A file's kind comes from its IMAGETYP header (anything containing
 * "dark"/"flat", e.g. "mdark"/"mflat"), else from the "dark"/"flat"
 * filename prefix. Exposure time comes from EXPTIME, filter from FILTER -
 * both falling back to the matching filename field when the header lacks
 * it. Darks are used as-is (they include the bias), so a frame is only
 * ever calibrated with a dark of the very same exposure time - there is
 * no dark scaling, which is why the exposure-time choice is limited to
 * what exists here.
 *
 * scan() builds the index once, before any camera exists; after that the
 * index is read-only and pixels() is the only mutating call (guarded by
 * its own mutex).
 */
class CalibrationLibrary
{
	public:
		/**
		 * Index every dark/flat in dir. Returns false (with err set) if the
		 * directory can't be read or holds no usable dark at all.
		 */
		bool scan (const std::string &dir, std::string &err);

		const std::string &directory () const { return dir; }
		const std::vector<CalibFrame> &darks () const { return darkFrames; }
		const std::vector<CalibFrame> &flats () const { return flatFrames; }

		const CalibFrame *findDark (double exptime, long width, long height) const;

		/**
		 * Exact filter-name match first, then case-insensitive - the
		 * pipeline keeps "R" and "r" as different flats, so the exact one
		 * must win whenever it exists.
		 */
		const CalibFrame *findFlat (const std::string &filter, long width, long height) const;

		/**
		 * Pixels of frame in FITS native (bottom-up) row order, width*height
		 * floats, or nullptr if the file can't be read. Flats are
		 * normalized to a median of 1 on load, so a calibrated frame stays
		 * in ADU whatever the master flat was normalized to. Cached; the
		 * returned pointer stays valid for the library's lifetime.
		 */
		const std::vector<float> *pixels (const CalibFrame *frame, std::string &err);

		/** Same tolerance findDark() uses - exposure times are written with 0.1 ms resolution at best. */
		static bool sameExptime (double a, double b) { return a > b - 5e-4 && a < b + 5e-4; }

	private:
		std::string dir;
		std::vector<CalibFrame> darkFrames;
		std::vector<CalibFrame> flatFrames;

		std::mutex cacheMutex;
		std::map<std::string, std::vector<float>> cache;
};

}
