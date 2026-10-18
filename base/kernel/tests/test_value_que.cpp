// Value changes that arrive while others are queued must apply in order.
//
// A queued change can rewrite another value when it is finally applied
// (Andor: ADCMODE -> setTiming () -> exposure). A later "exposure = 20"
// that happens to equal the stale current value used to be skipped as a
// no-op, and the queued change then left exposure at 0: D50 took 0 s
// frames at the start of every script repeat (UPSTREAM_BUGS.md).

#include "device.h"
#include "status.h"

#include <cassert>

class QueDevice:public rts2core::Device
{
	public:
		QueDevice (int argc, char **argv):rts2core::Device (argc, argv, DEVICE_TYPE_CCD, "C0")
		{
			createValue (mode, "ADCMODE", "mode whose change resets exposure", false, RTS2_VALUE_WRITABLE, CAM_READING);
			createValue (exposure, "exposure", "exposure", false, RTS2_VALUE_WRITABLE, CAM_READING);
			mode->setValueInteger (0);
			exposure->setValueDouble (1);
		}

		void set (rts2core::Value *v, int nv) { changeValue (v, nv); }
		void set (rts2core::Value *v, double nv) { changeValue (v, nv); }
		void reading (bool r) { maskState (CAM_MASK_READING, r ? CAM_READING : CAM_NOTREADING, "test"); }
		size_t queued () { return queValues.size (); }

		rts2core::ValueInteger *mode;
		rts2core::ValueDouble *exposure;

	protected:
		virtual int setValue (rts2core::Value *old_value, rts2core::Value *new_value)
		{
			// like Andor::setTiming (): the hardware reports its own
			// exposure, and a driver that reset it behind the value
			// writes that back here
			if (old_value == mode)
			{
				exposure->setValueDouble (0);
				return 0;
			}
			if (old_value == exposure)
				return 0;
			return rts2core::Device::setValue (old_value, new_value);
		}
};

int main ()
{
	char prog[] = "test_value_que";
	char *argv[] = { prog };
	QueDevice dev (1, argv);

	// idle: applied at once
	dev.set (dev.exposure, 20.0);
	assert (dev.exposure->getValueDouble () == 20);
	assert (dev.queued () == 0);

	// an equal set with nothing queued is still a no-op
	dev.set (dev.exposure, 20.0);
	assert (dev.queued () == 0);

	// reading out: script_ends queues the mode reset, then the next
	// script sets the exposure it already shows
	dev.reading (true);
	dev.set (dev.mode, 1);
	assert (dev.queued () == 1);
	dev.set (dev.exposure, 20.0);
	assert (dev.queued () == 2);

	// readout ends: mode first (resets exposure), then the script's value
	dev.reading (false);
	assert (dev.queued () == 0);
	assert (dev.mode->getValueInteger () == 1);
	assert (dev.exposure->getValueDouble () == 20);

	return 0;
}
