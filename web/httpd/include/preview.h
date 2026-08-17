#pragma once

#include <string>

namespace rts2web
{

/**
 * FITS -> JPEG preview generation with an on-disk cache (STATUS.md task
 * 3, "Image archive/preview performance"). Cache key is the image's
 * relative path plus requested size/quantile; freshness is checked via
 * mtime (images in the archive are immutable once written, so "cache
 * mtime >= source mtime" is a safe, simple invalidation signal - no
 * content hashing needed).
 *
 * Split into a cheap synchronous check and a separate expensive
 * generation call (STATUS.md task 5): checkPreviewCache() is just a
 * stat()+maybe-a-file-read, safe to call inline on Block's main
 * poll-loop thread; generatePreview() does the actual FITS decode +
 * JPEG encode and belongs on a worker thread so a burst of cache misses
 * can't stall bus traffic. See httpd.cpp's HttpD::handlePreview() for
 * how the two are wired together around MHD_suspend_connection().
 *
 * FITS decoding reuses base/kernel's already-ported rts2image::Image
 * (Image::getChannelGrayscaleImage()) rather than re-implementing FITS
 * parsing. JPEG encoding is done directly against libjpeg, replacing
 * classic's Magick++ dependency (see STATUS.md's "Dropped outright").
 * This first pass still quantile-stretches at full resolution before
 * downsampling to the requested preview size - STATUS.md's "downsample
 * before the quantile pass" refinement is a separate, later
 * optimization; the on-disk cache is what actually removes the repeat
 * cost, which is most of the real-world win.
 */

enum class PreviewStatus
{
	Hit,							 //!< jpegDataOrError holds the cached JPEG bytes
	Miss,							 //!< source image exists and is valid; call generatePreview() next
	Invalid							 //!< bad path or missing source; jpegDataOrError holds an error message, don't bother generating
};

/**
 * Cheap, synchronous check - safe to call on Block's main thread.
 * Validates relPath is safely contained within imagesDir, confirms the
 * source image exists, and checks for a fresh cached JPEG.
 */
PreviewStatus checkPreviewCache (const std::string &imagesDir, const std::string &cacheDir, const std::string &relPath, int previewSize, float quantiles, std::string &jpegDataOrError);

/**
 * Expensive: FITS decode + downsample + JPEG encode + write to cache.
 * Only meaningful after checkPreviewCache() returned Miss. Meant to run
 * on a worker thread, not Block's main poll-loop thread. Safe to call
 * concurrently from multiple worker threads on different images - see
 * the comment in preview.cpp on why the actual cfitsio calls are
 * serialized internally regardless.
 */
bool generatePreview (const std::string &imagesDir, const std::string &cacheDir, const std::string &relPath, int previewSize, float quantiles, std::string &jpegData, std::string &errorMsg);

}
