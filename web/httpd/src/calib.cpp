#include "calib.h"

#include <fitsio.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
#include <sys/stat.h>
#include <vector>

/*
 * A port of asarina's choice of master frames (MJ's image pipeline,
 * asarina/pipeline/image.py, ImageProcessor), so that the archive's
 * corrected previews use the masters the pipeline uses for the site
 * pages. Kept as asarina has it, step by step:
 *
 * - The chip id of the frame and of every master comes from the camera
 *   registry, pyrt's pyrt-chip-id (pyrt/utils/chip_id.py): not copied
 *   here, asked, and the answer kept per combination of the header fields
 *   the registry reads (REGISTRY_FIELDS).
 * - The masters of a year: <root>/<chip>/<year>/, then the legacy
 *   <template with year>/<CCD_NAME>/, or <template with year>/ for a frame
 *   without CCD_NAME (_load_calibration_year). The first loaded master of
 *   a key wins (_load_calibration_files).
 * - A dark: IMAGETYP mdark, same chip, CCD_TEMP rounded to 5 C groups
 *   (_round_temperature), same BINX, the closest EXPTIME within 3 s
 *   (find_best_dark). A flat: mflat, same chip, FILTER and BINX
 *   (find_flat). Not found: the other years, closest first, +-1 to +-5,
 *   loaded on top of what is there (_year_search_order).
 * - A windowed frame takes the masters' window: CCDSEC, else LTV1/LTV2
 *   (_parse_ccdsec). A full frame is cut to the chip's active area
 *   (CAMERA_CROPS, copied below: four entries).
 *
 * Not as asarina: a frame missing a master is not refused (asarina
 * corrects nothing then) - what there is applies, and the text says what
 * is missing; with neither, the preview is raw. [JS 2026-10-28]
 */

namespace fs = std::filesystem;
using namespace rts2web;

