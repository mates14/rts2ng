#include "preview.h"

#include "image.h"

#include <jpeglib.h>
#include <csetjmp>

#include <algorithm>
#include <cstdio>
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

// Shared by checkPreviewCache() and generatePreview(): resolves and
// validates relPath, filling fullPath/srcStat on success.
bool validatePath (const std::string &imagesDir, const std::string &relPath, std::string &fullPath, struct stat &srcStat, std::string &errorMsg)
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
	if (ec || cf.compare (0, cr.size (), cr) != 0)
	{
		errorMsg = "invalid path";
		return false;
	}

	return true;
}

std::string cacheFileFor (const std::string &cacheDir, const std::string &relPath, int previewSize, float quantiles)
{
	char suffix[64];
	snprintf (suffix, sizeof (suffix), ".ps%d.q%04d.jpg", previewSize, (int) (quantiles * 10000));
	return cacheDir + "/" + relPath + suffix;
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

bool encodeJpeg (const unsigned char *gray, int width, int height, std::string &out)
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
	jpeg_set_quality (&cinfo, 85, TRUE);

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

rts2web::PreviewStatus rts2web::checkPreviewCache (const std::string &imagesDir, const std::string &cacheDir, const std::string &relPath, int previewSize, float quantiles, std::string &jpegDataOrError)
{
	std::string fullPath;
	struct stat srcStat;
	if (!validatePath (imagesDir, relPath, fullPath, srcStat, jpegDataOrError))
		return PreviewStatus::Invalid;

	std::string cacheFile = cacheFileFor (cacheDir, relPath, previewSize, quantiles);

	struct stat cacheStat;
	if (stat (cacheFile.c_str (), &cacheStat) == 0 && cacheStat.st_mtime >= srcStat.st_mtime)
	{
		std::ifstream in (cacheFile, std::ios::binary);
		if (in)
		{
			std::ostringstream ss;
			ss << in.rdbuf ();
			jpegDataOrError = ss.str ();
			return PreviewStatus::Hit;
		}
		// cache file vanished/became unreadable between stat() and
		// open() - fall through and report a miss, generatePreview()
		// will recreate it.
	}

	return PreviewStatus::Miss;
}

bool rts2web::generatePreview (const std::string &imagesDir, const std::string &cacheDir, const std::string &relPath, int previewSize, float quantiles, std::string &jpegData, std::string &errorMsg)
{
	std::string fullPath;
	struct stat srcStat;
	if (!validatePath (imagesDir, relPath, fullPath, srcStat, errorMsg))
		return false;

	int srcW = 0, srcH = 0;
	unsigned char *buf = nullptr;

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

	if (!encodeJpeg (small.data (), dstW, dstH, jpegData))
	{
		errorMsg = "JPEG encoding failed";
		return false;
	}

	std::string cacheFile = cacheFileFor (cacheDir, relPath, previewSize, quantiles);
	fs::path cachePathObj (cacheFile);
	std::error_code mkec;
	fs::create_directories (cachePathObj.parent_path (), mkec);

	std::ofstream out (cacheFile, std::ios::binary | std::ios::trunc);
	if (out)
		out.write (jpegData.data (), jpegData.size ());

	return true;
}
