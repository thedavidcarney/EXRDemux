#!/usr/bin/env python3
"""Write the IFTK (team tool manager) zip for this platform's Release build.

    python scripts/package_iftk.py            # Windows -> release/iftk/EXRDemux-<ver>-win.zip
    python3 scripts/package_iftk.py           # macOS   -> release/iftk/EXRDemux-<ver>-mac.zip

Zip layout (IFTK catalog format, schema 1):
    plugins/EXRDemux.aex | plugins/EXRDemux.plugin
    support/THIRD_PARTY_LICENSES.txt

The companion .jsx is deliberately left out. Mac zips use `ditto` so the
bundle keeps its symlinks and ad-hoc signature. Version comes from
EXRDEMUX_VERSION_STRING in src/exrdemux.cpp. Builds only; never publishes.
"""
import argparse, re, shutil, subprocess, sys, tempfile, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOPS = {"plugins", "scripts", "support", "extension", "addon"}


def version():
    src = (ROOT / "src" / "exrdemux.cpp").read_text(encoding="utf-8")
    m = re.search(r'#define\s+EXRDEMUX_VERSION_STRING\s+"v?([^"]+)"', src)
    if not m:
        sys.exit("EXRDEMUX_VERSION_STRING not found in src/exrdemux.cpp")
    return m.group(1)


def validate(zip_path, platform):
    # Same checks as noschmo-catalog/scripts/publish.py validate_zip.
    names = zipfile.ZipFile(zip_path).namelist()
    tops = {n.split("/")[0] for n in names if not n.startswith("__MACOSX")}
    if tops - TOPS:
        sys.exit(f"{zip_path.name}: unexpected top-level folders {sorted(tops - TOPS)}")
    want = ".plugin/" if platform == "mac" else ".aex"
    if not any(n.startswith("plugins/") and want in n.lower() for n in names):
        sys.exit(f"{zip_path.name}: no {want.strip('/')} inside plugins/")
    if any(n.startswith("scripts/") for n in names):
        sys.exit(f"{zip_path.name}: scripts/ must not ship yet")


def main():
    mac = sys.platform == "darwin"
    platform = "mac" if mac else "win"
    default = ROOT / "build" / ("mac-arm64-release/EXRDemux.plugin" if mac
                                else "win-x64-release/EXRDemux.aex")
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, default=default, help=f"default: {default}")
    ap.add_argument("--out", type=Path, default=ROOT / "release" / "iftk")
    a = ap.parse_args()

    if not a.build.exists():
        sys.exit(f"build not found: {a.build}")
    a.out.mkdir(parents=True, exist_ok=True)
    out_zip = a.out / f"EXRDemux-{version()}-{platform}.zip"
    out_zip.unlink(missing_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / "stage"
        (stage / "plugins").mkdir(parents=True)
        (stage / "support").mkdir()
        dst = stage / "plugins" / ("EXRDemux.plugin" if mac else "EXRDemux.aex")
        if mac:
            subprocess.run(["ditto", str(a.build), str(dst)], check=True)
        else:
            shutil.copy2(a.build, dst)
        shutil.copy2(ROOT / "THIRD_PARTY_LICENSES.txt", stage / "support")

        if mac:
            subprocess.run(["codesign", "-v", "--strict", "--deep", str(dst)], check=True)
            subprocess.run(["ditto", "-c", "-k", "--norsrc", str(stage), str(out_zip)],
                           check=True)
        else:
            with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED) as z:
                for p in sorted(stage.rglob("*")):
                    z.write(p, p.relative_to(stage).as_posix())

    validate(out_zip, platform)
    print(out_zip)


if __name__ == "__main__":
    main()
