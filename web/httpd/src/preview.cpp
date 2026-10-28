#include "preview.h"

#include "image.h"

#include "calib.h"

#include <fitsio.h>
#include <jpeglib.h>
#include <csetjmp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{

// Reject anything that could escape imagesDir before even touching the
// filesystem - a bare ".." path-segment check. validatePath() below adds
// a second, filesystem-level check (weakly_canonical + prefix compare)
// on top of this one.
bool looksSafe (const std::string &relPath)
{
	if (relPath.empty () || relPath[0] == '/')
		return false;
	size_t pos = 0;
	while (true)
	{
		size_t slash = relPath.find ('/', pos);
		std::string segment = relPath.substr (pos, slash == std::string::npos ? std::string::npos : slash - pos);
		if (segment == "..")
			return false;
		if (slash == std::string::npos)
			break;
		pos = slash + 1;
	}
	return true;
}

}

// Shared by checkPreviewCache(), generatePreview() and /fits/: resolves
// and validates relPath, filling fullPath/srcStat on success.
bool rts2web::resolveImagePath (const std::string &imagesDir, const std::string &relPath, std::string &fullPath, struct stat &srcStat, std::string &errorMsg)
{
	if (!looksSafe (relPath))
	{
		errorMsg = "invalid path";
		return false;
	}

	fullPath = imagesDir + "/" + relPath;

	if (stat (fullPath.c_str (), &srcStat) != 0)
	{
		errorMsg = "image not found";
		return false;
	}

	std::error_code ec;
	fs::path canonicalFull = fs::weakly_canonical (fullPath, ec);
	fs::path canonicalRoot = fs::weakly_canonical (imagesDir, ec);
	std::string cf = canonicalFull.string ();
	std::string cr = canonicalRoot.string ();
	// inside the root: the root itself followed by a "/" (a bare prefix
	// compare let /images2/x through for an imagesDir of /images)
	if (ec || cf.compare (0, cr.size (), cr) != 0 || (cf.size () > cr.size () && cr.back () != '/' && cf[cr.size ()] != '/'))
	{
		errorMsg = "invalid path";
		return false;
	}
	if (!S_ISREG (srcStat.st_mode))
	{
		errorMsg = "not a file";
		return false;
	}

	return true;
}

