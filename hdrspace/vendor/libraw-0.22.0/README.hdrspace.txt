LibRaw 0.22.0 public headers (unmodified), from https://github.com/LibRaw/LibRaw
tag 0.22.0, commit 0b56545a4f828743f28a4345cdfdd4c49f9f9a2a.
LibRaw is dual-licensed under the LGPL 2.1 and the CDDL 1.0; see COPYRIGHT and the
LICENSE files in this folder.

Why they are here: hdrspace.app ships LibRaw 0.22.0 as Contents/Frameworks/
libraw.24.dylib, the release the mergehdr helpers were validated with. LibRaw 0.22.1
changed the library ABI (libraw.25), so the helpers must be compiled against these
headers, not a newer Homebrew release. tools/build_hdrspace.py uses this folder only
when no installed Homebrew keg matches the shipped library.
