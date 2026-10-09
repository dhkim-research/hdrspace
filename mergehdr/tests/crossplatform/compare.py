"""Compare mergehdr results of two platforms (reference = macOS, test = Windows).

  python compare.py REFERENCE_DIR TEST_DIR [--summary FILE] [--demosaic]

HDR files: pixel values are decoded from RGBE and compared per channel. RGBE keeps an
8-bit mantissa, so one rounding step is up to 1/128 of a value; a pixel counts as
different when it differs by more than that. Text files: every number is compared, the
rest of the text is ignored. Exit code 1 if anything is outside tolerance.
"""
import re, sys, json, pathlib
import numpy as np

RGBE_STEP = 1.0 / 128.0      # one mantissa step, relative
TEXT_RTOL = 1e-5
TEXT_ATOL = 1e-9
MAX_PIXEL_FRACTION = 1e-3    # at most 0.1 % of the pixels may differ by more than one step

# Reported, but not counted as a failure. evalglare takes a glare source's direction from its
# centroid truncated to a whole pixel; after pixels are split off, a one-pixel source can sit
# exactly on a pixel boundary, and a rounding difference of ~1e-12 between compilers moves it
# to the neighbouring pixel (0.35 degrees at 512 px). The original evalglare has the same
# property, so the code is left as it is; evalglare_summary.txt (the totals) must still match.
INFORMATIONAL = {'evalglare_d.txt'}

# --demosaic: RAW merges. AHD and DHT choose an interpolation direction per pixel by comparing
# floating-point gradients; last-bit differences between compilers and CPUs (for example fused
# multiply-add on Apple arm64) flip some of these choices, mostly at edges, so single pixels
# differ by a few percent while the light in any area is the same. These files are checked on luminance: the image mean, and
# the means of 32x32-pixel blocks.
DEMOSAIC_MEAN_RTOL = 1e-5
DEMOSAIC_BLOCK_P99 = 2e-3
DEMOSAIC_BLOCK_MAX = 2e-2
LUMINANCE_WEIGHTS = np.array([0.2651, 0.6701, 0.0648])

def read_hdr(path):
    data = path.read_bytes()
    end = data.find(b'\n\n')
    res_end = data.find(b'\n', end + 2)
    tok = data[end + 2:res_end].split()
    height, width = int(tok[1]), int(tok[3])
    raw = np.frombuffer(data[res_end + 1:], dtype=np.uint8)
    out = np.zeros((height, width, 4), np.uint8)
    pos = 0
    for y in range(height):
        if width >= 8 and width < 32768 and raw[pos] == 2 and raw[pos + 1] == 2 and raw[pos + 2] < 128:
            pos += 4
            for c in range(4):
                x = 0
                while x < width:
                    n = int(raw[pos]); pos += 1
                    if n > 128:
                        n -= 128
                        out[y, x:x + n, c] = raw[pos]; pos += 1
                    else:
                        out[y, x:x + n, c] = raw[pos:pos + n]; pos += n
                    x += n
        else:
            out[y] = raw[pos:pos + 4 * width].reshape(width, 4); pos += 4 * width
    e = out[..., 3].astype(np.int32)
    f = np.where(e > 0, np.ldexp(1.0, e - 136), 0.0)
    return out[..., :3].astype(np.float64) * f[..., None]

NUM = re.compile(r'[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?|[-+]?(?:nan|inf)', re.I)

def compare_hdr(ref, test):
    a, b = read_hdr(ref), read_hdr(test)
    if a.shape != b.shape:
        return False, f"size {a.shape[1]}x{a.shape[0]} vs {b.shape[1]}x{b.shape[0]}"
    scale = np.maximum(np.abs(a), np.abs(b))
    diff = np.abs(a - b)
    tiny = scale.max() * 1e-9 if scale.max() > 0 else 0.0
    rel = np.where(scale > tiny, diff / np.where(scale > 0, scale, 1), 0.0)
    pixel_rel = rel.max(axis=2)
    beyond = float((pixel_rel > RGBE_STEP * 1.0001).mean())
    identical = float((diff.max(axis=2) == 0).mean())
    ok = beyond <= MAX_PIXEL_FRACTION
    return ok, (f"identical {identical*100:.2f} %, more than 1 RGBE step {beyond*100:.3f} %, "
                f"max rel {pixel_rel.max():.2e}, median rel {np.median(pixel_rel):.1e}")

