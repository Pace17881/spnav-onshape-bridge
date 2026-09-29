// Small in-page sensitivity control for spnav-onshape-bridge, so the value
// can be tuned live while actually moving the 3D mouse in the open document,
// instead of restarting the daemon with a different --sensitivity guess.
//
// Deliberately not a browser-action popup: those close as soon as you click
// back into the page to test the change, which defeats the point. This is a
// small fixed-position overlay injected into the document itself, so it
// stays open while the model underneath keeps responding to the mouse.
//
// Talks only to the bridge's own /spnav/sensitivity endpoint (GET to read
// the current value, POST {"sensitivity": N} to change it) - unrelated to
// the WAMP/3dconnexion protocol on the same port. See main.c's
// handle_sensitivity_request(). Same-origin-checked there exactly like
// everything else the bridge serves, so this only works when injected into
// an allowed Onshape document, same as the platform spoof.
(function() {
	'use strict';

	var BRIDGE_ORIGIN = 'https://127.51.68.120:8181';
	var SLIDER_MIN = 0.05;
	var SLIDER_MAX = 3;
	var SLIDER_STEP = 0.01;
	var POST_THROTTLE_MS = 120;
	var POS_KEY = 'spnav-onshape-bridge-toggle-pos';
	var DRAG_THRESHOLD = 4; // px of movement before a mousedown counts as a drag, not a click
	/* Onshape's own UI claims most screen edges (toolbars, view cube,
	 * measurement/units controls bottom-right); starting here, off to the
	 * side at mid-height, is the least likely spot to land on top of
	 * something - still draggable afterwards since that's app-specific and
	 * changes over time. */
	var DEFAULT_POS = { top: '50%', left: null, right: '8px' };

	function injectStyle() {
		var style = document.createElement('style');
		style.textContent = [
			'#spnav-sens-toggle {',
			'  position: fixed; z-index: 2147483647;',
			'  width: 32px; height: 32px; border-radius: 50%;',
			'  background: #2b2b2b; color: #ddd; border: 1px solid #555;',
			'  font-size: 16px; line-height: 30px; text-align: center;',
			'  cursor: grab; user-select: none; box-shadow: 0 1px 4px rgba(0,0,0,.4);',
			'  font-family: sans-serif; opacity: .55; transition: opacity .15s;',
			'}',
			'#spnav-sens-toggle:hover, #spnav-sens-toggle.dragging { opacity: 1; }',
			'#spnav-sens-toggle.dragging { cursor: grabbing; }',
			'#spnav-sens-panel {',
			'  position: fixed; z-index: 2147483647;',
			'  background: #2b2b2b; color: #ddd; border: 1px solid #555; border-radius: 6px;',
			'  padding: 10px 14px; font: 12px sans-serif; width: 220px;',
			'  box-shadow: 0 1px 6px rgba(0,0,0,.5); display: none;',
			'}',
			'#spnav-sens-panel.open { display: block; }',
			'#spnav-sens-panel label { display: block; margin-bottom: 6px; }',
			'#spnav-sens-panel input[type=range] { width: 100%; }',
			'#spnav-sens-panel .spnav-row { display: flex; justify-content: space-between; align-items: center; }',
			'#spnav-sens-panel .spnav-status { margin-top: 6px; opacity: .7; font-size: 11px; }',
		].join('\n');
		document.head.appendChild(style);
	}

	function loadPos() {
		try {
			var raw = localStorage.getItem(POS_KEY);
			if(raw) return JSON.parse(raw);
		} catch(e) { /* private browsing, storage disabled, etc. - just use the default */ }
		return DEFAULT_POS;
	}

	function savePos(pos) {
		try {
			localStorage.setItem(POS_KEY, JSON.stringify(pos));
		} catch(e) { /* not persisted this session, harmless */ }
	}

	function applyPos(el, pos) {
		el.style.top = pos.top || '';
		el.style.left = pos.left || '';
		el.style.right = pos.right || '';
		el.style.bottom = pos.bottom || '';
		if(pos.top === '50%') el.style.transform = 'translateY(-50%)';
	}

	function makeDraggable(toggle, onMoved) {
		var dragging = false, moved = false, startX, startY, startLeft, startTop;

		toggle.addEventListener('mousedown', function(ev) {
			if(ev.button !== 0) return;
			dragging = true;
			moved = false;
			var rect = toggle.getBoundingClientRect();
			startX = ev.clientX;
			startY = ev.clientY;
			startLeft = rect.left;
			startTop = rect.top;
			ev.preventDefault();
		});

		window.addEventListener('mousemove', function(ev) {
			if(!dragging) return;
			var dx = ev.clientX - startX, dy = ev.clientY - startY;
			if(!moved && Math.abs(dx) < DRAG_THRESHOLD && Math.abs(dy) < DRAG_THRESHOLD) return;
			moved = true;
			toggle.classList.add('dragging');
			toggle.style.transform = '';
			toggle.style.right = '';
			toggle.style.bottom = '';
			toggle.style.left = Math.max(0, Math.min(window.innerWidth - 32, startLeft + dx)) + 'px';
			toggle.style.top = Math.max(0, Math.min(window.innerHeight - 32, startTop + dy)) + 'px';
		});

		window.addEventListener('mouseup', function() {
			if(!dragging) return;
			dragging = false;
			toggle.classList.remove('dragging');
			if(moved) {
				var pos = { left: toggle.style.left, top: toggle.style.top, right: null, bottom: null };
				savePos(pos);
				onMoved();
			}
		});

		return function wasClickSuppressed() {
			var wasMoved = moved;
			moved = false;
			return wasMoved;
		};
	}

	function positionPanel(toggle, panel) {
		var r = toggle.getBoundingClientRect();
		var panelW = 220, margin = 8;
		var left = r.left - panelW - margin; // prefer opening to the left of the toggle
		if(left < margin) left = Math.min(window.innerWidth - panelW - margin, r.right + margin);
		var top = Math.min(r.top, window.innerHeight - 140 - margin);
		panel.style.left = Math.max(margin, left) + 'px';
		panel.style.top = Math.max(margin, top) + 'px';
		panel.style.right = '';
		panel.style.bottom = '';
	}

	function buildUi() {
		var toggle = document.createElement('div');
		toggle.id = 'spnav-sens-toggle';
		toggle.title = 'spnav-onshape-bridge: mouse sensitivity (drag to move)';
		toggle.textContent = '⚙'; // gear
		applyPos(toggle, loadPos());

		var panel = document.createElement('div');
		panel.id = 'spnav-sens-panel';
		panel.innerHTML =
			'<div class="spnav-row"><label for="spnav-sens-range">Mouse sensitivity</label>' +
			'<span id="spnav-sens-value"></span></div>' +
			'<input type="range" id="spnav-sens-range" min="' + SLIDER_MIN + '" max="' + SLIDER_MAX +
			'" step="' + SLIDER_STEP + '">' +
			'<div class="spnav-status" id="spnav-sens-status">loading...</div>';

		document.body.appendChild(toggle);
		document.body.appendChild(panel);

		var wasClickSuppressed = makeDraggable(toggle, function() {
			if(panel.classList.contains('open')) positionPanel(toggle, panel);
		});

		toggle.addEventListener('click', function() {
			if(wasClickSuppressed()) return; // this click ended a drag, not a tap
			panel.classList.toggle('open');
			if(panel.classList.contains('open')) {
				positionPanel(toggle, panel);
				loadCurrent();
			}
		});

		var range = panel.querySelector('#spnav-sens-range');
		var valueLabel = panel.querySelector('#spnav-sens-value');
		var status = panel.querySelector('#spnav-sens-status');
		var postTimer = null;

		function setValueLabel(v) {
			valueLabel.textContent = Number(v).toFixed(2);
		}

		function loadCurrent() {
			status.textContent = 'loading...';
			fetch(BRIDGE_ORIGIN + '/spnav/sensitivity')
				.then(function(r) { return r.json(); })
				.then(function(data) {
					range.value = data.sensitivity;
					setValueLabel(data.sensitivity);
					status.textContent = 'move the 3D mouse to test';
				})
				.catch(function() {
					status.textContent = 'bridge not reachable - is it running?';
				});
		}

		function postValue(v) {
			fetch(BRIDGE_ORIGIN + '/spnav/sensitivity', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ sensitivity: v }),
			})
				.then(function(r) {
					status.textContent = r.ok ? 'saved' : 'rejected (out of range?)';
				})
				.catch(function() {
					status.textContent = 'bridge not reachable - is it running?';
				});
		}

		range.addEventListener('input', function() {
			var v = parseFloat(range.value);
			setValueLabel(v);
			status.textContent = 'move the 3D mouse to test';
			if(postTimer) clearTimeout(postTimer);
			postTimer = setTimeout(function() { postValue(v); }, POST_THROTTLE_MS);
		});
	}

	function init() {
		injectStyle();
		buildUi();
	}

	if(document.readyState === 'loading') {
		document.addEventListener('DOMContentLoaded', init);
	} else {
		init();
	}
})();
