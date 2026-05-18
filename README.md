# EXRDemux

A working v1 of a replacement for fnordware's EXtractoR for After Effects on Windows.

> ## ⚠️ Early prototype — don't put this in your production pipeline yet
>
> Use it for evaluation and feedback. Keep EXtractoR as your production tool for now.
>
> ## How it was built
>
> Entirely vibe-coded with [Claude](https://www.anthropic.com/claude) (Anthropic).

## Goals

- **Layer selection is scriptable.** EXRDemux exposes the layer selection as a normal scriptable property — read or write it from JSX/ExtendScript to drive automated importer and setup scripts.
- **Layer selection survives re-renders.** EXRDemux stores the layer name (as a hash) instead of an index and re-resolves it on every render. Add a new pass in Blender, remove one, swap orderings — every instance keeps pointing at the layer you originally picked, no need to redo your comp.
- **Own the tool.** EXR demuxing is a vital part of our workflow; having our own implementation means we can make other improvements as we see fit. (In my limited tests it's also modestly faster.)

## Things I didn't test very much

- AE versions other than 2025 and 2026. **v0.9.0 is the 1.0 release candidate aimed at AE 2026** — it builds and installs on Win x64 + macOS arm64 for AE 2026, but broad real-comp testing there is exactly what this RC is for. AE 2025 has the most mileage so far.
- Mac (builds and loads on Apple Silicon for AE 2025/2026; layer pick + Black/White Point verified on AE 2025, but UnMult, re-render persistence, and JSX scriptability are untested on Mac)
- Black Point / White Point / UnMult
- Anything other than barebones test comps with no other effects

Any testing helps — AE 2026 feedback especially. Feedback or wishlists welcome.

## Install

**Windows:** drop `EXRDemux.aex` into `C:\Program Files\Adobe\Adobe After Effects <version>\Support Files\Plug-ins\` (e.g. `Adobe After Effects 2026`) and restart AE.

**macOS:** drop `EXRDemux.plugin` into `/Applications/Adobe After Effects <version>/Plug-ins/` (e.g. `Adobe After Effects 2026`) and restart AE. The bundle is ad-hoc signed; if Gatekeeper blocks it, run `xattr -dr com.apple.quarantine "/Applications/Adobe After Effects <version>/Plug-ins/EXRDemux.plugin"`.

Effect appears under **Effect → EXR → EXRDemux**.

## Scripts

### `scripts/SplitAndSortPassesToPrecomps.jsx`

Bulk setup for multilayer EXRs. Takes a selected EXR footage item and
builds:

- One precomp per layer, with EXRDemux applied and the correct layer
  pre-selected by name hash (so the selection survives re-renders).
- A Master Comp that stacks every precomp with Lighten blending and
  label colors grouped by shared name prefix. `Image` / `Alpha` /
  `World` / `Ambient` / `HDRI` passes are pinned at the bottom and
  disabled by default. Cryptomattes are skipped from the Master Comp
  entirely (the precomps still exist).

Run from After Effects: **File → Scripts → Run Script File…** and pick
the `.jsx` from the `scripts/` folder of this repo.

A startup dialog asks how to order the regular-light block in the
Master Comp. Today only **Layers In Order** is enabled. The spatial
modes (Auto Left to Right, Auto Top to Bottom, Front to Back, Random)
are scaffolded for a planned companion tool that emits a JSON sidecar
of per-layer luminance centroids.

## Acknowledgements

EXRDemux exists because Brendan Bolles / fnordware built [EXtractoR](https://www.fnordware.com/ProEXR/), the standard tool for multilayer EXRs in AE for nearly twenty years. None of his code is in this binary, but EXRDemux's parameter design follows EXtractoR's conventions deliberately, and Brendan's long-running contributions to OpenEXR itself underpin the libraries we link. Thanks, Brendan.

EXRDemux statically links **OpenEXR** and **Imath** (both BSD-3-Clause). Full attribution is in [`THIRD_PARTY_LICENSES.txt`](THIRD_PARTY_LICENSES.txt).

## License

MIT — see [`LICENSE`](LICENSE).
