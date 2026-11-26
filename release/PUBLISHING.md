# Publishing MD Glitch (checklist)

For a new version: bump `plugin/MDGlitch/plugin.json` (and `info().version` in `src/MDGlitch.cpp`), run
`OPENMPC=... release/release.sh X.Y.Z`, test on a device, add the result to `tested.json`, tag `vX.Y.Z`,
and attach both zips to the release. The catalogue picks up new releases with no new PR.

Status 2026-11-26: steps 1-4 done. Release https://github.com/lichthaus/mpc-vst-md-glitch/releases/tag/v0.2.4
(pre-release; asset SHA-256 armv7 73e02d36..., aarch64 0d762ccb...), catalogue PR
https://github.com/open-mpc/mpc-vst-plugins/pull/1219 (waiting for review).

0.2.4 (first public release, 2026-11-26):
1. **Device test:** on the MPC Key 37 (Gen2, 3.10.1.12) and the MPC Live II (Gen1, 3.10.1), by the owner.
   The binaries in the zips are byte-identical to the tested build. On the Live II, Gens above 2 can be
   too heavy in a project; the summary says to keep Gens at 1-2 on Gen1.
2. **Public repo:** `lichthaus/mpc-vst-md-glitch`. Commits only as
   `Lichthaus <78316715+lichthaus@users.noreply.github.com>`. Tag `v0.2.4`.
3. **GitHub pre-release** `v0.2.4`, with `MD-Glitch-0.2.4-mpc-armv7.zip` and `MD-Glitch-0.2.4-mpc-aarch64.zip`
   (`catalog_check.py --catalog`: 0 warnings each; `install_smoke.py` needs Linux, not run).
4. **Catalogue PR** to open-mpc/mpc-vst-plugins from a topic branch of a fork. It adds
   `catalog/plugins/md-glitch.json` = `release/catalog-entry.json` and uses the PR template.
