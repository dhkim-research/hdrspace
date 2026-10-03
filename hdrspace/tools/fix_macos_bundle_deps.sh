#!/bin/sh
set -eu

exe_path="$1"
frameworks_dir="$2"
fftw_src="$3"
parallel_src="${4:-}"
libomp_src="${5:-}"
libgomp_src="${6:-}"

bundle_ref() {
  printf '%s\n' "@executable_path/../Frameworks/$1"
}

lib_name() {
  basename "$1"
}

lib_id() {
  if [ -z "${1:-}" ] || [ ! -e "$1" ]; then
    return 0
  fi
  otool -D "$1" 2>/dev/null | sed -n '2p'
}

real_path() {
  if [ -z "${1:-}" ] || [ ! -e "$1" ]; then
    return 0
  fi
  python3 - "$1" <<'PY'
import os, sys
print(os.path.realpath(sys.argv[1]))
PY
}

maybe_change() {
  target="$1"
  old_ref="$2"
  new_ref="$3"
  if [ -z "${old_ref:-}" ] || [ ! -e "$target" ]; then
    return 0
  fi
  if otool -L "$target" 2>/dev/null | awk 'NR > 1 {print $1}' | grep -Fx -- "$old_ref" >/dev/null 2>&1; then
    install_name_tool -change "$old_ref" "$new_ref" "$target"
  fi
}

maybe_set_id() {
  target="$1"
  new_id="$2"
  if [ -n "${new_id:-}" ] && [ -e "$target" ]; then
    install_name_tool -id "$new_id" "$target"
  fi
}

fftw_name="$(lib_name "$fftw_src")"
fftw_bundle="$frameworks_dir/$fftw_name"
fftw_bundle_ref="$(bundle_ref "$fftw_name")"
fftw_src_id="$(lib_id "$fftw_src")"
fftw_src_real="$(real_path "$fftw_src")"

if [ -e "$fftw_bundle" ]; then
  maybe_change "$exe_path" "$fftw_src_id" "$fftw_bundle_ref"
  maybe_change "$exe_path" "$fftw_src_real" "$fftw_bundle_ref"
  maybe_change "$exe_path" "$fftw_src" "$fftw_bundle_ref"
  maybe_set_id "$fftw_bundle" "$fftw_bundle_ref"
fi

if [ -n "$parallel_src" ] && [ -e "$parallel_src" ]; then
  parallel_name="$(lib_name "$parallel_src")"
  parallel_bundle="$frameworks_dir/$parallel_name"
  parallel_bundle_ref="$(bundle_ref "$parallel_name")"
  parallel_src_id="$(lib_id "$parallel_src")"
  parallel_src_real="$(real_path "$parallel_src")"

  if [ -e "$parallel_bundle" ]; then
    maybe_change "$exe_path" "$parallel_src_id" "$parallel_bundle_ref"
    maybe_change "$exe_path" "$parallel_src_real" "$parallel_bundle_ref"
    maybe_change "$exe_path" "$parallel_src" "$parallel_bundle_ref"
    maybe_set_id "$parallel_bundle" "$parallel_bundle_ref"

    maybe_change "$parallel_bundle" "$fftw_src_id" "$fftw_bundle_ref"
    maybe_change "$parallel_bundle" "$fftw_src_real" "$fftw_bundle_ref"
    maybe_change "$parallel_bundle" "$fftw_src" "$fftw_bundle_ref"
  fi
fi

if [ -n "$libomp_src" ] && [ -e "$libomp_src" ]; then
  libomp_name="$(lib_name "$libomp_src")"
  libomp_bundle="$frameworks_dir/$libomp_name"
  libomp_bundle_ref="$(bundle_ref "$libomp_name")"
  libomp_src_id="$(lib_id "$libomp_src")"
  libomp_src_real="$(real_path "$libomp_src")"

  if [ -e "$libomp_bundle" ]; then
    maybe_change "$exe_path" "$libomp_src_id" "$libomp_bundle_ref"
    maybe_change "$exe_path" "$libomp_src_real" "$libomp_bundle_ref"
    maybe_change "$exe_path" "$libomp_src" "$libomp_bundle_ref"
    maybe_set_id "$libomp_bundle" "$libomp_bundle_ref"
  fi
fi

if [ -n "${parallel_bundle:-}" ] && [ -e "${parallel_bundle:-}" ] && [ -n "$libomp_src" ] && [ -e "$libomp_src" ]; then
  maybe_change "$parallel_bundle" "$libomp_src_id" "$libomp_bundle_ref"
  maybe_change "$parallel_bundle" "$libomp_src_real" "$libomp_bundle_ref"
  maybe_change "$parallel_bundle" "$libomp_src" "$libomp_bundle_ref"
fi

if [ -n "$libgomp_src" ] && [ -e "$libgomp_src" ] && [ -n "${parallel_bundle:-}" ] && [ -e "${parallel_bundle:-}" ]; then
  libgomp_name="$(lib_name "$libgomp_src")"
  libgomp_bundle="$frameworks_dir/$libgomp_name"
  libgomp_bundle_ref="$(bundle_ref "$libgomp_name")"
  libgomp_src_id="$(lib_id "$libgomp_src")"
  libgomp_src_real="$(real_path "$libgomp_src")"

  if [ -e "$libgomp_bundle" ]; then
    maybe_change "$parallel_bundle" "$libgomp_src_id" "$libgomp_bundle_ref"
    maybe_change "$parallel_bundle" "$libgomp_src_real" "$libgomp_bundle_ref"
    maybe_change "$parallel_bundle" "$libgomp_src" "$libgomp_bundle_ref"
    maybe_set_id "$libgomp_bundle" "$libgomp_bundle_ref"
  fi
fi
