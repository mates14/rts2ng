#!/usr/bin/env python3
# Ford dome board of the BART roof on a pseudo-terminal, for rts2-dome-bart.
#
# Protocol (dome/src/connford.cpp): a byte 0x00|port asks for the port state and
# is answered by echo + state byte; a byte with bit 2 set is followed by the new
# state of port (b & 3), no reply. The echo is what the real board sends (BART
# logs, 2026-09/10).
# Port B: 0x40 motor on, 0x80 direction closing, 0x01-0x20 wall sockets (bit set = off).
# Port A: 0x02 south closed switch, 0x04 south open switch (bit set = reached).
#
# usage: fordsim.py <link> [travel s] [flags]
#   flags: o = the open switch never makes (as at BART), c = the closed switch never makes
#   SIGUSR1: send one stray 0x00 byte now (a power glitch on the board's TX line)
#   SIGUSR2: put one extra 0x00 byte in front of the next reply
import os, sys, time, select, signal, tty

link = sys.argv[1]
travel = float (sys.argv[2]) if len (sys.argv) > 2 else 15.0
flags = sys.argv[3] if len (sys.argv) > 3 else ""

m, s = os.openpty ()
tty.setraw (m)
try:
	os.unlink (link)
except FileNotFoundError:
	pass
os.symlink (os.ttyname (s), link)

pos = 0.0	# 0 closed, 1 open
latch = {0: 0x00, 1: 0x20}	# motor off, socket 6 (heating) off
pending_write = None
stray = [0]
inreply = [0]
t0 = time.time ()

def log (msg):
	print ("%7.2f %s" % (time.time () - t0, msg), flush=True)

def on_usr1 (sig, frm):
	stray[0] += 1

def on_usr2 (sig, frm):
	inreply[0] += 1

signal.signal (signal.SIGUSR1, on_usr1)
signal.signal (signal.SIGUSR2, on_usr2)

def port_a ():
	v = latch[0] & 0x01
	if pos <= 0.0 and "c" not in flags:
		v |= 0x02
	if pos >= 1.0 and "o" not in flags:
		v |= 0x04
	return v

def motor ():
	b = latch[1]
	if not (b & 0x40):
		return "stopped"
	return "CLOSING" if b & 0x80 else "OPENING"

def end ():
	return "CLOSED" if pos <= 0.0 else ("OPEN" if pos >= 1.0 else None)

last = time.time ()
last_motor = motor ()
last_end = end ()
log ("roof closed, travel %.0f s, flags '%s'" % (travel, flags))
while True:
	r, _, _ = select.select ([m], [], [], 0.1)
	now = time.time ()
	dt = now - last
	last = now
	if latch[1] & 0x40:
		pos += (-dt if latch[1] & 0x80 else dt) / travel
		pos = min (1.0, max (0.0, pos))
	if stray[0]:
		stray[0] -= 1
		os.write (m, b"\x00")
		log ("*** stray byte 0x00")
	if r:
		for c in os.read (m, 64):
			if pending_write is not None:
				latch[pending_write] = c
				log ("write port %s = 0x%02x" % ("AB"[pending_write], c))
				pending_write = None
			elif c & 0x04:
				pending_write = c & 0x03
			else:
				st = port_a () if (c & 0x03) == 0 else latch[1]
				if inreply[0]:
					inreply[0] -= 1
					os.write (m, bytes ([0, c, st]))
					log ("*** extra byte inside the reply")
				else:
					os.write (m, bytes ([c, st]))
	mo = motor ()
	if mo != last_motor:
		log ("motor %s (roof at %.0f%%)" % (mo, pos * 100))
		last_motor = mo
	e = end ()
	if e != last_end:
		if e:
			log ("roof at %s end" % e)
		last_end = e
