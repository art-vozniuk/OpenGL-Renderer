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
canvas.emscripten { max-width: 100vw !important; max-height: 100vh !important; width: auto !important; height: auto !important; display: block; object-fit: contain; outline: none !important; touch-action: none; }
body { margin: 0; overflow: hidden; background: transparent; }
</style>"""

# The setStatus hook posts messages whose `type` the React wrapper listens for;
# the `renderer-ready` literal also doubles as an idempotency sentinel below.
SCRIPT_PATCH = """<script>
(function () {
  // ---- Dev log sink ----------------------------------------------------
  // Fire-and-forget POSTs to /__dev_log so the agent can `tail` a single
  // file and see what the renderer actually did. Wrapped in try/catch
  // because we never want logging to break the renderer; absent endpoint
  // (prod build) silently fails.
  function devLog(tag, msg) {
    try {
      fetch('/__dev_log', {
        method: 'POST',
        headers: { 'content-type': 'text/plain' },
        body: '[' + tag + '] ' + msg,
        keepalive: true
      }).catch(function(){});
    } catch (e) {}
  }
  window.__devLog = devLog;
  devLog('shim', 'init');

  // Hook Module.print / printErr (which carry every INFO_CORE/WARN_CORE/
  // ERROR_CORE through spdlog → emscripten stdout) so all C++ logs end
  // up in the dev log alongside JS-side events.
  function hookModuleLogs() {
    if (typeof Module === 'undefined') {
      requestAnimationFrame(hookModuleLogs);
      return;
    }
    if (Module.__devLogHooked) return;
    Module.__devLogHooked = true;
    var origPrint    = Module.print    || function(){};
    var origPrintErr = Module.printErr || function(){};
    Module.print    = function(msg) { origPrint(msg);    devLog('engine', String(msg)); };
    Module.printErr = function(msg) { origPrintErr(msg); devLog('engine.err', String(msg)); };
  }
  hookModuleLogs();

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

<script>
(function () {
  // Touch + pinch driver for OrbitCamera. 1-finger drag → orbit
  // yaw/pitch; 2-finger pinch → zoom. Forwarded to C++ via Module.ccall.
  // Desktop mouse drag + scroll wheel are handled directly inside
  // OrbitCamera via Input::* (GLFW emscripten port), so we don't shim
  // them here. The same JS bridge also relays camera-mode requests from
  // the parent (orbit/fly toggle button) into vinput_request_mode.
  //
  // Native builds never load this file (this is in Sandbox.html which is
  // emscripten-only output).

  // Don't ccall before the wasm runtime has bound its function table —
  // emscripten installs assertion-shim exports that ABORT THE RUNTIME
  // when called pre-init. Module.calledRun flips true once callMain
  // returns; by then every export is real.
  var wasmReady = false;
  function isReady() {
    if (wasmReady) return true;
    if (typeof Module !== 'undefined' && Module.calledRun === true) {
      wasmReady = true;
      return true;
    }
    return false;
  }

  // Sensitivity matches the C++ default on mouse drag (0.25 deg / px).
  // Zoom delta is in log-radius units — 0.01 per pixel of pinch
  // change ≈ ~10% radius change per 10px finger movement.
  var ORBIT_DEG_PER_PIXEL = 0.25;
  var ZOOM_PER_PIXEL      = 0.01;

  // Active touches by identifier; map preserves insertion order so the
  // first two ids drive the pinch / drag distinction.
  var touches = new Map();
  var pinchPrevDist = null;

  function applyOrbit(dxPx, dyPx) {
    if (!isReady()) return;
    if (dxPx === 0 && dyPx === 0) return;
    Module.ccall(
      'vinput_apply_orbit',
      null,
      ['number', 'number'],
      [dxPx * ORBIT_DEG_PER_PIXEL, dyPx * ORBIT_DEG_PER_PIXEL]
    );
  }

  function applyZoom(dPx) {
    if (!isReady()) return;
    if (dPx === 0) return;
    // Spreading fingers (distance grows) = zoom IN = negative log-radius
    // delta. C++ side treats positive zoomDelta as zoom-out.
    Module.ccall('vinput_apply_zoom', null, ['number'], [-dPx * ZOOM_PER_PIXEL]);
  }

  function pinchDistance() {
    var arr = Array.from(touches.values());
    if (arr.length < 2) return null;
    var dx = arr[0].x - arr[1].x;
    var dy = arr[0].y - arr[1].y;
    return Math.hypot(dx, dy);
  }

  document.addEventListener('touchstart', function (e) {
    for (var i = 0; i < e.changedTouches.length; ++i) {
      var t = e.changedTouches[i];
      touches.set(t.identifier, { x: t.clientX, y: t.clientY });
    }
    pinchPrevDist = pinchDistance();
    e.preventDefault();
  }, { passive: false });

  document.addEventListener('touchmove', function (e) {
    if (touches.size === 0) return;

    if (touches.size === 1) {
      // Single-finger drag → orbit. Use the moved touch's delta.
      for (var i = 0; i < e.changedTouches.length; ++i) {
        var t = e.changedTouches[i];
        var prev = touches.get(t.identifier);
        if (!prev) continue;
        applyOrbit(t.clientX - prev.x, t.clientY - prev.y);
        touches.set(t.identifier, { x: t.clientX, y: t.clientY });
      }
    } else {
      // Two+ fingers → pinch zoom. Track centroid distance change.
      for (var j = 0; j < e.changedTouches.length; ++j) {
        var tt = e.changedTouches[j];
        if (touches.has(tt.identifier)) {
          touches.set(tt.identifier, { x: tt.clientX, y: tt.clientY });
        }
      }
      var d = pinchDistance();
      if (d !== null && pinchPrevDist !== null) {
        applyZoom(d - pinchPrevDist);
      }
      pinchPrevDist = d;
    }
    e.preventDefault();
  }, { passive: false });

  function dropTouches(e) {
    for (var i = 0; i < e.changedTouches.length; ++i) {
      touches.delete(e.changedTouches[i].identifier);
    }
    // Recompute pinch baseline so the transition pinch→drag (one finger
    // lifts) doesn't apply a huge spurious orbit delta on the next move.
    pinchPrevDist = pinchDistance();
  }
  document.addEventListener('touchend',    dropTouches);
  document.addEventListener('touchcancel', dropTouches);

  // Camera-mode request from the parent (orbit/fly toggle in React).
  // {type:'set-camera-mode', mode: 0|1} → C++ scene polls and switches.
  // Mode changes (including the C++ auto-switch on WASDEQ) are echoed
  // back to the parent from scene code via 'camera-mode-changed'.
  //
  // Editor scene also accepts two content-management messages:
  //   {type:'editor-load-splat', bytes: ArrayBuffer}
  //     — copies bytes onto the wasm heap and calls editor_load_splat_bytes.
  //       The scene posts 'editor-splat-loaded' or 'editor-error' back.
  //   {type:'editor-clear-scene'}
  //     — drops loaded content.
  window.addEventListener('message', function (e) {
    // Catch-all logging — fire BEFORE any filtering so we see absolutely
    // every postMessage that lands on the iframe window, including
    // accidental third-party messages or malformed payloads.
    try {
      var raw = e && e.data;
      var t = raw && typeof raw === 'object' ? raw.type : null;
      devLog('shim.recv',
        'type=' + String(t) +
        (raw && raw.id !== undefined ? ' id=' + raw.id : '') +
        (raw && raw.bytes ? ' bytes=' + (raw.bytes.byteLength || 0) : '') +
        ' origin=' + (e && e.origin));
    } catch (_) {}
    var data = e && e.data;
    if (!data) return;

    if (data.type === 'set-camera-mode') {
      var mode = data.mode;
      if (mode !== 0 && mode !== 1) return;
      if (!isReady()) return;
      Module.ccall('vinput_request_mode', null, ['number'], [mode]);
      return;
    }

    if (data.type === 'editor-load-splat') {
      if (!isReady()) {
        setTimeout(function () {
          window.dispatchEvent(new MessageEvent('message', { data: data }));
        }, 50);
        return;
      }
      var bytes = data.bytes;
      if (!(bytes instanceof ArrayBuffer)) {
        window.parent.postMessage({
          type: 'editor-error',
          message: 'editor-load-splat: bytes is not an ArrayBuffer'
        }, '*');
        return;
      }
      var view = new Uint8Array(bytes);
      var ptr  = Module._malloc(view.length);
      if (!ptr) {
        window.parent.postMessage({
          type: 'editor-error',
          message: 'editor-load-splat: malloc failed (' + view.length + ' bytes)'
        }, '*');
        return;
      }
      // Name string also marshalled — Module.ccall handles char* via the
      // 'string' arg type.
      var name = (typeof data.name === 'string') ? data.name : '';
      try {
        Module.HEAPU8.set(view, ptr);
        Module.ccall('editor_load_splat_bytes', null,
                     ['number', 'number', 'string'],
                     [ptr, view.length, name]);
      } finally {
        Module._free(ptr);
      }
      return;
    }

    if (data.type === 'editor-clear-scene') {
      if (!isReady()) return;
      Module.ccall('editor_clear_scene', null, [], []);
      return;
    }

    if (data.type === 'editor-select-object') {
      if (!isReady()) return;
      Module.ccall('editor_select_object', null, ['number'], [data.id || 0]);
      return;
    }

    if (data.type === 'editor-delete-object') {
      if (!isReady()) return;
      Module.ccall('editor_delete_object', null, ['number'], [data.id || 0]);
      return;
    }

    if (data.type === 'editor-focus-object') {
      if (!isReady()) return;
      Module.ccall('editor_focus_object', null, ['number'], [data.id || 0]);
      return;
    }

    if (data.type === 'editor-rename-object') {
      if (!isReady()) return;
      Module.ccall('editor_rename_object', null, ['number', 'string'],
                   [data.id || 0, String(data.name || '')]);
      return;
    }

    if (data.type === 'editor-set-visibility') {
      if (!isReady()) return;
      Module.ccall('editor_set_visibility', null, ['number', 'number'],
                   [data.id || 0, data.visible ? 1 : 0]);
      return;
    }

    if (data.type === 'editor-set-transform') {
      if (!isReady()) return;
      var p = data.position || [0,0,0];
      var r = data.rotationDeg || [0,0,0];
      var s = data.scale || [1,1,1];
      Module.ccall('editor_set_transform', null,
                   ['number','number','number','number','number','number','number','number','number','number'],
                   [data.id || 0, p[0], p[1], p[2], r[0], r[1], r[2], s[0], s[1], s[2]]);
      return;
    }

    if (data.type === 'editor-set-snap') {
      if (!isReady()) return;
      Module.ccall('editor_set_snap', null, ['number'], [data.on ? 1 : 0]);
      return;
    }
  });
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
