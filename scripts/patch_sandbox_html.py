#!/usr/bin/env python3
"""
Post-build patch for Emscripten-generated Sandbox.html.

The Emscripten default shell ships with a "powered by emscripten" logo,
status/progress/controls DOM, and a visible canvas border. None of those
are wanted when the viewer is embedded in an iframe.

This script injects two small patches into Sandbox.html:
  1. <style>  — hides the extra UI, makes the canvas fill the iframe
                viewport with a transparent background (so the outer page
                colour shows through instead of black bars).
  2. <script> — installs two hooks on Emscripten's Module:
                * locateFile: routes .data file requests to /renderer/data/
                  so they can be served by nginx / vite middleware.
                * setStatus: forwards progress + ready events to the
                  parent window via postMessage so the React wrapper can
                  render its loading bar.

The script is idempotent — the patch has a sentinel string ("renderer-ready")
so running it a second time is a no-op.

The post-build approach (rather than a custom Emscripten shell template)
keeps the patch surgical: Emscripten's default shell already provides
Module setup, canvas binding, and progress wiring, which are preserved.

Living inside the renderer repo, the script is invoked from CMake's
POST_BUILD hook so any build path — local `cmake --build`, CI workflow,
or helper script — produces an identical patched HTML.

Usage:
    python3 patch_sandbox_html.py <path/to/Sandbox.html>
"""

import os
import sys
import time
import pathlib


# --- Patch content -----------------------------------------------------------
# Plain strings (not f-strings): CSS and JS both use `{}` liberally and
# Python f-strings require `{{ }}` for literal braces, which is a common
# footgun.

HEAD_SCRIPT_PATCH = """<script>
// DPR cap for touch devices.
// Mobile screens commonly report DPR 2.5–3, which means a 360-CSS-px
// viewport renders into a 1080-physical-px backbuffer. Splat scenes are
// fragment-bound on those phones — overdraw + tile bandwidth dominate
// frame time. Capping DPR at 2 cuts the pixel count ~2.25× on a 3-DPR
// device with no perceptible visual loss at typical viewing distance.
//
// Has to run BEFORE the emscripten contrib.glfw3 port reads
// devicePixelRatio for its canvas-size logic — head script, not body.
// ?force_dpr=N forces a specific cap (e.g. 1.5 for harsher tests).
(function () {
  try {
    var coarse = window.matchMedia && window.matchMedia('(pointer: coarse)').matches;
    var params = new URLSearchParams(window.location.search || '');
    var force  = parseFloat(params.get('force_dpr') || '');
    if (!coarse && !isFinite(force)) return;
    var capped = isFinite(force) ? force : 2.0;
    var real   = window.devicePixelRatio;
    if (capped >= real) return;
    Object.defineProperty(window, 'devicePixelRatio', {
      configurable: true,
      get: function () { return capped; }
    });
  } catch (e) {}
})();
</script>"""

