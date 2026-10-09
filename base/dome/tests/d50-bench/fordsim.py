#!/usr/bin/env python3
"""Simulated Ford dome board (D50 roof) on a pseudo-terminal.

Protocol as FordConn uses it: a byte with bit 2 clear asks for the state of
port (b & 3), answered by echo + state byte; a byte with bit 2 set is followed
by the new value of port (b & 3), no answer.

Port B bit 0x01 = opening motor, 0x02 = closing motor.
Port A bit 0x02 = open end switch, 0x04 = closed end switch.

Travel time end to end: FORDSIM_TRAVEL seconds (default 6).
Control: write "stuck" / "ok" into <dir>/fordsim.ctl to make the roof stop
half-way (never reaching an end switch) or move normally.
"""
import os, pty, sys, time, select, tty

TRAVEL = float (os.environ.get ('FORDSIM_TRAVEL', 6.0))	# seconds end to end
d = sys.argv[1]
master, slave = pty.openpty()
tty.setraw(slave)
os.symlink(os.ttyname(slave), os.path.join(d, 'ford.tty'))
log = open(os.path.join(d, 'fordsim.log'), 'a', buffering=1)

port = [0x04, 0x00, 0x00]	# roof closed, motors off
pos = 0.0			# 0 closed .. 1 open
last = time.time()
pending = None			# port waiting for its value byte

def L (msg):
	log.write('%.3f %s\n' % (time.time(), msg))

def stuck ():
	try:
		return open(os.path.join(d, 'fordsim.ctl')).read().strip() == 'stuck'
	except OSError:
		return False

L('started, tty %s' % os.ttyname(slave))
while True:
	r, _, _ = select.select([master], [], [], 0.1)
	now = time.time()
	dt, last = now - last, now
	opening, closing = port[1] & 0x01, port[1] & 0x02
	limit_hi, limit_lo = (0.5, 0.5) if stuck() else (1.0, 0.0)
	if opening and not closing:
		pos = min(limit_hi, pos + dt / TRAVEL)
	elif closing and not opening:
		pos = max(limit_lo, pos - dt / TRAVEL)
	a = 0
	if pos >= 1.0: a |= 0x02
	if pos <= 0.0: a |= 0x04
	if a != port[0]:
		L('switches A=0x%02x pos=%.2f' % (a, pos))
	port[0] = a
	if not r:
		continue
	for b in os.read(master, 64):
		if pending is not None:
			if pending == 1 and b != port[1]:
				L('motors B=0x%02x (open=%d close=%d)' % (b, b & 1, (b >> 1) & 1))
			port[pending] = b
			pending = None
		elif b & 0x04:
			pending = b & 0x03
		else:
			os.write(master, bytes([b, port[b & 0x03]]))
