// Shared by status.html and monitor.html: rts2-mon's blocking flags and
// the helpers that draw them. Plain script, no modules: loaded before the
// page's own script.

// The blocking bits (status.h), with the texts Connection::getStateString
// gives them: "own" when the device itself blocks (rts2-mon's "|"),
// "other" when the block comes from another device, in the full BOP
// state (rts2-mon's "#").
const BOP_FLAGS = [
	{ bit: 0x01000000, own: 'BLOCK EXPOSURE', other: 'BLOCK EXPOSING' },
	{ bit: 0x02000000, own: 'BLOCK READOUT', other: 'BLOCK READOUT' },
	{ bit: 0x04000000, own: 'BLOCK TELESCOPE MOVEMENT', other: 'BLOCK TELESCOPE MOVEMENT' },
	{ bit: 0x08000000, own: 'WILL EXPOSE', other: 'WILL EXPOSE' },
	{ bit: 0x10000000, own: 'WAIT TRIG EXPOSE', other: 'WAIT TRIG EXPOSE' },
];
// device blocks of its own, with no "#" form
const OWN_FLAGS = [
	{ bit: 0x00004000, text: 'BLOCK_OPEN' },
	{ bit: 0x00002000, text: 'BLOCK_CLOSE' },
	{ bit: 0x40000000, text: 'STOP' },
];

// The blocking parts of the state string, shown as badges instead: every
// " | TEXT" or " # TEXT" above, up to the next separator or the end (so
// that " | STOP" goes but a dome's " | STOPPED" stays).
const BLOCK_TEXTS = [...new Set ([...BOP_FLAGS.flatMap (f => [f.own, f.other]), ...OWN_FLAGS.map (f => f.text)])];
const BLOCK_RE = new RegExp (` [|#] (?:${BLOCK_TEXTS.join ('|')})(?= [|#]|$)`, 'g');

function escapeHtml (s) {
	return String (s).replace (/[&<>"']/g, c => ({
		'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
	}[c]));
}

function blockBadges (dev) {
	const badges = [];
	for (const f of OWN_FLAGS)
		if (dev.state & f.bit)
			badges.push (`<span class="block-badge own">${f.text}</span>`);
	for (const f of BOP_FLAGS) {
		if (dev.state & f.bit)
			badges.push (`<span class="block-badge own">${f.own}</span>`);
		else if (dev.bop & f.bit)
			badges.push (`<span class="block-badge other">${f.other}</span>`);
	}
	return badges.join (' ');
}

// The state text without the blocking parts blockBadges () shows.
function stateText (dev) {
	return dev.statestring.replace (BLOCK_RE, '').replace (/^ \| /, '');
}

// rts2-mon's order (its default ORDER_TYPE, sortConnType in
// base/monitor/include/nmonitor.h): centrald first, then by device type,
// then by name (strcmp's order, which < on these ASCII names is).
function sortLikeMon (devices) {
	return devices.slice ().sort ((a, b) => {
		if ((a.name === 'centrald') !== (b.name === 'centrald'))
			return a.name === 'centrald' ? -1 : 1;
		if (a.type !== b.type)
			return a.type - b.type;
		return a.name < b.name ? -1 : a.name > b.name ? 1 : 0;
	});
}