namespace
{

// ".raw.jpg" or ".cal.jpg" (dark/flat corrected, its calibration text in
// a ".cal.jpg.txt" beside it): the two variants never share a file
std::string cacheFileFor (const std::string &cacheDir, const std::string &relPath, int previewSize, float quantiles, int jpegQuality, bool calibrated)
{
	char suffix[64];
	snprintf (suffix, sizeof (suffix), ".ps%d.q%04d.jq%d.%s.jpg", previewSize, (int) (quantiles * 10000), jpegQuality, calibrated ? "cal" : "raw");
	return cacheDir + "/" + relPath + suffix;
}

// --- dark/flat correction ------------------------------------------------
//
// The masters and how they apply come from findCalibration () (calib.cpp:
// asarina's choice of masters, ported). Here they are applied, (data -
// dark) / flat, and the frame rendered as the kernel renders a raw one.
// What applied goes to the response's X-RTS2-Calibration. [JS 2026-10-28]

// rows [y, y + n) of the columns [x, x + w) of a 2-D image as floats
// (BZERO/BSCALE applied)
bool readBlock (fitsfile *f, long x, long w, long y, long n, float *out)
{
	int status = 0;
	long fpixel[2] = {x + 1, y + 1};
	long lpixel[2] = {x + w, y + n};
	long inc[2] = {1, 1};
	fits_read_subset (f, TFLOAT, fpixel, lpixel, inc, nullptr, out, nullptr, &status);
	return status == 0;
}

// The corrected frame as the kernel renders a raw one
// (Image::getChannelGrayscaleByteBuffer): the low quantile white, the high
// one black, linear between, rows flipped; cut to the chip's active area
// when calibration says so. Read in strips, so that only the corrected
// frame is held whole. False (errorMsg says why) when nothing applies or
// a file cannot be read.
bool renderCalibrated (const std::string &fullPath, const rts2web::CalibConfig &config, float quantiles, std::vector <unsigned char> &gray, int &w, int &h, std::string &calibration, std::string &masters, std::string &errorMsg)
{
	rts2web::CalibFrames cal;
	rts2web::findCalibration (config, fullPath, cal);
	calibration = cal.text;
	masters = (cal.dark.empty () ? "" : "dark=" + cal.dark) + (cal.dark.empty () || cal.flat.empty () ? "" : "; ") + (cal.flat.empty () ? "" : "flat=" + cal.flat);
	if (cal.dark.empty () && cal.flat.empty ())
	{
		errorMsg = cal.text;
		return false;
	}

	fitsfile *img = nullptr, *fd = nullptr, *ff = nullptr;
	int status = 0;
	bool ok = !fits_open_file (&img, fullPath.c_str (), READONLY, &status);
	// DATASEC, the good data section ("[x1:x2,y1:y2]", 1-based, written by
	// camd --datasec): the stretch is taken from it alone, as the kernel
	// does for a raw preview (Image::getChannelHistogram), so that an
	// overscan does not set the black level. Without it, the whole frame.
	long ds[4] = {0, 0, 0, 0};
	bool haveDatasec = false;
	if (ok)
	{
		char val[FLEN_VALUE] = "";
		int dstatus = 0;
		fits_read_key (img, TSTRING, "DATASEC", val, nullptr, &dstatus);
		haveDatasec = dstatus == 0 && sscanf (val, " [%ld:%ld,%ld:%ld]", ds, ds + 1, ds + 2, ds + 3) == 4;
	}
	status = 0;
	ok = ok && (cal.dark.empty () || !fits_open_file (&fd, cal.dark.c_str (), READONLY, &status));
	status = 0;
	ok = ok && (cal.flat.empty () || !fits_open_file (&ff, cal.flat.c_str (), READONLY, &status));

	const long STRIP = 64;
	long cw = cal.cropW, ch = cal.cropH;
	std::vector <float> data;
	if (ok)
		data.resize ((size_t) cw * ch);
	std::vector <float> d (fd ? (size_t) cw * STRIP : 0), fl (ff ? (size_t) cw * STRIP : 0);
	for (long y = 0; ok && y < ch; y += STRIP)
	{
		long n = std::min (STRIP, ch - y);
		float *row = data.data () + (size_t) y * cw;
		// the frame's crop, and the same pixels in the masters (offset by
		// the frame's window there)
		ok = readBlock (img, cal.cropX0, cw, cal.cropY0 + y, n, row)
			&& (!fd || readBlock (fd, cal.x0 + cal.cropX0, cw, cal.y0 + cal.cropY0 + y, n, d.data ()))
			&& (!ff || readBlock (ff, cal.x0 + cal.cropX0, cw, cal.y0 + cal.cropY0 + y, n, fl.data ()));
		for (size_t i = 0; ok && i < (size_t) cw * n; i++)
		{
			float v = row[i];
			if (fd)
				v -= d[i];
			// a dead flat pixel is left as it is
			if (ff && fl[i] > 0.01f)
				v /= fl[i];
			row[i] = v;
		}
	}
	for (fitsfile *f : {img, fd, ff})
		if (f)
		{
			status = 0;
			fits_close_file (f, &status);
		}
	if (!ok)
	{
		errorMsg = "cannot read the frame or a master";
		return false;
	}

	// quantiles from up to a million evenly spaced pixels of the good data
	// section (in the crop's coordinates), or of all when it is not given
	// or does not overlap the crop
	long qx0 = 0, qx1 = cw, qy0 = 0, qy1 = ch;
	if (haveDatasec)
	{
		long x0 = std::max (0L, ds[0] - 1 - cal.cropX0), x1 = std::min (cw, ds[1] - cal.cropX0);
		long y0 = std::max (0L, ds[2] - 1 - cal.cropY0), y1 = std::min (ch, ds[3] - cal.cropY0);
		if (x0 < x1 && y0 < y1)
		{
			qx0 = x0;
			qx1 = x1;
			qy0 = y0;
			qy1 = y1;
		}
	}
	size_t total = data.size ();
	size_t region = (size_t) (qx1 - qx0) * (qy1 - qy0);
	size_t stride = std::max ((size_t) 1, region / 1000000);
	std::vector <float> sample;
	sample.reserve (region / stride + 1);
	for (size_t k = 0; k < region; k += stride)
	{
		long y = qy0 + k / (qx1 - qx0), x = qx0 + k % (qx1 - qx0);
		float v = data[(size_t) y * cw + x];
		if (std::isfinite (v))
			sample.push_back (v);
	}
	if (sample.empty ())
	{
		errorMsg = "no finite pixels";
		return false;
	}
	size_t lo = std::min (sample.size () - 1, (size_t) (quantiles * sample.size ()));
	size_t hi = std::min (sample.size () - 1, (size_t) ((1 - quantiles) * sample.size ()));
	std::nth_element (sample.begin (), sample.begin () + lo, sample.end ());
	float low = sample[lo];
	std::nth_element (sample.begin (), sample.begin () + hi, sample.end ());
	float high = sample[hi];

	w = cw;
	h = ch;
	gray.resize (total);
	for (long y = 0; y < ch; y++)
	{
		const float *src = data.data () + (size_t) y * cw;
		unsigned char *dst = gray.data () + (size_t) (ch - 1 - y) * cw;
		for (long x = 0; x < cw; x++)
		{
			float v = src[x];
			if (!std::isfinite (v) || v <= low)
				dst[x] = 255;
			else if (v >= high)
				dst[x] = 0;
			else
				dst[x] = (unsigned char) (255 - 255 * (v - low) / (high - low));
		}
	}
	return true;
}

// libjpeg's default error handler calls exit() - long jump out instead
// so a malformed/short-read FITS buffer can't take the whole daemon down.
struct JpegErrorMgr
{
	struct jpeg_error_mgr pub;
	jmp_buf setjmpBuffer;
};

void jpegErrorExit (j_common_ptr cinfo)
{
	JpegErrorMgr *err = (JpegErrorMgr *) cinfo->err;
	longjmp (err->setjmpBuffer, 1);
}

// Box-average downsample of an 8-bit single-channel image from
// (srcW x srcH) to (dstW x dstH). Only runs on a cache miss.
std::vector <unsigned char> downsample (const unsigned char *src, int srcW, int srcH, int dstW, int dstH)
{
	std::vector <unsigned char> dst (dstW * dstH);
	for (int y = 0; y < dstH; y++)
	{
		int sy0 = (int) ((long) y * srcH / dstH);
		int sy1 = (int) ((long) (y + 1) * srcH / dstH);
		if (sy1 <= sy0)
			sy1 = sy0 + 1;
		for (int x = 0; x < dstW; x++)
		{
			int sx0 = (int) ((long) x * srcW / dstW);
			int sx1 = (int) ((long) (x + 1) * srcW / dstW);
			if (sx1 <= sx0)
				sx1 = sx0 + 1;

			long sum = 0;
			int n = 0;
			for (int sy = sy0; sy < sy1 && sy < srcH; sy++)
			{
				for (int sx = sx0; sx < sx1 && sx < srcW; sx++)
				{
					sum += src[(size_t) sy * srcW + sx];
					n++;
				}
			}
			dst[(size_t) y * dstW + x] = n ? (unsigned char) (sum / n) : 0;
		}
	}
	return dst;
}

bool encodeJpeg (const unsigned char *gray, int width, int height, int quality, std::string &out)
{
	struct jpeg_compress_struct cinfo;
	JpegErrorMgr jerr;

	cinfo.err = jpeg_std_error (&jerr.pub);
	jerr.pub.error_exit = jpegErrorExit;

	if (setjmp (jerr.setjmpBuffer))
	{
		jpeg_destroy_compress (&cinfo);
		return false;
	}

	jpeg_create_compress (&cinfo);

	unsigned char *mem = nullptr;
	unsigned long memSize = 0;
	jpeg_mem_dest (&cinfo, &mem, &memSize);

	cinfo.image_width = width;
	cinfo.image_height = height;
	cinfo.input_components = 1;
	cinfo.in_color_space = JCS_GRAYSCALE;
	jpeg_set_defaults (&cinfo);
	jpeg_set_quality (&cinfo, quality, TRUE);

	jpeg_start_compress (&cinfo, TRUE);

	while (cinfo.next_scanline < cinfo.image_height)
	{
		JSAMPROW row = (JSAMPROW) (gray + (size_t) cinfo.next_scanline * width);
		jpeg_write_scanlines (&cinfo, &row, 1);
	}

	jpeg_finish_compress (&cinfo);

	out.assign ((const char *) mem, memSize);
	free (mem);

	jpeg_destroy_compress (&cinfo);
	return true;
}

}

