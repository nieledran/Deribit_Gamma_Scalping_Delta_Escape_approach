"""Preview reproducible build/cache cleanup. Local profiles and the legacy archive are protected."""
import argparse
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    "build": [ROOT / "build/gamma", ROOT / "build/gamma-asan"],
    "unused-boost": [ROOT / ".deps/boost_1_89_0", ROOT / ".deps/boost_1_89_0.zip"],
    "qt-sources": [ROOT / ".deps/qt-sources"],
}
def checked(path):
    resolved = path.resolve()
    if not resolved.is_relative_to(ROOT) or resolved == ROOT or (path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400):
        raise RuntimeError(f"Unsafe cleanup target: {path}")
    if path.is_dir():
        for item in path.rglob("*"):
            if item.is_symlink() or getattr(item.lstat(), "st_file_attributes", 0) & 0x400:
                raise RuntimeError(f"Linked directory in cleanup target: {item}")
            name = item.name.lower()
            if item.is_file() and (".db" in name or ".sqlite" in name or
                                   name.endswith((".key", ".pem", ".pfx"))):
                raise RuntimeError(f"Protected data found; preserve it manually first: {item}")
    return resolved

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--targets", nargs="+", choices=TARGETS, default=["build", "unused-boost"])
    parser.add_argument("--apply", action="store_true", help="Delete exactly the previewed categories")
    args = parser.parse_args()
    paths = [checked(p) for key in args.targets for p in TARGETS[key] if p.exists()]
    for path in paths:
        size = sum(p.stat().st_size for p in path.rglob("*") if p.is_file()) if path.is_dir() else path.stat().st_size
        print(f"{path} ({size / 1024**2:.1f} MiB)")
    if not args.apply:
        print("Preview only. Add --apply to delete these targets. Profiles and legacy-archive are excluded.")
        return
    for path in paths:
        checked(path)
        if path.is_dir(): shutil.rmtree(path)
        else: path.unlink()
        print(f"Removed {path}")
if __name__ == "__main__":
    main()
