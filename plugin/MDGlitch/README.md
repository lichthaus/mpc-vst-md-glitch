# MD Glitch: SDK project

The MPC Plugin Builder SDK project for MD Glitch. The plugin, its pages and controls are described in the
[top-level README](../../README.md), and the DSP in [docs/DESIGN.md](../../docs/DESIGN.md). The DSP itself
is in `../../core` (header-only C++17).

- `src/MDGlitch.cpp` holds the plugin: its parameters (ids and order are permanent), programs, display
  outputs and the Q-Link slot parameters. The slots aren't used while the ASSIGN page is hidden.
- `make_skin.py` draws the PANEL and EDIT pages into `skins/`, which git ignores. The ASSIGN page code is
  kept, switched off with `SHOW_ASSIGN = False`: on MPC OS 3.10.1.12 the MPC drew neither its list overlay
  nor the picker grid's captions.
- `make.sh` runs the SDK build, the skin script, the SDK build again, and the SDK's `host_sim` checks.

```sh
MPC_SDK=/path/to/mpc-plugin-builder-sdk ./make.sh
```

The output is `build/Lichthaus - VST - MD Glitch/`.
