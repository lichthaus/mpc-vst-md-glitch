#!/bin/sh
# Builds the plugin end to end on the Mac: SDK build (writes build/params.json), the PANEL skin
# (make_skin.py, into the git-ignored skins/), SDK build again with the skin, then host_sim.
set -e
cd "$(dirname "$0")"
SDK="${MPC_SDK:-$HOME/dev/mpc-plugin-builder-sdk}"
python3 "$SDK/tools/build.py" "$PWD" >/dev/null
uv run -q --with pillow python3 make_skin.py
python3 "$SDK/tools/build.py" "$PWD" | grep -E " errors|error:|warning:" || true
"$SDK/.sdk-tools/host_sim" build/native/md-glitch.dylib test --seconds 6 --wav build/test.wav | grep -E "FAIL|WARN|Result|timing|chunk is" || true