STYLE_PATCH = """<style>
#emscripten_logo, .spinner, #status, #progress, #controls, #output { display: none !important; }
.emscripten_border { border: none !important; position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; display: flex; align-items: center; justify-content: center; background: transparent; }
canvas.emscripten { max-width: 100vw !important; max-height: 100vh !important; width: auto !important; height: auto !important; display: block; object-fit: contain; outline: none !important; }
body { margin: 0; overflow: hidden; background: transparent; }

/* On-screen joystick UI for touch devices.
 * Hidden by default; the patched <script> below shows the .mobile-controls
 * root only when (pointer: coarse) is true. Two stick zones in the bottom
 * corners + a vertical Q/E button column in the centre. Z-indexed above
 * the canvas so finger drags hit the joystick first. */
.mobile-controls { display: none; position: fixed; inset: 0; pointer-events: none; z-index: 10; touch-action: none; }
.mobile-controls .stick-zone { position: absolute; bottom: 5vh; width: 35vw; max-width: 200px; height: 35vw; max-height: 200px; pointer-events: auto; }
.mobile-controls .stick-zone.left  { left: 4vw; }
.mobile-controls .stick-zone.right { right: 4vw; }
.mobile-controls .stick-base { position: absolute; inset: 0; border-radius: 50%; background: rgba(255,255,255,0.10); border: 1px solid rgba(255,255,255,0.25); backdrop-filter: blur(4px); -webkit-backdrop-filter: blur(4px); }
.mobile-controls .stick-knob { position: absolute; top: 50%; left: 50%; width: 40%; height: 40%; margin: -20% 0 0 -20%; border-radius: 50%; background: rgba(255,255,255,0.4); border: 1px solid rgba(255,255,255,0.6); transform: translate(0, 0); will-change: transform; touch-action: none; }
.mobile-controls .vbtn-col { position: absolute; left: 50%; bottom: 5vh; transform: translateX(-50%); display: flex; flex-direction: column; gap: 12px; pointer-events: auto; }
.mobile-controls .vbtn { width: 56px; height: 56px; border-radius: 50%; background: rgba(255,255,255,0.10); border: 1px solid rgba(255,255,255,0.25); color: rgba(255,255,255,0.9); font-size: 22px; font-weight: 600; display: flex; align-items: center; justify-content: center; user-select: none; -webkit-user-select: none; touch-action: none; backdrop-filter: blur(4px); -webkit-backdrop-filter: blur(4px); }
.mobile-controls .vbtn.active { background: rgba(255,255,255,0.3); }
/* Visibility is gated by JS adding .enabled (it makes that decision based on
 * pointer:coarse + ?force_mobile= override). No @media wrapper here — if JS
 * is disabled, the controls stay hidden either way. */
.mobile-controls.enabled { display: block !important; }
</style>"""

