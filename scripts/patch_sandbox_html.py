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

STYLE_PATCH = """<style>
#emscripten_logo, .spinner, #status, #progress, #controls, #output { display: none !important; }
.emscripten_border { border: none !important; position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; display: flex; align-items: center; justify-content: center; background: transparent; }
canvas.emscripten { max-width: 100vw !important; max-height: 100vh !important; width: auto !important; height: auto !important; display: block; object-fit: contain; outline: none !important; }
body { margin: 0; overflow: hidden; background: transparent; }
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

    html = html.replace("</head>", STYLE_PATCH + "\n</head>", 1)
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
