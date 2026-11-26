#!/bin/sh
# Packages a release for the Open MPC catalogue from a local build, with the catalogue's own tools.
#
#   OPENMPC=/path/to/open-mpc/mpc-vst-plugins MPC_SDK=/path/to/mpc-plugin-builder-sdk release/release.sh 0.2.4
#
# Writes dist/*-mpc-armv7.zip (Gen1) and dist/*-mpc-aarch64.zip (Gen2) and checks each with
# catalog_check.py --catalog (and install_smoke.py where /proc/mounts exists, i.e. on Linux).
set -e
cd "$(dirname "$0")/.."
V="$1"; [ -n "$V" ] || { echo "usage: release/release.sh X.Y.Z"; exit 1; }
: "${OPENMPC:?set OPENMPC to a checkout of open-mpc/mpc-vst-plugins}"
grep -q "\"version\": \"$V\"" plugin/MDGlitch/plugin.json || { echo "plugin.json is not version $V"; exit 1; }
plugin/MDGlitch/make.sh
F="plugin/MDGlitch/build/Lichthaus - VST - MD Glitch"
rm -rf dist && mkdir -p dist
python3 -I "$OPENMPC/tools/sdk_release.py" "$F" --version "$V" --id md-glitch --repo lichthaus/mpc-vst-md-glitch --license MIT -o dist
for Z in dist/*.zip; do
    python3 -I "$OPENMPC/tools/catalog_check.py" "$Z" --catalog
    if [ -r /proc/mounts ]; then python3 -I "$OPENMPC/tools/install_smoke.py" "$Z"; else echo "install_smoke.py skipped: no /proc/mounts here (run it on Linux)"; fi
done
shasum -a 256 dist/*.zip