namespace
{

const int TEMP_GROUPING = 5;		// asarina's temp_grouping
const double EXPOSURE_TOLERANCE = 3.0;	// s, exposure_tolerance
const int MAX_YEAR_SEARCH = 5;		// max_year_search

// the header fields pyrt's registry reads (CAMERA_PATTERNS,
// COMPOUND_PATTERNS, the CCD_NAME family fallback): a chip id is kept per
// combination of their values. A field the registry starts to read and
// that is missing here would mix cameras that differ only in it.
const char *REGISTRY_FIELDS[] = {"CCD_NAME", "CCD_SER", "CCD_TYPE", "DESCRIPTION", "INSTRUME", "PRODUCT_ID", "SERIAL"};

// asarina's CAMERA_CROPS, the active area of a full frame: {x0, x1, y0,
// y1}, 0-based, end exclusive (numpy's [y0:y1, x0:x1])
struct Crop { long x0, x1, y0, y1; };
const std::map <std::string, Crop> CAMERA_CROPS = {
	{"mi6166", {29, 4127, 10, 4108}},
	{"mi6167", {29, 4127, 10, 4108}},
	{"mi0008", {34, 2218, 4, 1476}},
	{"mi0315", {215, 1414, 19, 1218}},	// Makak zenith camera
};

std::string trim (const std::string &s)
{
	size_t b = s.find_first_not_of (' ');
	size_t e = s.find_last_not_of (' ');
	return b == std::string::npos ? "" : s.substr (b, e - b + 1);
}

// a keyword as its text (strings without quotes), or "" when absent
std::string keyText (fitsfile *f, const char *key)
{
	char val[FLEN_VALUE] = "";
	int status = 0;
	fits_read_keyword (f, key, val, nullptr, &status);
	if (status)
		return "";
	std::string v = trim (val);
	if (v.size () >= 2 && v.front () == '\'' && v.back () == '\'')
		v = trim (v.substr (1, v.size () - 2));
	return v;
}

bool keyDouble (fitsfile *f, const char *key, double &val)
{
	int status = 0;
	fits_read_key (f, TDOUBLE, key, &val, nullptr, &status);
	return status == 0;
}

// asarina's _round_temperature
int roundTemperature (double temp)
{
	if (temp >= 0)
		return TEMP_GROUPING * (int) (temp / TEMP_GROUPING + 0.5);
	return TEMP_GROUPING * (int) (temp / TEMP_GROUPING + 0.5 - 1);
}

std::string fingerprint (fitsfile *f)
{
	std::string fp;
	for (const char *field : REGISTRY_FIELDS)
		fp += keyText (f, field) + '\x1f';
	return fp;
}

// --- the camera registry ----------------------------------------------------

std::mutex calibMutex;				// guards everything below
std::map <std::string, std::string> chipIds;	// fingerprint -> chip id

// Chip ids for files whose fingerprints are not known yet: one call of
// the registry tool for all of them (one file per fingerprint). A tool
// that fails leaves them "unknown".
void askRegistry (const std::string &cmd, const std::map <std::string, std::string> &fileOfFingerprint)
{
	std::vector <std::pair <std::string, std::string>> todo;
	for (const auto &ff : fileOfFingerprint)
		if (!chipIds.count (ff.first))
			todo.push_back (ff);
	if (todo.empty ())
		return;
	std::string line = cmd;
	for (const auto &t : todo)
	{
		// paths come from the archive and the master directories; one
		// with a quote is left to "unknown" rather than quoted
		if (t.second.find ('\'') == std::string::npos)
			line += " '" + t.second + "'";
	}
	// the tool reads file names from stdin when it is not a terminal
	line += " </dev/null 2>/dev/null";
	std::map <std::string, std::string> idOfFile;
	FILE *p = popen (line.c_str (), "r");
	if (p)
	{
		char buf[4096];
		while (fgets (buf, sizeof (buf), p))
		{
			std::string l (buf);
			while (!l.empty () && (l.back () == '\n' || l.back () == '\r'))
				l.pop_back ();
			size_t tab = l.find ('\t');
			if (tab != std::string::npos)
			{
				std::string id = l.substr (tab + 1);
				size_t tab2 = id.find ('\t');
				idOfFile[l.substr (0, tab)] = trim (tab2 == std::string::npos ? id : id.substr (0, tab2));
			}
		}
		pclose (p);
	}
	for (const auto &t : todo)
	{
		std::map <std::string, std::string>::iterator it = idOfFile.find (t.second);
		chipIds[t.first] = it != idOfFile.end () && !it->second.empty () && it->second.compare (0, 6, "ERROR:") ? it->second : "unknown";
	}
}

// --- master indexes -----------------------------------------------------------

struct Master
{
	std::string path;
	bool dark;						// else a flat
	std::string chip;
	int temp;						// rounded (darks)
	double exptime;					// (darks)
	std::string filter;				// (flats)
	long binx;
	long w, h;
};

struct MasterDir
{
	time_t mtime;
	std::vector <Master> masters;
};

std::map <std::string, MasterDir> masterDirs;	// the masters of a directory, by its mtime

// The masters in dir, read once and again when the directory changes;
// sorted by name (asarina takes them in the order the file system lists
// them, which only matters for two masters of the same key).
const std::vector <Master> &mastersIn (const std::string &dir, const std::string &chipIdCmd)
{
	static const std::vector <Master> none;
	struct stat st;
	if (stat (dir.c_str (), &st) != 0 || !S_ISDIR (st.st_mode))
		return none;
	std::map <std::string, MasterDir>::iterator known = masterDirs.find (dir);
	if (known != masterDirs.end () && known->second.mtime == st.st_mtime)
		return known->second.masters;

	std::vector <std::string> files;
	std::error_code ec;
	for (const fs::directory_entry &e : fs::directory_iterator (dir, ec))
	{
		std::string ext = e.path ().extension ().string ();
		if (ext == ".fits" || ext == ".fit" || ext == ".FITS" || ext == ".FIT")
			files.push_back (e.path ().string ());
	}
	std::sort (files.begin (), files.end ());

	std::vector <Master> masters;
	std::vector <std::string> fps;
	std::map <std::string, std::string> fileOfFingerprint;
	for (const std::string &path : files)
	{
		fitsfile *f;
		int status = 0;
		if (fits_open_file (&f, path.c_str (), READONLY, &status))
			continue;
		Master m;
		m.path = path;
		std::string type = keyText (f, "IMAGETYP");
		m.dark = type == "mdark";
		double temp = NAN, binx = 1;
		bool ok = type == "mdark" || type == "mflat";
		if (m.dark)
			ok = ok && keyDouble (f, "CCD_TEMP", temp) && keyDouble (f, "EXPTIME", m.exptime);
		else
			m.filter = keyText (f, "FILTER");
		keyDouble (f, "BINX", binx);
		m.temp = std::isnan (temp) ? 0 : roundTemperature (temp);
		m.binx = (long) binx;
		long axes[2] = {0, 0};
		fits_get_img_size (f, 2, axes, &status);
		m.w = axes[0];
		m.h = axes[1];
		std::string fp = fingerprint (f);
		status = 0;
		fits_close_file (f, &status);
		if (!ok || m.w <= 0 || m.h <= 0)
			continue;
		masters.push_back (m);
		fps.push_back (fp);
		fileOfFingerprint.emplace (fp, path);
	}
	askRegistry (chipIdCmd, fileOfFingerprint);
	for (size_t i = 0; i < masters.size (); i++)
		masters[i].chip = chipIds[fps[i]];

	MasterDir &md = masterDirs[dir];
	md.mtime = st.st_mtime;
	md.masters = masters;
	return md.masters;
}

// a / b, with one slash between (Python's Path / as asarina joins them)
std::string joinPath (const std::string &a, const std::string &b)
{
	if (a.empty () || a.back () == '/')
		return a + b;
	return a + "/" + b;
}

std::string expandYear (std::string t, int year)
{
	for (size_t p; (p = t.find ("{year}")) != std::string::npos; )
		t.replace (p, 6, std::to_string (year));
	return t;
}

// The masters loaded for one frame, asarina's master_darks/master_flats:
// cumulative over the years searched, the first master of a key kept.
struct Loaded
{
	const CalibConfig &config;
	std::string chip, ccdName;
	std::set <int> years;
	std::set <std::string> paths;
	// (chip, temp, binx) -> darks in load order; (chip, filter, binx) -> flat
	std::map <std::tuple <std::string, int, long>, std::vector <const Master *>> darks;
	std::map <std::tuple <std::string, std::string, long>, const Master *> flats;
	std::vector <std::string> dirsSeen;

