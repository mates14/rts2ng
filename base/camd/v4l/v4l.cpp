/*
 * V4L2 webcam camd - cheap pointing camera / test device.
 * Copyright (C) 2026 Martin Jelinek <mates14@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

// base note: this is a first-cut skeleton, not a finished driver. It talks
// to whatever V4L2 exposes (a UVC webcam, in the expected use case) rather
// than to a real scientific sensor, so several things a real camd driver
// gets from hardware are approximated or simply unavailable here:
//
//  - No shutter. "Exposure" (seconds) is spent as wall-clock time grabbing
//    and stacking raw frames instead of a real shutter interval - but it IS
//    real wall-clock time: isExposing() is polled on a timer by the base
//    Camera class and doubles as the accumulation loop, draining whatever
//    frames the (non-blocking) V4L2 queue has ready on each call and adding
//    them to a running sum, until the requested exposure time is up (see
//    isExposing()/accumulateFrame()). doReadout() therefore does no device
//    I/O at all - it is just the final rescale of the accumulator, and
//    finishes in one call.
//  - No cooling, no real gain in physical units - setCoolTemp is left at
//    the Camera base default (reports "not supported").
//  - Only V4L2_PIX_FMT_YUYV is handled (the near-universal UVC default);
//    the Y (luma) byte of each pixel pair is used as an 8-bit grey value.
//    A real Bayer-capable UVC camera (V4L2_PIX_FMT_SBGGR8 etc.) would need
//    a separate code path and is deliberately not attempted here.
//  - Manual exposure/gain V4L2 controls are best-effort: cheap webcams
//    frequently only support AUTO and refuse to leave it, in which case
//    v4l_exposure/v4l_gain below are simply inert.
//  - The device is only open (and streaming, activity LED lit) while an
//    exposure is actually in progress - see acquireDevice()/
//    releaseDevice(), called from startExposure()/doReadout()/
//    stopExposure(). initHardware() opens it once, briefly, just to probe
//    geometry/frame rate for initCameraChip(), then closes it again.
//  - openDevice() enumerates VIDIOC_ENUM_FRAMESIZES and asks for the
//    largest-area YUYV mode the device offers, not whatever mode it
//    happens to power up in. Higher resolution usually means lower fps
//    (sometimes drastically), which is fine here precisely because
//    accumulation is wall-clock timed rather than a fixed frame count - a
//    slower device just contributes fewer, bigger frames to the same
//    exposure time.
//  - Binning (1x1/2x2/3x3/4x4) is done in software at readout time, by
//    summing the raw accumulator over each binned block instead of just
//    the temporally-stacked pixel - see doReadout().

#include "camd.h"

#include <cerrno>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

#define OPT_DEVICE  OPT_LOCAL + 1

namespace rts2camd
{

/**
 * Camera driver for V4L2 capture devices (webcams).
 *
 * Frames are captured as plain 8-bit grey (from YUYV luma) and averaged in
 * a wide accumulator into a 16-bit readout - see doReadout() for why the
 * rescale happens once, at the end, instead of per frame.
 *
 * @author Martin Jelinek <mates14@gmail.com>
 */
class V4l:public Camera
{
	public:
		V4l (int in_argc, char **in_argv);
		virtual ~V4l ();

		virtual int processOption (int in_opt);
		virtual int initHardware ();
		virtual int initChips ();
		virtual int info ();

		virtual int startExposure ();
		virtual long isExposing ();
		virtual int doReadout ();
		virtual int stopExposure ();

		virtual int setValue (rts2core::Value *old_value, rts2core::Value *new_value);

	protected:
		virtual void initDataTypes ()
		{
			// 16-bit output only - the whole point of stacking is to land
			// in a wider container than the raw 8-bit frames.
			addDataType (RTS2_DATA_USHORT);
		}

		virtual void initBinnings ()
		{
			Camera::initBinnings ();	 // adds 1x1
			addBinning2D (2, 2);
			addBinning2D (3, 3);
			addBinning2D (4, 4);
		}

	private:
		struct MappedBuffer
		{
			void *start;
			size_t length;
		};

		const char *devicePath;
		int fd;
		bool streaming;

		std::vector<MappedBuffer> buffers;

