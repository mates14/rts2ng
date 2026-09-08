// getChannelHistogram()/getChannelQuantiles() over both the fixed 16-bit bins
// and the data-driven ones the wider types need.
//
// The 32-bit case is what an Andor ACCUMULATE frame looks like: ACCNUM scans
// summed, so every pixel carries ACCNUM times the bias and the sky and the
// whole frame sits far above zero, in a band far narrower than the type it is
// stored in.  Binned over 0..65535 or over the whole of int32 that collapses
// into a single spike and the preview comes out flat; the quantiles have to
// come back inside the band the data actually occupies.

#include "image.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fitsio.h>
#include <unistd.h>

#define WIDTH	64
#define HEIGHT	64
#define NPIX	(WIDTH * HEIGHT)

// A frame with a flat pedestal over most of it and a bright patch in one
// corner - enough structure for a quantile to have something to find.
template < typename dt > static void fillFrame (dt * data, dt pedestal, dt bright)
{
	for (int i = 0; i < NPIX; i++)
	{
		int y = i / WIDTH;
		int x = i - y * WIDTH;
		data[i] = (x < 8 && y < 8) ? bright : pedestal;
	}
}

static void writeFits (const char *path, int bitpix, int datatype, void *data)
{
	fitsfile *fptr = NULL;
	int status = 0;
	long naxes[2] = { WIDTH, HEIGHT };

	unlink (path);
	fits_create_file (&fptr, path, &status);
	assert (status == 0);
	fits_create_img (fptr, bitpix, 2, naxes, &status);
	assert (status == 0);
	fits_write_img (fptr, datatype, 1, NPIX, data, &status);
	assert (status == 0);
	fits_close_file (fptr, &status);
	assert (status == 0);
}

int main ()
{
	char dir[] = "/tmp/base_test_image_histogram_XXXXXX";
	assert (mkdtemp (dir) != NULL);

	char path16[256];
	char path32[256];
	snprintf (path16, sizeof (path16), "%s/u16.fits", dir);
	snprintf (path32, sizeof (path32), "%s/i32.fits", dir);

	// --- 16 bits: one bin per ADU, unchanged by the wide-type work ---------
	{
		uint16_t data[NPIX];
		fillFrame < uint16_t > (data, 1000, 20000);
		writeFits (path16, USHORT_IMG, TUSHORT, data);

		rts2image::Image img;
		img.openFile (path16, true, false);
		// the data type is read off the file by loadChannels(), not by
		// openFile() - until then getDataType() still says USHORT
		img.loadChannels ();
		assert (img.getDataType () == RTS2_DATA_USHORT);

		long hist[65536];
		long npix = 0;
		double binOffset = -1;
		double binScale = -1;
		img.getChannelHistogram (0, hist, 65536, &npix, &binOffset, &binScale);

		assert (npix == NPIX);
		// the fixed range still maps one ADU to one bin, starting at zero
		assert (binOffset == 0);
		assert (binScale == 1);
		assert (hist[1000] == NPIX - 64);
		assert (hist[20000] == 64);

		// 64 of 4096 pixels are bright, so a 5% clip puts both ends on the
		// pedestal; the point is that they land on the data, not on 0/65535
		int low = 0;
		int high = 0;
		img.getChannelQuantiles < int > (0, 0, 65535, 0.005, &low, &high);
		assert (low == 1000);
		assert (high == 1000);
	}

	// --- 32 bits: an accumulated frame, nowhere near the type's range ------
	{
		// 250 x 0.1s, pedestal ~ 250 * (300 bias + 700 sky), a star at 3x that
		const int32_t pedestal = 250000;
		const int32_t bright = 750000;

		int32_t data[NPIX];
		fillFrame < int32_t > (data, pedestal, bright);
		writeFits (path32, LONG_IMG, TINT, data);

		rts2image::Image img;
		img.openFile (path32, true, false);
		// the data type is read off the file by loadChannels(), not by
		// openFile() - until then getDataType() still says USHORT
		img.loadChannels ();
		assert (img.getDataType () == RTS2_DATA_LONG);

		long hist[65536];
		long npix = 0;
		double binOffset = -1;
		double binScale = -1;
		img.getChannelHistogram (0, hist, 65536, &npix, &binOffset, &binScale);

		assert (npix == NPIX);

		// binned over the range the data occupies, not over 0..65535 and not
		// over int32 - both of which would have put every pixel in one bin
		assert (binOffset == pedestal);
		assert (binScale >= 1);
		assert (binScale <= (double) (bright - pedestal + 65536) / 65536);

		// every pixel is accounted for, and in exactly two bins
		long counted = 0;
		int occupied = 0;
		for (long i = 0; i < 65536; i++)
		{
			counted += hist[i];
			if (hist[i])
				occupied++;
		}
		assert (counted == NPIX);
		assert (occupied == 2);

		// the pedestal is bin 0, the star is the last occupied bin, and both
		// map back to the values they were written with
		assert (hist[0] == NPIX - 64);
		long topBin = (long) ((bright - pedestal) / (int64_t) binScale);
		assert (topBin > 0 && topBin < 65536);
		assert (hist[topBin] == 64);
		assert ((int32_t) (binOffset + topBin * binScale) <= bright);

		// and the quantiles come back inside the band, which is the whole
		// point: INT_MIN/INT_MAX here means a uniform grey preview
		int low = 0;
		int high = 0;
		img.getChannelQuantiles < int > (0, INT_MIN, INT_MAX, 0.005, &low, &high);
		assert (low >= pedestal);
		assert (low <= bright);
		assert (high >= low);
		assert (high <= bright);

		// and the end of the line the web preview actually walks: a stretch
		// over INT_MIN..INT_MAX renders every pixel the same shade, which is
		// how this failed before - a blank-looking frame that was not blank
		unsigned char *gray = NULL;
		img.getChannelGrayscaleImage (img.getDataType (), 0, gray, 0.005, 0);
		assert (gray != NULL);

		int shades = 0;
		bool seen[256];
		memset (seen, 0, sizeof (seen));
		for (int i = 0; i < NPIX; i++)
		{
			if (!seen[gray[i]])
			{
				seen[gray[i]] = true;
				shades++;
			}
		}
		assert (shades > 1);

		delete[] gray;
	}

	unlink (path16);
	unlink (path32);
	rmdir (dir);

	return 0;
}