# The setStatus hook posts messages whose `type` the React wrapper listens for;
# the `renderer-ready` literal also doubles as an idempotency sentinel below.
SCRIPT_PATCH = """<script>
(function () {
  // Re-route .data requests so both dev (vite middleware) and prod (nginx
  // /renderer/data/ alias) can serve the (huge) bundle however they like.
  function hookLocateFile() {
    if (typeof Module !== 'undefined') {
      var origLocate = Module.locateFile || function(p) { return p; };
      Module.locateFile = function(path, prefix) {
        if (path.endsWith('.data')) {
          // Cache-bust query param. Each build stamps a unique __BUILD_TAG__
          // into this string; browsers see a new URL on every deploy and
          // skip stale .data cached from an older build. Without this, a
          // new Sandbox.js reading offsets from an old cached .data silently
          // produces garbage (wrong shader bytes, wrong splat positions).
          return '/renderer/data/' + path + '?v=__BUILD_TAG__';
        }
        return origLocate(path, prefix);
      };
    } else {
      requestAnimationFrame(hookLocateFile);
    }
  }
  hookLocateFile();

  // Forward load progress / ready state to the parent frame. Skip if we're
  // not actually in an iframe (developer running Sandbox.html directly).
  function hookSetStatus() {
    if (typeof Module !== 'undefined' && typeof Module.setStatus === 'function') {
      var orig = Module.setStatus;
      Module.setStatus = function (text) {
        orig.call(Module, text);
        if (window.parent === window) return;
        if (text === '') {
          window.parent.postMessage({ type: 'renderer-ready' }, '*');
          Module.setStatus = orig;
        } else {
          var m = text.match(/\\((\\d+(?:\\.\\d+)?)\\/(\\d+)\\)/);
          if (m) {
            window.parent.postMessage({
              type: 'renderer-progress',
              loaded: parseFloat(m[1]),
              total: parseFloat(m[2])
            }, '*');
          }
        }
      };
    } else {
      requestAnimationFrame(hookSetStatus);
    }
  }
  hookSetStatus();
})();
</script>

<!-- On-screen joystick UI. The .mobile-controls root is hidden by CSS
     unless (pointer: coarse) matches AND we add the .enabled class below. -->
<div class="mobile-controls" id="mobileControls" aria-hidden="true">
  <div class="stick-zone left"  id="stickMove"><div class="stick-base"></div><div class="stick-knob"></div></div>
  <div class="vbtn-col">
    <div class="vbtn" id="btnUp">▲</div>
    <div class="vbtn" id="btnDown">▼</div>
  </div>
  <div class="stick-zone right" id="stickLook"><div class="stick-base"></div><div class="stick-knob"></div></div>
</div>

<script>
(function () {
  // Mobile joystick driver. Wires two virtual sticks + Q/E buttons to the
  // C++ FlyCamera via Module.ccall. No-op on desktop (pointer: fine) — the
  // CSS keeps .mobile-controls hidden, the touch handlers see no events.

  // Heuristic: only enable on touch-primary devices. We check pointer:coarse
  // (the matchMedia way). 'ontouchstart' alone is unreliable on hybrid laptops.
  // ?force_mobile=1 overrides the detection — used for local UI testing on
  // a desktop browser where matchMedia('(pointer: coarse)') always reports
  // false, even with devtools mobile emulation in some hosts.
  var params = new URLSearchParams(window.location.search || '');
  var forceMobile = params.get('force_mobile') === '1';
  var isCoarse = forceMobile ||
    (window.matchMedia && window.matchMedia('(pointer: coarse)').matches);
  if (!isCoarse) return;

  var root = document.getElementById('mobileControls');
  if (!root) return;
  root.classList.add('enabled');
  root.setAttribute('aria-hidden', 'false');

  // Per-stick state. `value` is normalised to [-1, 1] on each axis,
  // updated on touchmove and zeroed on touchend / touchcancel.
  function makeStick(zoneId) {
    var zone = document.getElementById(zoneId);
    var knob = zone.querySelector('.stick-knob');
    var state = { activeId: null, value: { x: 0, y: 0 } };
    var radius = 0;
    var center = { x: 0, y: 0 };

    function setKnob(dx, dy) {
      knob.style.transform = 'translate(' + dx + 'px, ' + dy + 'px)';
    }

    function start(t) {
      var rect = zone.getBoundingClientRect();
      radius = Math.min(rect.width, rect.height) * 0.5 * 0.8;
      center.x = rect.left + rect.width  * 0.5;
      center.y = rect.top  + rect.height * 0.5;
      state.activeId = t.identifier;
      move(t);
    }
    function move(t) {
      var dx = t.clientX - center.x;
      var dy = t.clientY - center.y;
      var len = Math.hypot(dx, dy);
      if (len > radius && len > 0) {
        dx = (dx / len) * radius;
        dy = (dy / len) * radius;
      }
      setKnob(dx, dy);
      state.value.x = radius > 0 ? dx / radius : 0;
      state.value.y = radius > 0 ? dy / radius : 0;
    }
    function end() {
      state.activeId = null;
      state.value.x = 0; state.value.y = 0;
      setKnob(0, 0);
    }

    zone.addEventListener('touchstart', function (e) {
      // Take only the first touch landing in this zone — preserves the
      // other zone's existing touch identifier.
      if (state.activeId !== null) return;
      for (var i = 0; i < e.changedTouches.length; ++i) {
        start(e.changedTouches[i]);
        break;
      }
      e.preventDefault();
    }, { passive: false });

    document.addEventListener('touchmove', function (e) {
      if (state.activeId === null) return;
      for (var i = 0; i < e.touches.length; ++i) {
        if (e.touches[i].identifier === state.activeId) {
          move(e.touches[i]);
          e.preventDefault();
          return;
        }
      }
    }, { passive: false });

    function maybeEnd(e) {
      if (state.activeId === null) return;
      // If our active touch is no longer in e.touches, the gesture ended.
      for (var i = 0; i < e.touches.length; ++i) {
        if (e.touches[i].identifier === state.activeId) return;
      }
      end();
    }
    document.addEventListener('touchend', maybeEnd);
    document.addEventListener('touchcancel', maybeEnd);

    return state;
  }

  function makeButton(btnId, onChange) {
    var btn = document.getElementById(btnId);
    var activeId = null;
    function press(t) { activeId = t.identifier; btn.classList.add('active'); onChange(true); }
    function release() { activeId = null; btn.classList.remove('active'); onChange(false); }
    btn.addEventListener('touchstart', function (e) {
      if (activeId !== null) return;
      press(e.changedTouches[0]);
      e.preventDefault();
    }, { passive: false });
    function maybeRelease(e) {
      if (activeId === null) return;
      for (var i = 0; i < e.touches.length; ++i) if (e.touches[i].identifier === activeId) return;
      release();
    }
    btn.addEventListener('touchend', maybeRelease);
    btn.addEventListener('touchcancel', maybeRelease);
  }

  var leftStick  = makeStick('stickMove');
  var rightStick = makeStick('stickLook');
  var verticalAxis = 0;
  makeButton('btnUp',   function (down) { verticalAxis = down ? +1 : (verticalAxis === +1 ? 0 : verticalAxis); });
  makeButton('btnDown', function (down) { verticalAxis = down ? -1 : (verticalAxis === -1 ? 0 : verticalAxis); });

  // Per-frame poll: writes the latest stick + button state to the C++ side
  // via Module.ccall. The look stick maps to yaw/pitch deltas (degrees per
  // tick); LOOK_SPEED was tuned by hand against the existing desktop mouse
  // sensitivity in FlyCamera.
  var LOOK_SPEED = 1.5; // degrees per (frame · stick magnitude)

  // Don't ccall before the wasm runtime has bound its function table —
  // emscripten installs assertion-shim exports that ABORT THE RUNTIME
  // when called pre-init ("call to '_vinput_set_move' via reference
  // taken before Wasm module initialization"). The abort happens
  // inside assert() before any try/catch can run.
  //
  // Module.calledRun flips true after callMain returns / unwinds. With
  // our flow that's after scene fetch + emscripten_set_main_loop's
  // simulate-infinite-loop unwind. By then every wasm export is real.
  // (The parent's loading overlay covers the iframe until splat-ready,
  // so the user can't touch the joysticks before the gate opens
  // anyway.)
  var wasmReady = false;
  function isReady() {
    if (wasmReady) return true;
    if (typeof Module !== 'undefined' && Module.calledRun === true) {
      wasmReady = true;
      return true;
    }
    return false;
  }

  function tick() {
    requestAnimationFrame(tick);
    if (!isReady()) return;
    // Left stick: x = strafe, y (screen-up) = forward. Invert y so
    // pushing the knob up moves the camera forward (away from viewer).
    Module.ccall(
      'vinput_set_move',
      null,
      ['number', 'number', 'number'],
      [leftStick.value.x, verticalAxis, -leftStick.value.y]
    );
    // Right stick: yaw / pitch deltas. Multiply by frame magnitude so
    // a held knob keeps rotating the camera, and invert pitch so down
    // on the stick = look down (matches mouse drag convention).
    var yawDelta   = rightStick.value.x * LOOK_SPEED;
    var pitchDelta = -rightStick.value.y * LOOK_SPEED;
    if (yawDelta !== 0 || pitchDelta !== 0) {
      Module.ccall(
        'vinput_apply_look',
        null,
        ['number', 'number'],
        [yawDelta, pitchDelta]
      );
    }
  }
  requestAnimationFrame(tick);
})();
</script>"""