	explicit Loaded (const CalibConfig &c):config (c) {}

	// _load_calibration_year: (darks, flats) new from this year
	std::pair <int, int> loadYear (int year)
	{
		if (!years.insert (year).second)
			return {0, 0};
		std::vector <std::string> dirs;
		if (!config.root.empty () && !chip.empty () && chip != "unknown")
			dirs.push_back (joinPath (joinPath (config.root, chip), std::to_string (year)));
		if (!config.dirTemplate.empty ())
		{
			std::string base = expandYear (config.dirTemplate, year);
			dirs.push_back (ccdName.empty () ? base : joinPath (base, ccdName));
		}
		int nd = 0, nf = 0;
		for (const std::string &dir : dirs)
		{
			const std::vector <Master> &ms = mastersIn (dir, config.chipIdCmd);
			if (!ms.empty ())
				dirsSeen.push_back (dir);
			for (const Master &m : ms)
			{
				if (!paths.insert (m.path).second)
					continue;
				if (m.dark)
				{
					std::vector <const Master *> &v = darks[std::make_tuple (m.chip, m.temp, m.binx)];
					bool have = std::any_of (v.begin (), v.end (), [&m] (const Master *o) { return o->exptime == m.exptime; });
					if (!have)
					{
						v.push_back (&m);
						nd++;
					}
				}
				else if (flats.emplace (std::make_tuple (m.chip, m.filter, m.binx), &m).second)
					nf++;
			}
		}
		return {nd, nf};
	}

	const Master *darkInMemory (int temp, long binx, double expt)
	{
		auto it = darks.find (std::make_tuple (chip, temp, binx));
		if (it == darks.end ())
			return nullptr;
		const Master *best = nullptr;
		double minDiff = INFINITY;
		for (const Master *m : it->second)
			if (std::fabs (expt - m->exptime) < minDiff)
			{
				minDiff = std::fabs (expt - m->exptime);
				best = m;
			}
		return minDiff <= EXPOSURE_TOLERANCE ? best : nullptr;
	}

	const Master *flatInMemory (const std::string &filter, long binx)
	{
		auto it = flats.find (std::make_tuple (chip, filter, binx));
		return it == flats.end () ? nullptr : it->second;
	}
};

// _year_search_order: -1, +1, -2, +2, ...
std::vector <int> yearSearchOrder (int year)
{
	std::vector <int> order;
	for (int i = 1; i <= MAX_YEAR_SEARCH; i++)
	{
		order.push_back (year - i);
		order.push_back (year + i);
	}
	return order;
}

// _parse_ccdsec: the window as {x0, y0} (0-based), false for a full frame
bool parseWindow (fitsfile *f, long w, long h, long &x0, long &y0)
{
	std::string ccdsec = keyText (f, "CCDSEC");
	if (!ccdsec.empty ())
	{
		long x1, x2, y1, y2;
		if (sscanf (ccdsec.c_str (), "[%ld:%ld,%ld:%ld]", &x1, &x2, &y1, &y2) == 4)
		{
			x0 = x1 - 1;
			y0 = y1 - 1;
			return true;
		}
		return false;
	}
	double ltv1 = 0, ltv2 = 0;
	keyDouble (f, "LTV1", ltv1);
	keyDouble (f, "LTV2", ltv2);
	if (ltv1 == 0 && ltv2 == 0)
		return false;
	x0 = (long) ltv1;
	y0 = (long) ltv2;
	return w > 0 && h > 0;
}

}

