# Publishing MD Glitch (checklist)

For a new version: bump `plugin/MDGlitch/plugin.json` (and `info().version` in `src/MDGlitch.cpp`), run
`OPENMPC=... release/release.sh X.Y.Z`, test on a device, add the result to `tested.json`, tag `vX.Y.Z`,
and attach both zips to the release. The catalogue picks up new releases with no new PR.

Status 2026-11-26: 0.2.5 released (pre-release), https://github.com/lichthaus/mpc-vst-md-glitch/releases/tag/v0.2.5
(asset SHA-256 armv7 edd67139..., aarch64 3e1f51bd...). Catalogue PR
https://github.com/open-mpc/mpc-vst-plugins/pull/1219 is still waiting for review; its entry and description were updated
for 0.2.5 (summary = `release/catalog-entry.json`).

0.2.5 (2026-11-26): Gen1 CPU work (about half the CPU on the Live II; codec output bit-identical to 0.2.4).
- **Device test:** the owner played it on the MPC Live II (Gen1, 3.10.1), and the bench and NEON tests ran there
  (docs/DESIGN.md). The armv7 library in the zip is byte-identical to the tested one. The Key 37 runs 0.2.4; 0.2.5's
  aarch64 build runs the same NEON code that the Mac tests check bit for bit, but has not been run on the Key 37.
- **Packaging:** `release/release.sh 0.2.5` with the catalogue tools at c4f8b7d; `catalog_check.py --catalog`: 0 warnings
  each.

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
