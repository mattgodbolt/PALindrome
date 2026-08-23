#!/usr/bin/env python3
"""Golden-image harness for the corpus renders.

The manifest (tests/golden.txt) holds one hash per corpus clip: the exact
sha256 (first 16 hex chars) of the decoded RGB pixels of the clip's `--colour`
render. The hash only detects change - any 1-LSB difference fails, naming the
clip - and a human approves every change: `diff` renders old and new for
sign-off, `bless` records the approved state. Nothing is auto-approved.
Hashing the decoded pixels rather than the PNG byte stream means a
PNG-encoder change cannot fake (or mask) a picture change.

  golden.py check        render each clip, compare against the manifest
  golden.py bless        rewrite the manifest from the current renders
  golden.py diff [REF]   build REF (default main) in a throwaway worktree,
                         render both, write side-by-side and 8x-amplified-diff
                         PNGs to /var/tmp/palindrome/golden/ for sign-off

check and bless expect the repo's release binary (build/release/cli/palindrome)
to be built already.
"""

import argparse
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parent.parent
BINARY = REPO / "build" / "release" / "cli" / "palindrome"
MANIFEST = REPO / "tests" / "golden.txt"
DIFF_DIR = Path("/var/tmp/palindrome/golden")
CLIPS = ["alex_kidd", "alex_kidd_title", "wb3", "wb3_airspy"]
MANIFEST_HEADER = (
    "# Exact render-pixel hashes - change detection only, never approval:"
    " a human signs off via tools/golden.py diff, then bless."
)


def render(binary: Path, repo: Path, clip: str, out_png: Path) -> None:
    subprocess.run(
        [binary, "render", repo / "corpus" / clip, "--colour", "-o", out_png],
        check=True,
        cwd=repo,
        stdout=subprocess.DEVNULL,
    )


def render_hash(png: Path) -> str:
    rgb = np.asarray(Image.open(png).convert("RGB"), dtype=np.uint8)
    digest = hashlib.sha256()
    digest.update(str(rgb.shape).encode())
    digest.update(np.ascontiguousarray(rgb).tobytes())
    return digest.hexdigest()[:16]


def scratch_dir() -> tempfile.TemporaryDirectory:
    DIFF_DIR.parent.mkdir(parents=True, exist_ok=True)
    return tempfile.TemporaryDirectory(prefix="golden-", dir=DIFF_DIR.parent)


def current_hashes(binary: Path) -> dict[str, str]:
    hashes = {}
    with scratch_dir() as tmp:
        for clip in CLIPS:
            png = Path(tmp) / f"{clip}.png"
            render(binary, REPO, clip, png)
            hashes[clip] = render_hash(png)
    return hashes


def read_manifest() -> dict[str, str]:
    entries = {}
    for line in MANIFEST.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, digest = line.split()
        entries[name] = digest
    return entries


def cmd_check() -> int:
    expected = read_manifest()
    actual = current_hashes(BINARY)
    failures = []
    for clip in CLIPS:
        want = expected.get(clip)
        got = actual[clip]
        status = "ok" if got == want else f"FAIL (manifest {want or 'missing'})"
        print(f"{clip:>16} {got} {status}")
        if got != want:
            failures.append(clip)
    if failures:
        print(f"golden mismatch: {', '.join(failures)}", file=sys.stderr)
        print("If the change is intended: tools/golden.py diff, inspect,", file=sys.stderr)
        print("then tools/golden.py bless and commit tests/golden.txt.", file=sys.stderr)
        return 1
    print("all golden hashes match")
    return 0


def cmd_bless() -> int:
    actual = current_hashes(BINARY)
    lines = [MANIFEST_HEADER] + [f"{clip} {actual[clip]}" for clip in CLIPS]
    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text("\n".join(lines) + "\n")
    print(f"wrote {MANIFEST}")
    return 0


def build_ref(ref: str, worktree: Path) -> Path:
    subprocess.run(
        ["git", "worktree", "add", "--detach", worktree, ref], check=True, cwd=REPO
    )
    subprocess.run(["cmake", "--preset", "release"], check=True, cwd=worktree)
    subprocess.run(["cmake", "--build", "--preset", "release"], check=True, cwd=worktree)
    return worktree / "build" / "release" / "cli" / "palindrome"


def pad_to(img: np.ndarray, h: int, w: int) -> np.ndarray:
    out = np.full((h, w, 3), 128, dtype=img.dtype)
    out[: img.shape[0], : img.shape[1]] = img
    return out


def cmd_diff(ref: str) -> int:
    DIFF_DIR.mkdir(parents=True, exist_ok=True)
    with scratch_dir() as tmp:
        worktree = Path(tmp) / "worktree"
        try:
            ref_binary = build_ref(ref, worktree)
            for clip in CLIPS:
                ref_png = Path(tmp) / f"{clip}_ref.png"
                new_png = Path(tmp) / f"{clip}_new.png"
                render(ref_binary, worktree, clip, ref_png)
                render(BINARY, REPO, clip, new_png)
                before = np.asarray(Image.open(ref_png).convert("RGB"), dtype=np.int16)
                after = np.asarray(Image.open(new_png).convert("RGB"), dtype=np.int16)
                if before.shape != after.shape:
                    h = max(before.shape[0], after.shape[0])
                    w = max(before.shape[1], after.shape[1])
                    side = np.concatenate(
                        [pad_to(before, h, w), pad_to(after, h, w)], axis=1
                    ).astype(np.uint8)
                    Image.fromarray(side).save(DIFF_DIR / f"{clip}_side_by_side.png")
                    print(
                        f"{clip:>16} size mismatch: ref {before.shape[1]}x{before.shape[0]}"
                        f" vs current {after.shape[1]}x{after.shape[0]}"
                        " - amplified diff skipped"
                    )
                    continue
                delta = np.abs(after - before)
                side = np.concatenate([before, after], axis=1).astype(np.uint8)
                amplified = np.clip(delta * 8, 0, 255).astype(np.uint8)
                Image.fromarray(side).save(DIFF_DIR / f"{clip}_side_by_side.png")
                Image.fromarray(amplified).save(DIFF_DIR / f"{clip}_diff8x.png")
                print(f"{clip:>16} mean delta {delta.mean():.4f}  max delta {delta.max()}")
        finally:
            subprocess.run(
                ["git", "worktree", "remove", "--force", worktree],
                cwd=REPO,
                check=False,
            )
    print(f"sign-off images in {DIFF_DIR}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("check", help="compare current renders against the manifest")
    sub.add_parser("bless", help="rewrite the manifest from current renders")
    diff = sub.add_parser("diff", help="render against another ref for sign-off")
    diff.add_argument("ref", nargs="?", default="main", help="git ref to compare against")
    args = parser.parse_args()
    if args.command == "check":
        return cmd_check()
    if args.command == "bless":
        return cmd_bless()
    return cmd_diff(args.ref)


if __name__ == "__main__":
    sys.exit(main())
