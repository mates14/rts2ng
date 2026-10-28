// The enlarged image of the web pages: openLightbox (src, title) shows an
// image over the page; the wheel (two fingers or a pinch on a touchpad)
// zooms, the image follows the mouse with no button held, a click or Esc
// closes it. The same viewer as the site monitor pages
// (web/sites/*-monitor.html): zoom from the image's own size (100 %, for
// an image that fitting enlarges) or the fitted size up to 8 screen
// pixels per image pixel; the mouse at the view's edge shows the image's
// edge, with LIGHTBOX_PAN_EDGE already counting as the edge, which also
// keeps the point under the mouse where it is while zooming; from 200 %
// the pixels are drawn sharp. Builds its own markup; plain script, no
// modules.

const LIGHTBOX_MAX_PIXEL = 8;		// screen pixels per image pixel, at most
const LIGHTBOX_PAN_EDGE = 0.1;

const lightbox = (() => {
	const el = document.createElement ('div');
	el.className = 'lightbox';
	el.hidden = true;
	el.innerHTML = '<div class="lightbox-title"></div><div class="lightbox-wrap"><img alt=""></div><div class="lightbox-info"></div>';
	document.body.appendChild (el);
	return {
		el,
		title: el.querySelector ('.lightbox-title'),
		wrap: el.querySelector ('.lightbox-wrap'),
		img: el.querySelector ('img'),
		info: el.querySelector ('.lightbox-info'),
		zoom: 1,
		mouse: { x: 0.5, y: 0.5 },
		text: '',
	};
}) ();

// the image's size fitted into the view, before any zoom
function lightboxFit () {
	const img = lightbox.img;
	if (!img.naturalWidth)
		return;
	const scale = Math.min (lightbox.wrap.clientWidth / img.naturalWidth, lightbox.wrap.clientHeight / img.naturalHeight);
	img.style.width = Math.round (img.naturalWidth * scale) + 'px';
	img.style.height = Math.round (img.naturalHeight * scale) + 'px';
}

function lightboxApply () {
	const img = lightbox.img;
	const w0 = img.offsetWidth, h0 = img.offsetHeight;
	if (!img.naturalWidth || !w0) {
		lightbox.info.textContent = lightbox.text;
		return;
	}
	const maxZoom = Math.max (1, LIGHTBOX_MAX_PIXEL * img.naturalWidth / w0);
	const minZoom = Math.min (1, img.naturalWidth / w0);
	lightbox.zoom = Math.min (maxZoom, Math.max (minZoom, lightbox.zoom));
	const W = lightbox.wrap.clientWidth, H = lightbox.wrap.clientHeight;
	// the image's shift from the centre, per axis
	const shift = (size, view, mouse) => {
		if (size <= view)
			return 0;
		const u = Math.min (1, Math.max (0, (mouse - LIGHTBOX_PAN_EDGE) / (1 - 2 * LIGHTBOX_PAN_EDGE)));
		return (view - size) * u + (size - view) / 2;
	};
	const dx = shift (w0 * lightbox.zoom, W, lightbox.mouse.x);
	const dy = shift (h0 * lightbox.zoom, H, lightbox.mouse.y);
	img.style.transform = lightbox.zoom !== 1 ? `translate(${dx}px, ${dy}px) scale(${lightbox.zoom})` : '';
	img.classList.toggle ('zoomed', lightbox.zoom > 1);
	const pixel = w0 * lightbox.zoom / img.naturalWidth;
	img.classList.toggle ('pixels', pixel >= 2);
	lightbox.info.textContent = `${lightbox.text} - ${Math.round (100 * pixel)} %`;
}

function lightboxMouse (ev) {
	const rect = lightbox.wrap.getBoundingClientRect ();
	lightbox.mouse = {
		x: rect.width ? (ev.clientX - rect.left) / rect.width : 0.5,
		y: rect.height ? (ev.clientY - rect.top) / rect.height : 0.5,
	};
}

function openLightbox (src, title, info) {
	lightboxSeq++;
	lightbox.zoom = 1;
	lightbox.img.style.transform = '';
	lightbox.img.classList.remove ('zoomed', 'pixels');
	lightbox.img.removeAttribute ('src');
	lightbox.img.style.width = lightbox.img.style.height = '';
	lightbox.title.textContent = title || '';
	lightbox.text = (info ? info + ' - ' : '') + 'scroll to zoom, move the mouse to look around, click or Esc to close';
	lightbox.info.textContent = 'loading… ' + lightbox.text;
	lightbox.el.hidden = false;
	releaseBlob ();
	if (src)
		lightbox.img.src = src;
}

function closeLightbox () {
	lightbox.el.hidden = true;
	lightbox.img.removeAttribute ('src');
	releaseBlob ();
}

// an image openLightboxFetch () loaded, freed when the lightbox shows
// another or closes
let lightboxBlob = null;

function releaseBlob () {
	if (lightboxBlob)
		URL.revokeObjectURL (lightboxBlob);
	lightboxBlob = null;
}

// As openLightbox (), but loads the image itself, so that a response
// header can go to the info line: header names the header (e.g.
// X-RTS2-Calibration, what rts2-httpd's preview is).
async function openLightboxFetch (src, title, info, header) {
	openLightbox ('', title, info);
	lightbox.img.removeAttribute ('src');
	const shown = ++lightboxSeq;
	try {
		const res = await fetch (src);
		if (!res.ok)
			throw new Error (`${res.status}`);
		const extra = header ? res.headers.get (header) : null;
		const blob = await res.blob ();
		if (shown !== lightboxSeq || lightbox.el.hidden)
			return;
		if (extra)
			lightbox.text = `${info ? info + ' · ' : ''}${extra} - scroll to zoom, move the mouse to look around, click or Esc to close`;
		releaseBlob ();
		lightboxBlob = URL.createObjectURL (blob);
		lightbox.img.src = lightboxBlob;
	} catch (e) {
		if (shown === lightboxSeq)
			lightbox.info.textContent = `cannot load the image (${e.message}) - ${lightbox.text}`;
	}
}

let lightboxSeq = 0;

lightbox.img.addEventListener ('load', () => {
	lightboxFit ();
	lightboxApply ();
});

lightbox.img.addEventListener ('error', () => {
	lightbox.info.textContent = 'cannot load the image - ' + lightbox.text;
});

// never scroll the page under it, nor zoom the browser (a touchpad pinch
// comes as a wheel event with ctrlKey)
lightbox.el.addEventListener ('wheel', ev => {
	ev.preventDefault ();
	lightboxMouse (ev);
	const dy = ev.deltaY * (ev.deltaMode === 1 ? 40 : ev.deltaMode === 2 ? 800 : 1);
	lightbox.zoom *= Math.exp (-dy * (ev.ctrlKey ? 0.01 : 0.002));
	lightboxApply ();
}, { passive: false });

lightbox.el.addEventListener ('mousemove', ev => {
	if (lightbox.zoom <= 1)
		return;
	lightboxMouse (ev);
	lightboxApply ();
});

lightbox.el.addEventListener ('click', closeLightbox);

document.addEventListener ('keydown', ev => {
	if (ev.key === 'Escape' && !lightbox.el.hidden)
		closeLightbox ();
});

window.addEventListener ('resize', () => {
	if (lightbox.el.hidden)
		return;
	lightboxFit ();
	lightboxApply ();
});