		int devWidth;
		int devHeight;
		double fps;				 // detected (or guessed) device capture rate

		rts2core::ValueInteger *stackFrames;	 // frames actually stacked into the current/last readout, reported after the fact
		rts2core::ValueDouble *v4lFps;		 // detected device frame rate, informational
		rts2core::ValueInteger *v4lExposure;	 // V4L2_CID_EXPOSURE_ABSOLUTE, best-effort
		rts2core::ValueInteger *v4lGain;	 // V4L2_CID_GAIN, best-effort

		std::vector<uint32_t> accum;		 // wide accumulator, one entry per pixel
		int framesGrabbed;

		int openDevice ();
		void closeDevice ();

		bool haveControl (unsigned int id);
		bool controlDefault (unsigned int id, int &def);
		void setControl (unsigned int id, int value);
		void disableAuto ();
		void restoreAuto ();

		int setupStreaming ();
		void teardownStreaming ();

		// open device + negotiate format/controls + start streaming, for
		// the duration of one exposure
		int acquireDevice ();
		// stop streaming + close device again
		void releaseDevice ();

		// dequeues one buffer (if "wait" is false, only if one is already
		// ready - fd is O_NONBLOCK), adds its luma into accum, requeues the
		// buffer. Returns 1 if a frame was accumulated, 0 if none was ready
		// (only possible with wait=false), negative on hard I/O error.
		int accumulateFrame (bool wait);
};

}

using namespace rts2camd;

V4l::V4l (int in_argc, char **in_argv):Camera (in_argc, in_argv)
{
	devicePath = "/dev/video0";
	fd = -1;
	streaming = false;
	devWidth = devHeight = 0;
	fps = 30.0;				 // placeholder until openDevice() detects the real rate
	framesGrabbed = 0;

	// requested "exposure" (seconds, the ordinary camd control) drives how
	// many raw frames get stacked - see startExposure(). stack_frames is
	// the resulting count, reported here for visibility, not a dial.
	createValue (stackFrames, "stack_frames", "raw 8-bit frames stacked into the current/last readout (= exposure time * device fps)", false);
	stackFrames->setValueInteger (0);

	createValue (v4lFps, "v4l_fps", "[fps] V4L2 capture rate used to convert exposure time into a frame count", false);
	v4lFps->setValueDouble (fps);

	// placeholder until initHardware() overwrites these with the values the
	// driver itself reports as its defaults for these two controls
	createValue (v4lExposure, "v4l_exposure", "V4L2_CID_EXPOSURE_ABSOLUTE, device units (no effect if the device has no manual exposure control)", true, RTS2_VALUE_WRITABLE, CAM_WORKING);
	v4lExposure->setValueInteger (0);

	createValue (v4lGain, "v4l_gain", "V4L2_CID_GAIN, device units (no effect if the device has no manual gain control)", true, RTS2_VALUE_WRITABLE, CAM_WORKING);
	v4lGain->setValueInteger (0);

	addOption (OPT_DEVICE, "device", 1, "V4L2 device node (default /dev/video0)");
}

V4l::~V4l ()
{
	teardownStreaming ();
	closeDevice ();
}

int V4l::processOption (int in_opt)
{
	switch (in_opt)
	{
		case OPT_DEVICE:
			devicePath = optarg;
			break;
		default:
			return Camera::processOption (in_opt);
	}
	return 0;
}

