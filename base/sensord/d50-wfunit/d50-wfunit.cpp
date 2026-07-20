/*
 * Driver for arduino unit, located near the WF camera of the D50 telescope.
 * Contains external shutter/cover of the WF camera and two temparature/humidity
 * sensors, measuring values inside & outside of the main (NF) optical tube.
 *
 * Copyright (C) 2011 Petr Kubanek <petr@kubanek.net>
 * Copyright (C) 2014 Martin Jelinek
 * Copyright (C) 2019 Jan Strobl
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

/*
Commands: oc
o : open shutter
c : close shutter
0-9 : set power to fan (0=off, 9=max)
s : get status (also the default response)

example output of any command (including unknown):
C 0 16.96 46.57 18.64 40.13 16.75 13.50 //shutter_state fan_state t_out h_out t_in h_in temp_mirror_air temp_mirror
*/

#include "sensord.h"

#include "connserial.h"

namespace rts2sensord
{

/**
 * D50WFUnit board control various I/O.
 *
 * @author Petr Kubanek <petr@kubanek.net>, Martin Jelinek <mates@iaa.es>, Jan Strobl
 */
class D50WFUnit:public Sensor
{
	public:
		D50WFUnit (int argc, char **argv);
		virtual ~D50WFUnit ();
		virtual int scriptEnds ();

	protected:
		virtual int processOption (int opt);
		virtual int initHardware ();
		virtual int info ();

		virtual int setValue (rts2core::Value *old_value, rts2core::Value *new_value);

	private:
		const char *device_file;
		rts2core::ConnSerial *wfunitConn;

		rts2core::ValueSelection *wfShutter;
		rts2core::ValueSelection *wfFanPower;
		rts2core::ValueDoubleStat *otaTempIn;
		rts2core::ValueDoubleStat *otaHumIn;
		rts2core::ValueDoubleStat *otaTempOut;
		rts2core::ValueDoubleStat *otaHumOut;
		rts2core::ValueDoubleStat *otaTempMirrorAir;
		rts2core::ValueDoubleStat *otaTempMirror;

		rts2core::ValueInteger *numVal;

		int wfunitCommand (char c);
};

}

using namespace rts2sensord;

D50WFUnit::D50WFUnit (int argc, char **argv): Sensor (argc, argv)
{
	device_file = "/dev/wfunit"; // default value
	wfunitConn = NULL;

	createValue (numVal, "num_stat", "number of measurements for temp/hum values", false, RTS2_VALUE_WRITABLE);
	numVal->setValueInteger (6);

	createValue (otaTempIn, "OTAtempIN", "temperature inside of optical tube", false);
	createValue (otaHumIn, "OTAhumIN", "humidity [%] inside of optical tube", false);
	createValue (otaTempOut, "OTAtempOUT", "temperature outside of optical tube", false);
	createValue (otaHumOut, "OTAhumOUT", "humidity [%] outside of optical tube", false);

	createValue (otaTempMirrorAir, "OTAtempAIR", "temperature of the air in OTA, close to mirror", false);
	createValue (otaTempMirror, "OTAtempMIRROR", "temperature of primary mirror", false);

	createValue (wfShutter, "WFshutter", "WF shutter/cover state", false, RTS2_VALUE_WRITABLE);
	wfShutter->addSelVal ("CLOSED");
	wfShutter->addSelVal ("OPEN");

	createValue (wfFanPower, "OTAfanPower", "power to the fans in the OTA", false, RTS2_VALUE_WRITABLE);
	wfFanPower->addSelVal ("off");
	wfFanPower->addSelVal ("1");
	wfFanPower->addSelVal ("2");
	wfFanPower->addSelVal ("3");
	wfFanPower->addSelVal ("4");
	wfFanPower->addSelVal ("5");
	wfFanPower->addSelVal ("6");
	wfFanPower->addSelVal ("7");
	wfFanPower->addSelVal ("8");
	wfFanPower->addSelVal ("full");

	addOption ('f', NULL, 1, "serial port with the module (may be /dev/ttyUSBn, defaults to /dev/wfunit)");

	setIdleInfoInterval (10);
}

D50WFUnit::~D50WFUnit ()
{
	delete wfunitConn;
}

int D50WFUnit::processOption (int opt)
{
	switch (opt)
	{
		case 'f':
			device_file = optarg;
			break;
		default:
			return Sensor::processOption (opt);
	}
	return 0;
}

