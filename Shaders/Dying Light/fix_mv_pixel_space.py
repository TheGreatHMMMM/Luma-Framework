#!/usr/bin/env python3
"""
fix_mv_pixel_space.py — Convert Dying Light motion vector vertex shaders from UV space to pixel space.

Dying Light VS motion vector shaders encode the NDC delta as UV-space output:
    output.xy = float2(0.5,-0.5) * r?.xy;

This script rewrites that line to pixel space so DLSS/DLAA receives correct input:
    output.xy = LumaData.GameData.RenderResolution.xy * float2(0.5,-0.5) * r?.xy;

It also prepends #include "Includes/Common.hlsl" so LumaData is available.

Files already containing LumaData.GameData.RenderResolution are skipped (already converted).
Files whose hash already exists in the output directory under any name are also skipped.

Usage:
    python fix_mv_pixel_space.py [--dump DUMP_DIR] [--out OUT_DIR] [--dry-run]
"""

import argparse
import pathlib
import re
import sys

_HERE = pathlib.Path(__file__).parent

DUMP_DIR_DEFAULT = _HERE / "Dump"
OUT_DIR_DEFAULT  = _HERE

INCLUDE_LINE  = '#include "Includes/Common.hlsl"'
LUMA_MARKER   = "LumaData.GameData.RenderResolution"
LUMA_COMMENT  = "// ---- Modified by Luma: multiply render resolution into MV output (UV space -> pixel space)"

# Matches: (o0-o9 or p1).xy = float2(0.5,-0.5) * r?.xy;
# Captures everything before/after so we can insert the resolution multiply.
MV_PAT = re.compile(
    r'([ \t]*(?:o\d|p\d)\.xy\s*=\s*)float2\(0\.5,-0\.5\)(\s*\*\s*r\d\.xy\s*;)'
)
LUMA_REPLACEMENT = r'\1LumaData.GameData.RenderResolution.xy * float2(0.5,-0.5)\2'

MIGOTO_COMMENT = re.compile(r'^// ---- Created with 3Dmigoto', re.MULTILINE)


def extract_hash(name: str) -> str | None:
    """Return the 8-hex-digit hash from a filename like 0x16A9C22B.vs_5_0.hlsl."""
    m = re.search(r'[0-9A-Fa-f]{8}', name)
    return m.group(0).upper() if m else None


def find_existing(out_dir: pathlib.Path, file_hash: str) -> pathlib.Path | None:
    """Return any .hlsl file in out_dir whose name contains the given hash, or None."""
    for f in out_dir.glob("*.hlsl"):
        if file_hash.upper() in f.name.upper():
            return f
    return None


def transform(txt: str) -> str:
    """Apply the pixel-space MV transformation to shader source text."""
    # Insert include line right after the 3Dmigoto header comment (first match).
    match = MIGOTO_COMMENT.search(txt)
    if match:
        insert_pos = txt.index('\n', match.start()) + 1
        if INCLUDE_LINE not in txt:
            txt = txt[:insert_pos] + LUMA_COMMENT + '\n' + INCLUDE_LINE + '\n' + txt[insert_pos:]
    elif INCLUDE_LINE not in txt:
        txt = LUMA_COMMENT + '\n' + INCLUDE_LINE + '\n' + txt

    txt = MV_PAT.sub(LUMA_REPLACEMENT, txt)
    return txt


def process_file(src: pathlib.Path, out_dir: pathlib.Path, dry_run: bool) -> str:
    """Process one dump shader. Returns a status string."""
    txt = src.read_text(encoding='utf-8', errors='replace')

    if not MV_PAT.search(txt):
        return 'no_pattern'

    file_hash = extract_hash(src.name)
    existing = find_existing(out_dir, file_hash) if file_hash else None

    if existing is not None:
        existing_txt = existing.read_text(encoding='utf-8', errors='replace')
        if LUMA_MARKER in existing_txt:
            return f'skip_converted ({existing.name})'
        # Exists but not yet converted — overwrite it.
        dst = existing
    else:
        dst = out_dir / src.name

    new_txt = transform(txt)

    if dry_run:
        return f'would_write {dst.name}'

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(new_txt, encoding='utf-8')
    return f'written {dst.name}'


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument('--dump', type=pathlib.Path, default=DUMP_DIR_DEFAULT,
                        help='Directory containing 3Dmigoto dump .hlsl files '
                             f'(default: {DUMP_DIR_DEFAULT})')
    parser.add_argument('--out', type=pathlib.Path, default=OUT_DIR_DEFAULT,
                        help='Output directory for modified shaders '
                             f'(default: {OUT_DIR_DEFAULT})')
    parser.add_argument('--dry-run', action='store_true',
                        help='Print what would be done without writing any files')
    args = parser.parse_args()

    dump_dir: pathlib.Path = args.dump
    out_dir:  pathlib.Path = args.out

    if not dump_dir.exists():
        print(f'ERROR: dump directory not found: {dump_dir}', file=sys.stderr)
        sys.exit(1)

    written, already, skipped = [], [], []

    for src in sorted(dump_dir.glob('*.vs_5_0.hlsl')):
        result = process_file(src, out_dir, args.dry_run)
        if result.startswith('written') or result.startswith('would_write'):
            written.append((src.name, result))
        elif result.startswith('skip_converted'):
            already.append((src.name, result))
        else:
            skipped.append(src.name)

    tag = '[DRY RUN] ' if args.dry_run else ''
    print(f'{tag}Converted : {len(written)}')
    print(f'{tag}Already done (skipped): {len(already)}')
    print(f'{tag}No MV pattern (skipped): {len(skipped)}')

    if written:
        print(f'\n{tag}Files written:')
        for name, result in written:
            print(f'  {name}  ->  {result}')

    if already:
        print(f'\n{tag}Already converted:')
        for name, result in already:
            print(f'  {name}  ->  {result}')


if __name__ == '__main__':
    main()