rts2web::PreviewStatus rts2web::checkPreviewCache (const std::string &imagesDir, const std::string &cacheDir, const std::string &relPath, int previewSize, float quantiles, int jpegQuality, bool calibrated, std::string &jpegDataOrError, std::string &calibration, std::string &masters)
{
	std::string fullPath;
	struct stat srcStat;
	if (!resolveImagePath (imagesDir, relPath, fullPath, srcStat, jpegDataOrError))
		return PreviewStatus::Invalid;

	std::string cacheFile = cacheFileFor (cacheDir, relPath, previewSize, quantiles, jpegQuality, calibrated);

	struct stat cacheStat;
	if (stat (cacheFile.c_str (), &cacheStat) == 0 && cacheStat.st_mtime >= srcStat.st_mtime)
	{
		std::ifstream in (cacheFile, std::ios::binary);
		if (in)
		{
			std::ostringstream ss;
			ss << in.rdbuf ();
			jpegDataOrError = ss.str ();
			calibration = "raw";
			masters = "";
			if (calibrated)
			{
				std::ifstream txt (cacheFile + ".txt");
				std::getline (txt, calibration);
				std::getline (txt, masters);
			}
			return PreviewStatus::Hit;
		}
		// cache file vanished/became unreadable between stat() and
		// open() - fall through and report a miss, generatePreview()
		// will recreate it.
	}

	return PreviewStatus::Miss;
}

