# Checks files written by hdrspace's "Export image" against the picture that was open.
#   check_export.py source.hdr exported.exr|exported.hdr ...
import math
import sys

import numpy as np


def read_radiance(path):
    data = open(path, "rb").read()
    head, rest = data.split(b"\n\n", 1)
    res, pix = rest.split(b"\n", 1)
    a, h, b, w = res.split()
    h, w = int(h), int(w)
    assert a == b"-Y" and b == b"+X", res
    out = np.zeros((h, w, 4), np.uint8)
    pos = 0
    for y in range(h):
        if pix[pos] == 2 and pix[pos + 1] == 2:
            pos += 4
            for c in range(4):
                x = 0
                while x < w:
                    n = pix[pos]; pos += 1
                    if n > 128:
                        out[y, x:x + n - 128, c] = pix[pos]; pos += 1; x += n - 128
                    else:
                        out[y, x:x + n, c] = list(pix[pos:pos + n]); pos += n; x += n
        else:
            out[y] = np.frombuffer(pix[pos:pos + 4 * w], np.uint8).reshape(w, 4); pos += 4 * w
    e = out[..., 3].astype(np.int32)
    f = np.where(e > 0, np.ldexp(1.0, e - 136), 0.0)
    plain = out[..., :3] * f[..., None]                       # stb_image: m * 2^(e-136)
    radiance = np.where(e[..., None] > 0, (out[..., :3] + 0.5) * f[..., None], 0.0)  # Radiance: (m+0.5)
    return head.decode("latin1").splitlines(), plain, radiance


def compare(name, got, plain, radiance):
    for label, ref in (("stb (m)", plain), ("Radiance (m+0.5)", radiance)):
        mask = ref > 1e-6
        rel = np.abs(got[mask] - ref[mask]) / ref[mask]
        print(f"  vs {label}: max rel {rel.max():.2e}, median rel {np.median(rel):.2e}")
    print(f"  {name} max {got.max():.6g}, source max {plain.max():.6g}")


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    src_head, plain, radiance = read_radiance(sys.argv[1])
    print("source header:", [l for l in src_head if l.split("=")[0] in ("PRIMARIES", "EXPOSURE", "VIEW", "FORMAT")])
    for path in sys.argv[2:]:
        print(f"== {path}")
        try:
            if path.lower().endswith(".exr"):
                import OpenEXR
                with OpenEXR.File(path) as f:
                    hdr = f.header()
                    for k in sorted(hdr):
                        v = hdr[k]
                        print(f"  header {k}: {str(v)[:120]}")
                    chans = f.channels()
                    for k, c in chans.items():
                        print(f"  channel {k}: {c.pixels.dtype} {c.pixels.shape}")
                    rgb = chans["RGB"].pixels if "RGB" in chans else (
                        chans["RGBA"].pixels[..., :3] if "RGBA" in chans else
                        np.dstack([chans[n].pixels for n in "RGB"]))
                    compare(path, rgb.astype(np.float64), plain, radiance)
            else:
                head, p2, r2 = read_radiance(path)
                print("  header lines:", head)
                compare(path, p2, plain, radiance)
        except Exception as e:  # report and go on with the next file
            print(f"  ERROR {type(e).__name__}: {e}")


if __name__ == "__main__":
    main()
