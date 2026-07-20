/*
 * Driver for AVE lab led source (arduino led control).
 *
 * Copyright (C) 2011 Petr Kubanek <petr@kubanek.net>
 * Copyright (C) 2014 Martin Jelinek <petr@kubanek.net>
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
No response expected
send a character with an ASCII value of the requested light intensity
*/

#include "sensord.h"

#include "connserial.h"

namespace rts2sensord
{

/**
 * Colamp board control various I/O.
 *
 * @author Petr Kubanek <petr@kubanek.net>, Martin Jelinek <mates@iaa.es>
 */
class Colamp:public Sensor
{
	public:
		Colamp (int argc, char **argv);
		virtual ~Colamp ();
		virtual int scriptEnds ();

	protected:
		virtual int processOption (int opt);
		virtual int initHardware ();
		virtual int info ();

		virtual int setValue (rts2core::Value *old_value, rts2core::Value *new_value);

	private:
		const char *device_file;
		rts2core::ConnSerial *colampConn;

		rts2core::ValueInteger *intensity;
		void colampCommand (int c);
};

}

using namespace rts2sensord;

Colamp::Colamp (int argc, char **argv): Sensor (argc, argv)
{
	device_file = "/dev/collamp"; // default value
	colampConn = NULL;

	createValue (intensity, "LAMP", "LED light intensity", true, RTS2_VALUE_WRITABLE );
	addOption ('f', NULL, 1, "serial port with the module (may be /dev/ttyUSBn, defaults to /dev/collamp)");

	setIdleInfoInterval (100);
}

Colamp::~Colamp ()
{
	delete colampConn;
}

int Colamp::processOption (int opt)
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

int Colamp::initHardware ()
{
	if (device_file == NULL)
	{
		logStream (MESSAGE_ERROR) << "you must specify device file (TTY port)" << sendLog;
		return -1;
	}

	colampConn = new rts2core::ConnSerial (device_file, this, rts2core::BS9600, rts2core::C8, rts2core::NONE, 50);
	int ret = colampConn->init ();
	if (ret)
		return ret;

	colampConn->flushPortIO ();
	colampConn->setDebug (true);

	colampCommand (0);

	return 0;
}

int Colamp::info ()
{
	return Sensor::info ();
}

int Colamp::scriptEnds ()
{
return 0;
}

int Colamp::setValue (rts2core::Value *old_value, rts2core::Value *new_value)
{
	if (old_value == intensity)
	{
		colampCommand (((rts2core::ValueInteger *) new_value)->getValueInteger () );
		return 0;
	}

	return Sensor::setValue (old_value, new_value);
}

void Colamp::colampCommand (int c)
{
	char buf[32];
	buf[0]=c;

	colampConn->writePort (buf, 1);
	colampConn->flushPortIO();
	//intensity->setValueInteger ( c );
}

int main (int argc, char **argv)
{
	Colamp device (argc, argv);
	return device.run ();
}
