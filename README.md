# EXRDemux

A working v1 of a replacement for fnordware's EXtractoR for After Effects 2025 on Windows.

> ## ⚠️ Early prototype — don't put this in your production pipeline yet
>
> Use it for evaluation and feedback. Keep EXtractoR as your production tool for now.
>
> ## How it was built
>
> Entirely vibe-coded with [Claude](https://www.anthropic.com/claude) (Anthropic).

## Goals

- **Make the layer selection scriptable.** EXtractoR doesn't let us read or write that property, so we can't drive the importing and setup scripts that would be useful. EXRDemux exposes the layer selection as a normal scriptable parameter.
- **Stop the layer selection from getting stuck.** EXtractoR locks in the channel set at first import — re-render the EXR with a new pass, or remove one, and every existing instance silently drifts to the wrong channel. EXRDemux stores the layer name (as a hash) instead of the index and re-resolves on every render, so adding or removing lights doesn't require redoing your comp.
- **Own the tool.** EXR demuxing is a vital part of our workflow; having our own implementation means we can make other improvements as we see fit. (For example, this caches a bit differently than EXtractoR and is modestly faster in my limited tests.)

## Things I didn't test very much

- Any AE version other than 2025
- Mac (not compiled yet)
- Black Point / White Point / UnMult
- Anything other than barebones test comps with no other effects

Any testing helps. Feedback or wishlists welcome.

## Install

Drop `EXRDemux.aex` into your AE 2025 Plug-ins folder and restart AE. Effect appears under **Effect → EXR → EXRDemux**.

## Acknowledgements

EXRDemux exists because Brendan Bolles / fnordware built [EXtractoR](https://www.fnordware.com/ProEXR/), the standard tool for multilayer EXRs in AE for nearly twenty years. None of his code is in this binary, but EXRDemux's parameter design follows EXtractoR's conventions deliberately, and Brendan's long-running contributions to OpenEXR itself underpin the libraries we link. Thanks, Brendan.

EXRDemux statically links **OpenEXR** and **Imath** (both BSD-3-Clause). Full attribution is in [`THIRD_PARTY_LICENSES.txt`](THIRD_PARTY_LICENSES.txt) — that file ships next to the `.aex` in each release.

## License

MIT — see [`LICENSE`](LICENSE).
