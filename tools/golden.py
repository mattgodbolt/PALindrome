#!/usr/bin/env python3
"""Golden-image harness for the corpus renders.

The manifest (tests/golden.txt) holds one desensitised hash per corpus clip:
the clip is rendered with `--colour`, the PNG is quantised to 6 bits per
channel and 2x2 box-downsampled, and the result is sha256-hashed (first 16 hex
chars). ULP-level drift (SIMD reassociation and the like) passes untouched;
any visible change fails, naming the clip.

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
    "# Desensitised golden render hashes - maintained by tools/golden.py"
    " (bless to regenerate, diff to sign off)."
)


def render(binary: Path, clip: str, out_png: Path) -> None:
    subprocess.run(
        [binary, "render", REPO / "corpus" / clip, "--colour", "-o", out_png],
        check=True,
        cwd=REPO,
        stdout=subprocess.DEVNULL,
    )


def desensitise(png: Path) -> np.ndarray:
    """Quantise to 6 bits per channel, then 2x2 integer box-downsample."""
    rgb = np.asarray(Image.open(png).convert("RGB"), dtype=np.uint16)
    q = rgb >> 2
    h, w = (q.shape[0] // 2) * 2, (q.shape[1] // 2) * 2
    q = q[:h, :w]
    return (q[0::2, 0::2] + q[0::2, 1::2] + q[1::2, 0::2] + q[1::2, 1::2]) // 4


def desensitised_hash(png: Path) -> str:
    ds = desensitise(png)
    digest = hashlib.sha256()
    digest.update(str(ds.shape).encode())
    digest.update(np.ascontiguousarray(ds, dtype=np.uint8).tobytes())
    return digest.hexdigest()[:16]


def scratch_dir() -> tempfile.TemporaryDirectory:
    DIFF_DIR.parent.mkdir(parents=True, exist_ok=True)
    return tempfile.TemporaryDirectory(prefix="golden-", dir=DIFF_DIR.parent)


def current_hashes(binary: Path) -> dict[str, str]:
    hashes = {}
    with scratch_dir() as tmp:
        for clip in CLIPS:
            png = Path(tmp) / f"{clip}.png"
            render(binary, clip, png)
            hashes[clip] = desensitised_hash(png)
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


def cmd_diff(ref: str) -> int:
    DIFF_DIR.mkdir(parents=True, exist_ok=True)
    with scratch_dir() as tmp:
        worktree = Path(tmp) / "worktree"
        try:
            ref_binary = build_ref(ref, worktree)
            for clip in CLIPS:
                ref_png = Path(tmp) / f"{clip}_ref.png"
                new_png = Path(tmp) / f"{clip}_new.png"
                render(ref_binary, clip, ref_png)
                render(BINARY, clip, new_png)
                before = np.asarray(Image.open(ref_png).convert("RGB"), dtype=np.int16)
                after = np.asarray(Image.open(new_png).convert("RGB"), dtype=np.int16)
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
