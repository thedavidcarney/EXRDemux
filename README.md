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

- Any AE version other than 2025
- Mac (compiles and loads in AE 2025 on Apple Silicon; layer pick + Black/White Point verified, but UnMult and re-render persistence are untested on Mac)
- Black Point / White Point / UnMult
- Anything other than barebones test comps with no other effects

Any testing helps. Feedback or wishlists welcome.

## Install

**Windows:** drop `EXRDemux.aex` into `C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\Plug-ins\` and restart AE.

**macOS:** drop `EXRDemux.plugin` into `/Applications/Adobe After Effects 2025/Plug-ins/` and restart AE. The bundle is ad-hoc signed; if Gatekeeper blocks it, run `xattr -dr com.apple.quarantine /Applications/Adobe\ After\ Effects\ 2025/Plug-ins/EXRDemux.plugin`.

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