def build_tag() -> str:
    """Stable per-build string used to cache-bust the .data URL. CI sets
    BUILD_TAG (submodule sha + workflow sha) in the env; for local builds
    we fall back to the current unix timestamp."""
    env = os.environ.get("BUILD_TAG")
    if env:
        return env
    return str(int(time.time()))


def patch(html_path: pathlib.Path) -> None:
    html = html_path.read_text(encoding="utf-8")

    # Idempotency guard — 'renderer-ready' only appears in the SCRIPT_PATCH.
    if "renderer-ready" in html:
        print(f"[patch_sandbox_html] {html_path}: already patched, skipping.")
        return

    if "</head>" not in html:
        raise SystemExit(f"{html_path}: no </head> tag — unexpected Emscripten output")
    if "</body>" not in html:
        raise SystemExit(f"{html_path}: no </body> tag — unexpected Emscripten output")

    tag = build_tag()
    script = SCRIPT_PATCH.replace("__BUILD_TAG__", tag)

    # Order matters: HEAD_SCRIPT_PATCH must execute before emscripten
    # GLFW reads window.devicePixelRatio. Both go before </head>; the
    # script is appended first so it lands before the </head> close,
    # ahead of the body's Sandbox.js include.
    html = html.replace("</head>", HEAD_SCRIPT_PATCH + "\n" + STYLE_PATCH + "\n</head>", 1)
    html = html.replace("</body>", script + "\n</body>", 1)

    html_path.write_text(html, encoding="utf-8")
    print(f"[patch_sandbox_html] {html_path}: patched OK (build tag: {tag}).")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: patch_sandbox_html.py <path/to/Sandbox.html>", file=sys.stderr)
        return 2
    patch(pathlib.Path(argv[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
