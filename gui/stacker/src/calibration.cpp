#include "stacker/calibration.h"

#include <fitsio.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <sstream>

using namespace stacker;

namespace
{
	std::string lower (std::string s)
	{
		std::transform (s.begin (), s.end (), s.begin (), [] (unsigned char c) { return std::tolower (c); });
		return s;
	}

	std::vector<std::string> splitName (const std::string &stem)
	{
		std::vector<std::string> tokens;
		std::stringstream ss (stem);
		std::string tok;
		while (std::getline (ss, tok, '-'))
			tokens.push_back (tok);
		return tokens;
	}

	std::string fitsError (int status)
	{
		char text[FLEN_STATUS];
		fits_get_errstatus (status, text);
		return text;
	}

	/** String keyword, or "" if absent. */
	std::string readKeyString (fitsfile *f, const char *key)
	{
		char value[FLEN_VALUE];
		int status = 0;
		if (fits_read_key (f, TSTRING, key, value, nullptr, &status))
			return "";
		return value;
	}

	bool isFitsName (const std::string &name)
	{
		std::string l = lower (name);
		for (const char *ext : { ".fits", ".fit", ".fts", ".fits.fz", ".fits.gz" })
		{
			std::string e (ext);
			if (l.size () > e.size () && l.compare (l.size () - e.size (), e.size (), e) == 0)
				return true;
		}
		return false;
	}

	/** Basename without any of the extensions isFitsName() accepts. */
	std::string stemOf (const std::string &name)
	{
		std::string l = lower (name);
		for (const char *ext : { ".fits.fz", ".fits.gz", ".fits", ".fit", ".fts" })
		{
			std::string e (ext);
			if (l.size () > e.size () && l.compare (l.size () - e.size (), e.size (), e) == 0)
				return name.substr (0, name.size () - e.size ());
		}
		return name;
	}
}

bool CalibrationLibrary::scan (const std::string &_dir, std::string &err)
{
	namespace fs = std::filesystem;

	dir = _dir;
	darkFrames.clear ();
	flatFrames.clear ();

	std::error_code ec;
	fs::directory_iterator it (dir, ec);
	if (ec)
	{
		err = "cannot read calibration directory " + dir + ": " + ec.message ();
		return false;
	}

	for (const auto &entry : it)
	{
		if (!entry.is_regular_file () || !isFitsName (entry.path ().filename ().string ()))
			continue;

		CalibFrame frame;
		frame.path = entry.path ().string ();
		frame.name = entry.path ().filename ().string ();
		std::vector<std::string> tokens = splitName (stemOf (frame.name));

		fitsfile *f = nullptr;
		int status = 0;
		if (fits_open_image (&f, frame.path.c_str (), READONLY, &status))
			continue;

		int naxis = 0;
		long naxes[2] = { 0, 0 };
		fits_get_img_dim (f, &naxis, &status);
		fits_get_img_size (f, 2, naxes, &status);
		if (status || naxis != 2)
		{
			fits_close_file (f, &status);
			continue;
		}
		frame.width = naxes[0];
		frame.height = naxes[1];

		std::string type = lower (readKeyString (f, "IMAGETYP"));
		std::string prefix = tokens.empty () ? "" : lower (tokens[0]);
		bool isDark = type.find ("dark") != std::string::npos || (type.find ("flat") == std::string::npos && prefix == "dark");
		bool isFlat = !isDark && (type.find ("flat") != std::string::npos || prefix == "flat");

		if (isDark)
		{
			double exptime = NAN;
			int st = 0;
			if (fits_read_key (f, TDOUBLE, "EXPTIME", &exptime, nullptr, &st))
			{
				// dark-<temp>-<exptime>-<bin>-<size>
				exptime = tokens.size () > 2 ? std::strtod (tokens[2].c_str (), nullptr) : NAN;
			}
			if (!std::isnan (exptime))
			{
				frame.exptime = exptime;
				darkFrames.push_back (frame);
			}
		}
		else if (isFlat)
		{
			frame.filter = readKeyString (f, "FILTER");
			// flat-<filter>-<bin>
			if (frame.filter.empty () && tokens.size () > 1)
				frame.filter = tokens[1];
			if (!frame.filter.empty ())
				flatFrames.push_back (frame);
		}

		status = 0;
		fits_close_file (f, &status);
	}

	if (darkFrames.empty ())
	{
		err = "no master darks found in " + dir;
		return false;
	}

	std::sort (darkFrames.begin (), darkFrames.end (), [] (const CalibFrame &a, const CalibFrame &b) { return a.exptime < b.exptime; });
	std::sort (flatFrames.begin (), flatFrames.end (), [] (const CalibFrame &a, const CalibFrame &b) { return a.filter < b.filter; });
	return true;
}

const CalibFrame *CalibrationLibrary::findDark (double exptime, long width, long height) const
{
	for (const auto &d : darkFrames)
		if (sameExptime (d.exptime, exptime) && d.width == width && d.height == height)
			return &d;
	return nullptr;
}

const CalibFrame *CalibrationLibrary::findFlat (const std::string &filter, long width, long height) const
{
	for (const auto &f : flatFrames)
		if (f.filter == filter && f.width == width && f.height == height)
			return &f;
	std::string l = lower (filter);
	for (const auto &f : flatFrames)
		if (lower (f.filter) == l && f.width == width && f.height == height)
			return &f;
	return nullptr;
}

const std::vector<float> *CalibrationLibrary::pixels (const CalibFrame *frame, std::string &err)
{
	std::lock_guard<std::mutex> lock (cacheMutex);

	auto it = cache.find (frame->path);
	if (it != cache.end ())
		return &it->second;

	fitsfile *f = nullptr;
	int status = 0;
	if (fits_open_image (&f, frame->path.c_str (), READONLY, &status))
	{
		err = frame->name + ": " + fitsError (status);
		return nullptr;
	}

	long n = frame->width * frame->height;
	std::vector<float> data (n);
	float nulval = 0;
	int anynul = 0;
	fits_read_img (f, TFLOAT, 1, n, &nulval, data.data (), &anynul, &status);
	int closeStatus = 0;
	fits_close_file (f, &closeStatus);
	if (status)
	{
		err = frame->name + ": " + fitsError (status);
		return nullptr;
	}

	bool isFlat = !frame->filter.empty ();
	if (isFlat)
	{
		std::vector<float> sorted (data);
		size_t mid = sorted.size () / 2;
		std::nth_element (sorted.begin (), sorted.begin () + mid, sorted.end ());
		float median = sorted[mid];
		if (!(median > 0))
		{
			err = frame->name + ": flat has non-positive median";
			return nullptr;
		}
		for (auto &v : data)
			v /= median;
	}

	return &(cache[frame->path] = std::move (data));
}
