/*
 * Connection to Ford boards.
 * Copyright (C) 2007-2009 Petr Kubanek <petr@kubanek.net>
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

#include "connford.h"

#include <sys/ioctl.h>
#include <termios.h>

using namespace rts2core;

#define CTENI_PORTU 0
#define ZAPIS_NA_PORT 4
#define STAV_PORTU 0

#include <iomanip>

struct typ_a
{
	unsigned char port;
	unsigned char pin;
} adresa[] =
{
	{PORT_B, 1},
	{PORT_B, 2},
	{PORT_B, 4},
	{PORT_B, 8},
	{PORT_B, 16},
	{PORT_B, 32},
	{PORT_B, 64},
	{PORT_B, 128},
	{PORT_A, 1},
	{PORT_A, 2},
	{PORT_A, 4},
	{PORT_A, 8},
	{PORT_A, 16},
	{PORT_A, 32},
	{PORT_A, 64},
	{PORT_A, 128}
};

int
FordConn::readPortState (unsigned char c_port, unsigned char &state)
{
	char cmd = STAV_PORTU | c_port;
	char echo, st;

	if (writePort (cmd))
		return -1;
	if (readPort (echo) != 1)
		return -1;
	if (readPort (st) != 1)
		return -1;
	if (echo != cmd)
	{
		logStream (MESSAGE_ERROR) << "Ford board: reply to the port " << (int) c_port << " query starts with 0x"
			<< std::hex << std::setw (2) << std::setfill ('0') << (int) (unsigned char) echo
			<< ", not with its echo, state discarded" << sendLog;
		return -1;
	}
	state = st;
	return 0;
}

int
FordConn::zjisti_stav_portu ()
{
	// base note: classic read the two reply bytes of each port blindly and never
	// flushed the line, so one stray byte (a power glitch on the board) shifted all
	// later replies by one: the command echo was taken for the port state until the
	// driver was restarted, and every pin write rewrote the whole port from it (on
	// the BART roof a close then drives the motor in the opening direction). Stale
	// input is dropped before each query, the echo is checked (the board echoes the
	// command, 30 days of BART logs), and bytes left after the reply are an error.
	// See UPSTREAM_BUGS.md, "Ford board replies are never resynchronised".
	unsigned char ca, cb;

	tcflush (sock, TCIFLUSH);

	if (readPortState (PORT_A, ca) || readPortState (PORT_B, cb))
	{
		tcflush (sock, TCIFLUSH);
		return -1;
	}

	int pending = 0;
	if (ioctl (sock, FIONREAD, &pending) == 0 && pending > 0)
	{
		logStream (MESSAGE_ERROR) << "Ford board: " << pending << " unexpected byte(s) after the port state, state discarded" << sendLog;
		tcflush (sock, TCIFLUSH);
		return -1;
	}

	if (stav_portu[PORT_A] != ca || stav_portu[PORT_B] != cb)
	{
		// base note: classic printed the state bytes sign-extended (0xffffffe0)
		logStream (MESSAGE_DEBUG) << "A 0x" << std::hex << std::setw (2) << std::setfill ('0') << (STAV_PORTU | PORT_A)
			<< " state 0x" << std::hex << std::setw (2) << std::setfill ('0') << (int) ca
			<< " B 0x" << std::hex << std::setw (2) << std::setfill ('0') << (STAV_PORTU | PORT_B)
			<< " state 0x" << std::hex << std::setw (2) << std::setfill ('0') << (int) cb << sendLog;
	}

	stav_portu[PORT_A] = ca;
	stav_portu[PORT_B] = cb;

	return 0;
}

int
FordConn::readStablePort (unsigned char c_port)
{
	// base note: a pin write rewrites the whole port from the state read just
	// before it, so a misread byte switches every relay on that port (motor,
	// direction, the BART wall sockets). Two reads in a row must agree.
	if (zjisti_stav_portu ())
		return -1;
	unsigned char first = stav_portu[c_port];
	if (zjisti_stav_portu ())
		return -1;
	if (stav_portu[c_port] != first)
	{
		logStream (MESSAGE_ERROR) << "Ford board: port " << (int) c_port << " read 0x"
			<< std::hex << std::setw (2) << std::setfill ('0') << (int) first << " and 0x"
			<< std::hex << std::setw (2) << std::setfill ('0') << (int) stav_portu[c_port]
			<< " in a row, not writing it" << sendLog;
		return -1;
	}
	return 0;
}


int
FordConn::zapni_pin (unsigned char c_port, unsigned char pin)
{
	unsigned char c;

	if (readStablePort (c_port))
		return -1;

	c = ZAPIS_NA_PORT | c_port;

	if (writePort (c))
		return -1;

	c = stav_portu[c_port] | pin;

	// base note: classic ignored this write and always returned 0, so a motor
	// command lost on the serial line looked like a success
	return writePort (c) ? -1 : 0;
}


int
FordConn::vypni_pin (unsigned char c_port, unsigned char pin)
{
	unsigned char c;

	if (readStablePort (c_port))
		return -1;

	c = ZAPIS_NA_PORT | c_port;

	if (writePort (c))
	  	return -1;

	c = stav_portu[c_port] & (~pin);

	// base note: classic ignored this write and always returned 0, so a motor
	// command lost on the serial line looked like a success
	return writePort (c) ? -1 : 0;
}


int
FordConn::ZAP (int i)
{
	return zapni_pin (adresa[i].port, adresa[i].pin);
}


int
FordConn::VYP (int i)
{
	return vypni_pin (adresa[i].port, adresa[i].pin);
}


int
FordConn::switchOffPins (int pin1, int pin2)
{
	if (adresa[pin1].port != adresa[pin2].port)
	{
		logStream (MESSAGE_ERROR) << "pin address do not match! pin1: "
			<< pin1 << " pin2: " << pin2 << sendLog;
		return -1;

	}
	return vypni_pin (adresa[pin1].port, adresa[pin1].pin | adresa [pin2].pin);
}


int
FordConn::switchOffPins (int pin1, int pin2, int pin3)
{
	if (adresa[pin1].port != adresa[pin2].port
		|| adresa[pin1].port != adresa[pin3].port)
	{
		logStream (MESSAGE_ERROR) << "pin address do not match! pin1: "
			<< pin1 << " pin2: " << pin2 << " pin3: " << pin3 << sendLog;
		return -1;

	}
	return vypni_pin (adresa[pin1].port, adresa[pin1].pin | adresa [pin2].pin | adresa[pin3].pin);
}


int
FordConn::switchOffPins (int pin1, int pin2, int pin3, int pin4)
{
	if (adresa[pin1].port != adresa[pin2].port
		|| adresa[pin1].port != adresa[pin3].port
		|| adresa[pin1].port != adresa[pin4].port)
	{
		logStream (MESSAGE_ERROR) << "pin address do not match! pin1: "
			<< pin1 << " pin2: " << pin2 << " pin3: " << pin3
			<< pin4 << " pin4: " << pin4 << sendLog;
		return -1;

	}
	return vypni_pin (adresa[pin1].port, adresa[pin1].pin | adresa [pin2].pin | adresa[pin3].pin | adresa[pin4].pin);	// base note: classic OR-ed adresa[pin4].port here
}


bool
FordConn::getPortState (int c_port)
{
	return (stav_portu[adresa[c_port].port] & adresa[c_port].pin);
}


int
FordConn::isOn (int c_port)
{
	int ret;
	ret = zjisti_stav_portu ();
	if (ret)
		return -1;
	return !(stav_portu[adresa[c_port].port] & adresa[c_port].pin);
}

FordConn::FordConn (const char *_devName, rts2core::Block * _master, bSpeedT _baudSpeed, cSizeT _cSize, parityT _parity, int _vTime):ConnSerial (_devName, _master, _baudSpeed, _cSize, _parity)
{
	stav_portu[PORT_A] = stav_portu[PORT_B] = stav_portu[PORT_C] = 0;
}


FordConn::~FordConn (void)
{
}