def compare_demosaic(ref, test):
    a, b = read_hdr(ref), read_hdr(test)
    if a.shape != b.shape:
        return False, f"size {a.shape[1]}x{a.shape[0]} vs {b.shape[1]}x{b.shape[0]}"
    la, lb = a @ LUMINANCE_WEIGHTS, b @ LUMINANCE_WEIGHTS
    mean_rel = abs(lb.mean() / la.mean() - 1) if la.mean() > 0 else 0.0
    k = 32
    h, w = (la.shape[0] // k) * k, (la.shape[1] // k) * k
    A = la[:h, :w].reshape(h // k, k, w // k, k).mean(axis=(1, 3))
    B = lb[:h, :w].reshape(h // k, k, w // k, k).mean(axis=(1, 3))
    keep = A > A.max() * 1e-4
    r = np.abs(B - A)[keep] / A[keep]
    pix = np.abs(a - b).max(axis=2) > RGBE_STEP * np.maximum(np.abs(a), np.abs(b)).max(axis=2)
    ok = mean_rel <= DEMOSAIC_MEAN_RTOL and np.percentile(r, 99) <= DEMOSAIC_BLOCK_P99 and r.max() <= DEMOSAIC_BLOCK_MAX
    return ok, (f"mean luminance rel {mean_rel:.1e}; 32x32 blocks p99 {np.percentile(r, 99):.1e}, "
                f"max {r.max():.1e}; pixels more than 1 RGBE step {pix.mean()*100:.2f} %")

def compare_text(ref, test):
    a = NUM.findall(ref.read_text(errors='replace'))
    b = NUM.findall(test.read_text(errors='replace'))
    if len(a) != len(b):
        return False, f"{len(a)} vs {len(b)} numbers"
    worst = 0.0
    for x, y in zip(a, b):
        fx, fy = float(x), float(y)
        if np.isnan(fx) and np.isnan(fy):
            continue
        d = abs(fx - fy)
        if d > TEXT_ATOL:
            worst = max(worst, d / max(abs(fx), abs(fy), 1e-300))
    ok = worst <= TEXT_RTOL
    return ok, f"{len(a)} numbers, max rel diff {worst:.1e}"

def main():
    ref_dir, test_dir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    summary = sys.argv[sys.argv.index('--summary') + 1] if '--summary' in sys.argv else None
    rows, failed = [], 0
    for ref in sorted(ref_dir.iterdir()):
        test = test_dir / ref.name
        if not test.exists():
            ok, note = False, "missing"
        elif ref.suffix == '.hdr' and '--demosaic' in sys.argv:
            ok, note = compare_demosaic(ref, test)
        elif ref.suffix == '.hdr':
            ok, note = compare_hdr(ref, test)
        else:
            ok, note = compare_text(ref, test)
        info = ref.name in INFORMATIONAL
        if not ok and info:
            note += " (informational, see compare.py)"
        failed += not ok and not info
        rows.append((ref.name, ok, note))
        print(f"{'OK  ' if ok else ('INFO' if info else 'DIFF')} {ref.name}: {note}")
    if summary:
        with open(summary, 'a') as fh:
            title = "RAW merges" if '--demosaic' in sys.argv else "HDR tools"
            fh.write(f"## mergehdr {title}: Windows vs macOS reference\n\n| File | Result | Detail |\n|---|---|---|\n")
            for name, ok, note in rows:
                fh.write(f"| {name} | {'OK' if ok else ('info' if name in INFORMATIONAL else '**DIFF**')} | {note} |\n")
    print(f"{len(rows) - failed}/{len(rows)} within tolerance or informational")
    sys.exit(1 if failed else 0)

if __name__ == '__main__':
    main()
