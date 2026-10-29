# compare_hdr.py a.hdr b.hdr : pixel difference of two Radiance pictures (decoded like Radiance)
import sys
import numpy as np
sys.path.insert(0, __file__.rsplit("/", 1)[0].rsplit("\\", 1)[0])
from check_export import read_radiance

ha, _, a = read_radiance(sys.argv[1])
hb, _, b = read_radiance(sys.argv[2])
print("sizes", a.shape, b.shape)
if a.shape == b.shape:
    m = np.maximum(np.abs(a), np.abs(b)) > 1e-6
    rel = np.abs(a - b)[m] / np.maximum(np.abs(a), np.abs(b))[m]
    print(f"identical pixels {np.mean(np.all(a == b, axis=2)) * 100:.2f} %; max rel {rel.max() if rel.size else 0:.2e}; mean {a.mean():.6g} vs {b.mean():.6g}")