void rts2web::findCalibration (const CalibConfig &config, const std::string &fullPath, CalibFrames &out)
{
	out = CalibFrames ();
	fitsfile *f;
	int status = 0;
	if (fits_open_file (&f, fullPath.c_str (), READONLY, &status))
	{
		out.text = "raw: cannot read the frame's header";
		return;
	}
	long axes[2] = {0, 0};
	fits_get_img_size (f, 2, axes, &status);
	out.w = axes[0];
	out.h = axes[1];
	std::string date = keyText (f, "DATE-OBS");
	std::string ccdName = keyText (f, "CCD_NAME");
	std::string filter = keyText (f, "FILTER");
	double temp = NAN, expt = NAN, binx = 1;
	bool haveTemp = keyDouble (f, "CCD_TEMP", temp);
	bool haveExpt = keyDouble (f, "EXPTIME", expt);
	keyDouble (f, "BINX", binx);
	long wx0 = 0, wy0 = 0;
	bool windowed = parseWindow (f, out.w, out.h, wx0, wy0);
	std::string fp = fingerprint (f);
	status = 0;
	fits_close_file (f, &status);

	int year = atoi (date.substr (0, date.find ('-')).c_str ());

	std::lock_guard <std::mutex> lock (calibMutex);
	askRegistry (config.chipIdCmd, {{fp, fullPath}});
	Loaded loaded (config);
	loaded.chip = chipIds[fp];
	loaded.ccdName = ccdName;

	const Master *dark = nullptr, *flat = nullptr;
	int rtemp = haveTemp ? roundTemperature (temp) : 0;
	if (year > 0)
	{
		loaded.loadYear (year);
		// find_best_dark, find_flat: this year, then the others
		if (haveTemp && haveExpt)
		{
			dark = loaded.darkInMemory (rtemp, (long) binx, expt);
			for (int y : yearSearchOrder (year))
			{
				if (dark)
					break;
				if (loaded.loadYear (y).first > 0)
					dark = loaded.darkInMemory (rtemp, (long) binx, expt);
			}
		}
		flat = loaded.flatInMemory (filter, (long) binx);
		for (int y : yearSearchOrder (year))
		{
			if (flat)
				break;
			if (loaded.loadYear (y).second > 0)
				flat = loaded.flatInMemory (filter, (long) binx);
		}
	}

	// the frame in the masters: a window there, or the whole master
	auto fits = [&] (const Master *m) {
		return m && (windowed ? wx0 >= 0 && wy0 >= 0 && wx0 + out.w <= m->w && wy0 + out.h <= m->h
			: m->w == out.w && m->h == out.h);
	};
	std::ostringstream whyDark, whyFlat;
	if (!dark)
	{
		whyDark << "no dark (chip " << loaded.chip;
		if (haveTemp)
			whyDark << ", " << rtemp << " C";
		whyDark << ", bin " << (long) binx;
		if (haveExpt)
			whyDark << ", " << expt << " s";
		whyDark << ")";
	}
	else if (!fits (dark))
	{
		whyDark << "dark of another size (" << dark->path << ")";
		dark = nullptr;
	}
	if (!flat)
		whyFlat << "no flat for " << (filter.empty () ? std::string ("this filter") : filter) << " (chip " << loaded.chip << ", bin " << (long) binx << ")";
	else if (!fits (flat))
	{
		whyFlat << "flat of another size (" << flat->path << ")";
		flat = nullptr;
	}

	if (dark)
		out.dark = dark->path;
	if (flat)
		out.flat = flat->path;
	if (windowed)
	{
		out.x0 = wx0;
		out.y0 = wy0;
	}
	out.cropW = out.w;
	out.cropH = out.h;
	std::map <std::string, Crop>::const_iterator crop = CAMERA_CROPS.find (loaded.chip);
	if ((dark || flat) && !windowed && crop != CAMERA_CROPS.end () && crop->second.x1 <= out.w && crop->second.y1 <= out.h)
	{
		out.cropX0 = crop->second.x0;
		out.cropY0 = crop->second.y0;
		out.cropW = crop->second.x1 - crop->second.x0;
		out.cropH = crop->second.y1 - crop->second.y0;
	}

	if (dark && flat)
		out.text = "dark and flat corrected";
	else if (dark)
		out.text = "dark only, " + whyFlat.str ();
	else if (flat)
		out.text = "flat only, " + whyDark.str ();
	else
		out.text = "raw: " + whyDark.str () + ", " + whyFlat.str ();
}