bool rts2web::generatePreview (const std::string &imagesDir, const std::string &cacheDir, const std::string &relPath, int previewSize, float quantiles, int jpegQuality, const CalibConfig *calib, std::string &jpegData, std::string &calibration, std::string &masters, std::string &errorMsg)
{
	std::string fullPath;
	struct stat srcStat;
	if (!resolveImagePath (imagesDir, relPath, fullPath, srcStat, errorMsg))
		return false;

	int srcW = 0, srcH = 0;
	unsigned char *buf = nullptr;

	// dark/flat corrected when asked (calib given) and possible;
	// otherwise, and whatever went wrong there, the raw preview below
	bool calibrated = calib != nullptr;
	calibration = "raw";
	masters = "";
	std::vector <unsigned char> corrected;
	if (calibrated)
	{
		std::string why;
		if (!renderCalibrated (fullPath, *calib, quantiles, corrected, srcW, srcH, calibration, masters, why))
		{
			corrected.clear ();
			masters = "";
			calibration = why.compare (0, 4, "raw:") == 0 ? why : "raw: " + why;
		}
	}

	// Not locked: each call opens its own independent rts2image::Image/
	// fitsfile* handle onto a different file - no state is shared
	// between concurrent calls here. cfitsio itself protects its one
	// genuinely global structure (the open-file table) internally on
	// this system - confirmed via `nm -D libcfitsio.so.10`, which shows
	// it linked against pthread_mutex_init/lock/unlock, i.e. built with
	// its internal locking enabled. The one other global cfitsio has -
	// a process-wide error-message stack, drained by getFitsErrors()'s
	// fits_read_errmsg() call inside FitsFile - is only ever touched on
	// a cfitsio *error* path (confirmed by reading fitsfile.cpp: every
	// call site is gated on `fits_status != 0`), and even then a race
	// between two simultaneously-failing threads can only swap which
	// thread's *log message text* comes out garbled - fits_status
	// itself is a plain per-instance int, so it can never affect
	// whether openFile() throws or what pixels get decoded. Accepted as
	// a negligible, understood risk rather than serialized away.
	rts2image::Image img;
	if (corrected.empty ())
	{
		try
		{
			img.openFile (fullPath.c_str (), true, false);
		}
		catch (rts2core::Error &er)
		{
			errorMsg = "cannot open image";
			return false;
		}

		img.loadChannels ();
		if (img.getChannelSize () == 0)
		{
			errorMsg = "image has no data";
			return false;
		}

		srcW = img.getChannelWidth (0);
		srcH = img.getChannelHeight (0);

		img.getChannelGrayscaleImage (img.getDataType (), 0, buf, quantiles, 0);

		if (buf == nullptr)
		{
			errorMsg = "cannot render preview";
			return false;
		}
	}
	else
	{
		buf = new unsigned char[corrected.size ()];
		memcpy (buf, corrected.data (), corrected.size ());
		std::vector <unsigned char> ().swap (corrected);
	}

	int dstW = srcW, dstH = srcH;
	if (srcW >= srcH && srcW > previewSize)
	{
		dstW = previewSize;
		dstH = std::max (1, (int) ((long) srcH * previewSize / srcW));
	}
	else if (srcH > previewSize)
	{
		dstH = previewSize;
		dstW = std::max (1, (int) ((long) srcW * previewSize / srcH));
	}

	std::vector <unsigned char> small = (dstW == srcW && dstH == srcH)
		? std::vector <unsigned char> (buf, buf + (size_t) srcW * srcH)
		: downsample (buf, srcW, srcH, dstW, dstH);
	delete[] buf;

	if (!encodeJpeg (small.data (), dstW, dstH, jpegQuality, jpegData))
	{
		errorMsg = "JPEG encoding failed";
		return false;
	}

	std::string cacheFile = cacheFileFor (cacheDir, relPath, previewSize, quantiles, jpegQuality, calibrated);
	fs::path cachePathObj (cacheFile);
	std::error_code mkec;
	fs::create_directories (cachePathObj.parent_path (), mkec);

	// the text first: a JPEG found without it reads as "raw"
	if (calibrated)
	{
		std::ofstream txt (cacheFile + ".txt", std::ios::trunc);
		txt << calibration << "\n" << masters << "\n";
	}
	std::ofstream out (cacheFile, std::ios::binary | std::ios::trunc);
	if (out)
		out.write (jpegData.data (), jpegData.size ());

	return true;
}