int V4l::openDevice ()
{
	fd = open (devicePath, O_RDWR | O_NONBLOCK);
	if (fd < 0)
	{
		logStream (MESSAGE_ERROR) << "cannot open " << devicePath << ": " << strerror (errno) << sendLog;
		return -1;
	}

	v4l2_capability cap;
	memset (&cap, 0, sizeof (cap));
	if (ioctl (fd, VIDIOC_QUERYCAP, &cap) < 0 || !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING))
	{
		logStream (MESSAGE_ERROR) << devicePath << " is not a streaming video capture device" << sendLog;
		close (fd);
		fd = -1;
		return -1;
	}

	// Enumerate the discrete YUYV frame sizes and pick the one with the
	// largest area. (Asking VIDIOC_S_FMT for an out-of-range width/height
	// and relying on the driver to clamp to its largest mode looks
	// tempting - the spec only requires drivers to clamp to *some*
	// supported mode, not the largest, and at least uvcvideo in practice
	// clamps a wildly-oversized request down to its *smallest* mode - so
	// this enumerates for real instead of gambling on that.)
	unsigned int bestWidth = 0, bestHeight = 0;
	v4l2_frmsizeenum fse;
	memset (&fse, 0, sizeof (fse));
	fse.pixel_format = V4L2_PIX_FMT_YUYV;
	while (ioctl (fd, VIDIOC_ENUM_FRAMESIZES, &fse) == 0)
	{
		unsigned int w, h;
		if (fse.type == V4L2_FRMSIZE_TYPE_DISCRETE)
		{
			w = fse.discrete.width;
			h = fse.discrete.height;
		}
		else
		{
			// stepwise/continuous - the largest mode is just its max
			w = fse.stepwise.max_width;
			h = fse.stepwise.max_height;
		}
		if ((unsigned long) w * h > (unsigned long) bestWidth * bestHeight)
		{
			bestWidth = w;
			bestHeight = h;
		}
		fse.index++;
	}

	v4l2_format fmt;
	memset (&fmt, 0, sizeof (fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl (fd, VIDIOC_G_FMT, &fmt) < 0)
	{
		logStream (MESSAGE_ERROR) << "VIDIOC_G_FMT failed on " << devicePath << ": " << strerror (errno) << sendLog;
		close (fd);
		fd = -1;
		return -1;
	}
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (bestWidth > 0)
	{
		// found at least one enumerated YUYV mode - use the largest
		fmt.fmt.pix.width = bestWidth;
		fmt.fmt.pix.height = bestHeight;
	}
	// else: VIDIOC_ENUM_FRAMESIZES isn't mandatory for every driver: some
	// (mostly older bridge-chip webcam drivers, not real UVC devices) don't
	// implement it. Fall back to whatever VIDIOC_G_FMT reported, already
	// sitting in fmt.fmt.pix.width/height.
	if (ioctl (fd, VIDIOC_S_FMT, &fmt) < 0)
	{
		logStream (MESSAGE_ERROR) << devicePath << " does not support V4L2_PIX_FMT_YUYV - MJPEG-only / Bayer webcams need a different code path (not implemented)" << sendLog;
		close (fd);
		fd = -1;
		return -1;
	}

	devWidth = fmt.fmt.pix.width;
	devHeight = fmt.fmt.pix.height;

	v4l2_streamparm parm;
	memset (&parm, 0, sizeof (parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl (fd, VIDIOC_G_PARM, &parm) == 0
		&& (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)
		&& parm.parm.capture.timeperframe.numerator > 0)
	{
		fps = (double) parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator;
	}
	else
	{
		logStream (MESSAGE_WARNING) << devicePath << " did not report a capture frame rate - assuming " << fps << " fps, exposure-time-to-frame-count will be off if that's wrong" << sendLog;
	}
	v4lFps->setValueDouble (fps);

	logStream (MESSAGE_INFO) << devicePath << ": " << devWidth << "x" << devHeight << " @ " << fps << " fps" << sendLog;

	return 0;
}

void V4l::closeDevice ()
{
	if (fd >= 0)
	{
		close (fd);
		fd = -1;
	}
}

bool V4l::haveControl (unsigned int id)
{
	v4l2_queryctrl q;
	memset (&q, 0, sizeof (q));
	q.id = id;
	return ioctl (fd, VIDIOC_QUERYCTRL, &q) == 0 && !(q.flags & V4L2_CTRL_FLAG_DISABLED);
}

bool V4l::controlDefault (unsigned int id, int &def)
{
	v4l2_queryctrl q;
	memset (&q, 0, sizeof (q));
	q.id = id;
	if (ioctl (fd, VIDIOC_QUERYCTRL, &q) < 0 || (q.flags & V4L2_CTRL_FLAG_DISABLED))
		return false;
	def = q.default_value;
	return true;
}

void V4l::setControl (unsigned int id, int value)
{
	v4l2_control ctrl;
	memset (&ctrl, 0, sizeof (ctrl));
	ctrl.id = id;
	ctrl.value = value;
	if (ioctl (fd, VIDIOC_S_CTRL, &ctrl) < 0)
		logStream (MESSAGE_WARNING) << "cannot set control 0x" << std::hex << id << std::dec << " to " << value << ": " << strerror (errno) << sendLog;
}

void V4l::disableAuto ()
{
	// best-effort: switch to manual exposure/gain so stacking averages
	// real sensor noise instead of chasing an AGC/AE loop. Cheap webcams
	// often ignore this and stay in auto - v4l_exposure/v4l_gain simply
	// won't do anything in that case.
	if (haveControl (V4L2_CID_EXPOSURE_AUTO))
		setControl (V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
	if (haveControl (V4L2_CID_AUTOGAIN))
		setControl (V4L2_CID_AUTOGAIN, 0);
}

void V4l::restoreAuto ()
{
	// V4L2 control state lives in the driver, not in our file descriptor -
	// it outlives releaseDevice()'s close() and is visible to every other
	// program using the device (cheese included). Put auto exposure/gain
	// back on the way out so we don't leave the webcam stuck on whatever
	// fixed exposure/gain happened to be set for the last stack.
	if (haveControl (V4L2_CID_EXPOSURE_AUTO))
		setControl (V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_AUTO);
	if (haveControl (V4L2_CID_AUTOGAIN))
		setControl (V4L2_CID_AUTOGAIN, 1);
}

int V4l::setupStreaming ()
{
	v4l2_requestbuffers req;
	memset (&req, 0, sizeof (req));
	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl (fd, VIDIOC_REQBUFS, &req) < 0)
	{
		logStream (MESSAGE_ERROR) << "VIDIOC_REQBUFS failed: " << strerror (errno) << sendLog;
		return -1;
	}

	buffers.resize (req.count);
	for (unsigned int i = 0; i < req.count; i++)
	{
		v4l2_buffer buf;
		memset (&buf, 0, sizeof (buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (ioctl (fd, VIDIOC_QUERYBUF, &buf) < 0)
		{
			logStream (MESSAGE_ERROR) << "VIDIOC_QUERYBUF failed: " << strerror (errno) << sendLog;
			return -1;
		}
		buffers[i].length = buf.length;
		buffers[i].start = mmap (nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
		if (buffers[i].start == MAP_FAILED)
		{
			logStream (MESSAGE_ERROR) << "mmap failed: " << strerror (errno) << sendLog;
			return -1;
		}
		if (ioctl (fd, VIDIOC_QBUF, &buf) < 0)
		{
			logStream (MESSAGE_ERROR) << "VIDIOC_QBUF failed: " << strerror (errno) << sendLog;
			return -1;
		}
	}

	v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl (fd, VIDIOC_STREAMON, &type) < 0)
	{
		logStream (MESSAGE_ERROR) << "VIDIOC_STREAMON failed: " << strerror (errno) << sendLog;
		return -1;
	}

	streaming = true;
	return 0;
}

void V4l::teardownStreaming ()
{
	if (streaming && fd >= 0)
	{
		v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		ioctl (fd, VIDIOC_STREAMOFF, &type);
		streaming = false;
	}
	for (auto &b : buffers)
	{
		if (b.start)
			munmap (b.start, b.length);
	}
	buffers.clear ();
}

int V4l::accumulateFrame (bool wait)
{
	v4l2_buffer buf;
	memset (&buf, 0, sizeof (buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;

	if (ioctl (fd, VIDIOC_DQBUF, &buf) < 0)
	{
		if (errno != EAGAIN)
		{
			logStream (MESSAGE_ERROR) << "VIDIOC_DQBUF failed: " << strerror (errno) << sendLog;
			return -1;
		}
		if (!wait)
			return 0;			 // nothing ready yet, not an error

		fd_set read_fds;
		FD_ZERO (&read_fds);
		FD_SET (fd, &read_fds);
		timeval tv;
		tv.tv_sec = 2;
		tv.tv_usec = 0;
		int sret = select (fd + 1, &read_fds, nullptr, nullptr, &tv);
		if (sret <= 0)
		{
			logStream (MESSAGE_ERROR) << "timed out waiting for a V4L2 frame" << sendLog;
			return -1;
		}
		if (ioctl (fd, VIDIOC_DQBUF, &buf) < 0)
		{
			logStream (MESSAGE_ERROR) << "VIDIOC_DQBUF failed: " << strerror (errno) << sendLog;
			return -1;
		}
	}

	// YUYV: 2 bytes per pixel (Y0 U Y1 V), luma is every even byte. V4L2
	// delivers rows top-down (row 0 = top, same as cheese's display); FITS/
	// RTS2 images are bottom-up, so flip vertically here rather than
	// leaving every viewer to disagree with what cheese shows. Accumulate
	// straight into the wide per-pixel sum - see doReadout() for why the
	// rescale to 16-bit happens once, at the very end.
	const uint8_t *yuyv = (const uint8_t *) buffers[buf.index].start;
	for (int y = 0; y < devHeight; y++)
	{
		const uint8_t *srcRow = yuyv + (size_t) (devHeight - 1 - y) * devWidth * 2;
		uint32_t *dstRow = accum.data () + (size_t) y * devWidth;
		for (int x = 0; x < devWidth; x++)
			dstRow[x] += srcRow[2 * x];
	}

	if (ioctl (fd, VIDIOC_QBUF, &buf) < 0)
	{
		logStream (MESSAGE_ERROR) << "VIDIOC_QBUF (requeue) failed: " << strerror (errno) << sendLog;
		return -1;
	}

	framesGrabbed++;
	return 1;
}

int V4l::acquireDevice ()
{
	if (openDevice () < 0)
		return -1;

	disableAuto ();

	if (haveControl (V4L2_CID_EXPOSURE_ABSOLUTE))
		setControl (V4L2_CID_EXPOSURE_ABSOLUTE, v4lExposure->getValueInteger ());
	if (haveControl (V4L2_CID_GAIN))
		setControl (V4L2_CID_GAIN, v4lGain->getValueInteger ());

	if (setupStreaming () < 0)
	{
		closeDevice ();
		return -1;
	}

	return 0;
}

void V4l::releaseDevice ()
{
	teardownStreaming ();
	if (fd >= 0)
		restoreAuto ();
	closeDevice ();
}

int V4l::initHardware ()
{
	// probe geometry/frame rate only - RTS2 needs the chip size up front,
	// but there's no need to keep the device open (and streaming) for the
	// rest of the daemon's lifetime just to know that.
	if (openDevice () < 0)
		return -1;

	// v4l_exposure/v4l_gain start at whatever the driver itself reports as
	// its default for V4L2_CID_EXPOSURE_ABSOLUTE/V4L2_CID_GAIN, not at a
	// synthetic 0 - VIDIOC_S_CTRL clamps an out-of-range value to the
	// control's min rather than rejecting it, so a hardcoded 0 silently
	// became "shortest exposure the hardware allows" on cameras whose valid
	// range doesn't include 0 (e.g. min=9, max=625 in device units).
	int def;
	if (controlDefault (V4L2_CID_EXPOSURE_ABSOLUTE, def))
		v4lExposure->setValueInteger (def);
	if (controlDefault (V4L2_CID_GAIN, def))
		v4lGain->setValueInteger (def);

	closeDevice ();

	ccdRealType->setValueCharArr ("V4L2");
	serialNumber->setValueCharArr (devicePath);

	return initChips ();
}

int V4l::initChips ()
{
	// pixel size is unknown for a generic webcam sensor - 0,0 like Dummy.
	initCameraChip (devWidth, devHeight, 0, 0);
	return Camera::initChips ();
}

int V4l::info ()
{
	return Camera::info ();
}

int V4l::startExposure ()
{
	if (acquireDevice () < 0)
		return -1;

	// "exposure" (seconds) is the standard camd control; there's no real
	// shutter behind it here, so the base Camera class's own exposureEnd
	// deadline (now + getExposure()) is what actually paces this - see
	// isExposing(), which is polled on a timer and accumulates whatever
	// frames arrive until that deadline passes. No frame count is assumed
	// up front, so a misdetected fps (or one that changed because
	// openDevice() just renegotiated resolution) can't throw off how long
	// the exposure actually runs.
	accum.assign ((size_t) devWidth * devHeight, 0);
	framesGrabbed = 0;
	stackFrames->setValueInteger (0);
	sendValueAll (stackFrames);
	return 0;
}

long V4l::isExposing ()
{
	// Drain whatever frames are already queued, without blocking the
	// daemon's event loop - fd is O_NONBLOCK, so this returns immediately
	// once VIDIOC_DQBUF hits EAGAIN.
	for (;;)
	{
		int ret = accumulateFrame (false);
		if (ret < 0)
			return -1;
		if (ret == 0)
			break;
	}

	if (getNow () < getExposureEnd ())
	{
		// re-poll roughly once per device frame period - fast enough not
		// to fall behind a 4-buffer queue, no point polling faster
		long usec = (long) (1000000.0 / (fps > 0 ? fps : 30.0));
		if (usec < 1000)
			usec = 1000;
		return usec;
	}

	// Requested exposure time is up. For a very short exposure (shorter
	// than one device frame period) the drain above may not have caught
	// anything yet - wait (briefly, blocking) for exactly one frame rather
	// than handing back an all-zero image.
	if (framesGrabbed == 0)
	{
		if (accumulateFrame (true) < 0)
			return -1;
	}

	return -2;
}

int V4l::doReadout ()
{
	// All the device I/O already happened in isExposing(); this is just
	// the final rescale of the wide accumulator, spatially summed over
	// each binned block on top of the temporal sum already in accum[] -
	// one combined "sum N raw 8-bit samples, rescale once" step instead of
	// binning and stacking separately. Always finishes in one call.
	stackFrames->setValueInteger (framesGrabbed);
	sendValueAll (stackFrames);

	int outW = getUsedWidthBinned ();
	int outH = getUsedHeightBinned ();
	int baseX = getUsedX ();
	int baseY = getUsedY ();
	int binX = binningHorizontal ();
	int binY = binningVertical ();

	uint16_t *out = (uint16_t *) getDataBuffer (0);
	if (framesGrabbed > 0)
	{
		for (int oy = 0; oy < outH; oy++)
		{
			int y0 = baseY + oy * binY;
			for (int ox = 0; ox < outW; ox++)
			{
				int x0 = baseX + ox * binX;
				uint64_t blockSum = 0;
				int count = 0;
				for (int dy = 0; dy < binY; dy++)
				{
					int sy = y0 + dy;
					if (sy < 0 || sy >= devHeight)
						continue;
					const uint32_t *row = accum.data () + (size_t) sy * devWidth;
					for (int dx = 0; dx < binX; dx++)
					{
						int sx = x0 + dx;
						if (sx < 0 || sx >= devWidth)
							continue;
						blockSum += row[sx];
						count++;
					}
				}
				// Rescale once, from the wide sum, not via an intermediate
				// 8-bit average - this is what preserves the sub-LSB
				// information that summing many noisy 8-bit samples
				// collects, instead of throwing it away by truncating to
				// an 8-bit average first and just shifting it left.
				out[(size_t) oy * outW + ox] = count ? (uint16_t) (blockSum * 65535.0 / (255.0 * framesGrabbed * count) + 0.5) : 0;
			}
		}
	}
	else
	{
		memset (out, 0, chipByteSize ());
	}

	framesGrabbed = 0;
	releaseDevice ();

	int sret = sendReadoutData ((char *) out, chipByteSize (), 0);
	if (sret < 0)
		return sret;

	return -2;				 // no more data
}

int V4l::stopExposure ()
{
	framesGrabbed = 0;
	releaseDevice ();
	return Camera::stopExposure ();
}

int V4l::setValue (rts2core::Value *old_value, rts2core::Value *new_value)
{
	if (old_value == v4lExposure)
	{
		if (fd >= 0 && haveControl (V4L2_CID_EXPOSURE_ABSOLUTE))
			setControl (V4L2_CID_EXPOSURE_ABSOLUTE, new_value->getValueInteger ());
		return 0;
	}
	else if (old_value == v4lGain)
	{
		if (fd >= 0 && haveControl (V4L2_CID_GAIN))
			setControl (V4L2_CID_GAIN, new_value->getValueInteger ());
		return 0;
	}
	return Camera::setValue (old_value, new_value);
}

int main (int argc, char **argv)
{
	V4l device (argc, argv);
	return device.run ();
}