int D50WFUnit::initHardware ()
{
	int ret;

	if (device_file == NULL)
	{
		logStream (MESSAGE_ERROR) << "you must specify device file (TTY port)" << sendLog;
		return -1;
	}

	wfunitConn = new rts2core::ConnSerial (device_file, this, rts2core::BS9600, rts2core::C8, rts2core::NONE, 50);
	ret = wfunitConn->init ();
	if (ret)
		return ret;

	wfunitConn->flushPortIO ();
	wfunitConn->setDebug (getDebug ());

	// it must be twice here, from some reason the first response after inicialization is never received...
	ret = wfunitCommand ('s');
	ret = wfunitCommand ('s');

	return ret;
}

int D50WFUnit::info ()
{
	wfunitCommand ('s');
	return Sensor::info ();
}

int D50WFUnit::scriptEnds ()
{
changeValue(wfShutter, 0);
return 0;
}

int D50WFUnit::setValue (rts2core::Value *old_value, rts2core::Value *new_value)
{
	if (old_value == wfShutter)
	{
		switch (new_value->getValueInteger ())
		{
			case 0:
				wfunitCommand ('c');
				break;
			case 1:
				wfunitCommand ('o');
				break;
		}
		return 0;
	}
	else if (old_value == wfFanPower)
	{
		wfunitCommand (((char) new_value->getValueInteger()) + '0');
	}

	return Sensor::setValue (old_value, new_value);
}

int D50WFUnit::wfunitCommand (char c)
{
	char buf[200];
	// base note: classic tree declared shutState as a bare char and
	// sscanf'd it with "%s" - writing the matched token plus a NUL
	// terminator (at least 2 bytes) into a 1-byte lvalue, a stack buffer
	// overflow. See UPSTREAM_BUGS.md. Fixed by reading into a real
	// buffer and taking its first character.
	char shutStateBuf[8];
	char shutState;
	int fanState;
	float temp1, temp2, hum1, hum2, tempAir, tempMirror;

	wfunitConn->flushPortIO();
	wfunitConn->writeRead (&c, 1, buf, 199, '\n');

	// typical reply is: C 0 16.96 46.57 18.64 40.13 16.75 13.50 //shutter_state fan_state t_out h_out t_in h_in temp_mirror_air temp_mirror

	int ret = sscanf (buf, "%7s %u %f %f %f %f %f %f //*", shutStateBuf, &fanState, &temp1, &hum1, &temp2, &hum2, &tempAir, &tempMirror);
	shutState = shutStateBuf[0];
	if (ret != 8)
	{
		logStream (MESSAGE_ERROR) << "cannot parse reply from unit, reply was: '" << buf << "', return " << ret << sendLog;
		return -1;
	}

	switch (shutState)
	{
		case 'C':
			wfShutter->setValueInteger(0);
			break;
		case 'O':
			wfShutter->setValueInteger(1);
			break;
		default:
			logStream (MESSAGE_ERROR) << "unexpected shutter state in response from unit: '" << buf << "'" << sendLog;
			return -1;
	}

	// base note: classic tree had "wfFanPower->setValueInteger(shutState)"
	// here - assigning the shutter-state character ('C'=67/'O'=79) to the
	// fan-power selection value instead of the parsed fanState integer.
	// See UPSTREAM_BUGS.md.
	wfFanPower->setValueInteger(fanState);

	// check the I2C sensors credibility
	if (hum1 > 110.0)
	{
		temp1 = NAN;
		hum1 = NAN;
	}
	if (hum2 > 110.0)
	{
		temp2 = NAN;
		hum2 = NAN;
	}

	otaTempOut->addValue (temp1, numVal->getValueInteger());
	otaTempOut->calculate ();
	otaHumOut->addValue (hum1, numVal->getValueInteger());
	otaHumOut->calculate ();

	otaTempIn->addValue (temp2, numVal->getValueInteger());
	otaTempIn->calculate ();
	otaHumIn->addValue (hum2, numVal->getValueInteger());
	otaHumIn->calculate ();

	otaTempMirrorAir->addValue (tempAir, numVal->getValueInteger());
	otaTempMirrorAir->calculate ();
	otaTempMirror->addValue (tempMirror, numVal->getValueInteger());
	otaTempMirror->calculate ();

	return 0;
}

int main (int argc, char **argv)
{
	D50WFUnit device (argc, argv);
	return device.run ();
}