// Only what the cache writes goes: *.jpg and *.jpg.txt, then the
// directories left empty (deepest first) - so a cache-dir set to the
// archive by mistake loses no FITS file. The cache directory itself
// stays. [JS 2026-10-28]
bool rts2web::purgePreviewCache (const std::string &cacheDir, long &files, long long &bytes, std::string &errorMsg)
{
	files = 0;
	bytes = 0;
	std::error_code ec;
	fs::path root = fs::weakly_canonical (cacheDir, ec);
	if (ec || cacheDir.empty () || root == root.root_path ())
	{
		errorMsg = "refusing to purge " + cacheDir;
		return false;
	}
	if (!fs::is_directory (root, ec))
		return true;
	std::vector <fs::path> dirs;
	for (fs::recursive_directory_iterator it (root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment (ec))
	{
		if (ec)
			break;
		const fs::directory_entry &e = *it;
		if (e.is_symlink (ec))
		{
			// not followed, not deleted
			if (e.is_directory (ec))
				it.disable_recursion_pending ();
			continue;
		}
		if (e.is_directory (ec))
		{
			dirs.push_back (e.path ());
			continue;
		}
		std::string name = e.path ().filename ().string ();
		bool ours = (name.size () > 4 && name.compare (name.size () - 4, 4, ".jpg") == 0)
			|| (name.size () > 8 && name.compare (name.size () - 8, 8, ".jpg.txt") == 0);
		if (!ours || !e.is_regular_file (ec))
			continue;
		std::error_code sec;
		uintmax_t size = e.file_size (sec);
		if (fs::remove (e.path (), sec))
		{
			files++;
			bytes += sec ? 0 : size;
		}
	}
	// deepest first; a directory with anything else in it stays
	std::sort (dirs.begin (), dirs.end (), [] (const fs::path &a, const fs::path &b) { return a.string ().size () > b.string ().size (); });
	for (const fs::path &d : dirs)
	{
		std::error_code dec;
		if (fs::is_empty (d, dec) && !dec)
			fs::remove (d, dec);
	}
	return true;
}
