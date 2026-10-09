"""Compare mergehdr results of two platforms (reference = macOS, test = Windows).

  python compare.py REFERENCE_DIR TEST_DIR [--summary FILE]

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
        elif ref.suffix == '.hdr':
            ok, note = compare_hdr(ref, test)
        else:
            ok, note = compare_text(ref, test)
        failed += not ok
        rows.append((ref.name, ok, note))
        print(f"{'OK  ' if ok else 'DIFF'} {ref.name}: {note}")
    if summary:
        with open(summary, 'a') as fh:
            fh.write("## mergehdr: Windows vs macOS reference\n\n| File | Result | Detail |\n|---|---|---|\n")
            for name, ok, note in rows:
                fh.write(f"| {name} | {'OK' if ok else '**DIFF**'} | {note} |\n")
    print(f"{len(rows) - failed}/{len(rows)} within tolerance")
    sys.exit(1 if failed else 0)

if __name__ == '__main__':
    main()
