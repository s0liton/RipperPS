#!/usr/bin/env bash
# Builds the libraries the PS5 SDK's pacbrew bundle ships and a development
# host may lack, at the same versions, into build/deps (no root needed):
#
#   libsmb2 6.1.0   SMB destinations
#   libnfs  5.0.3   NFS destinations
#
# Without them the host build leaves SMB and NFS out; the PS5 build always has them.
#
# CMake finds them there (CMAKE_PREFIX_PATH is set by CMakeLists.txt).
#
#   tools/host-deps.sh            build what's missing
#   tools/host-deps.sh --clean    start over
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${ROOT}/build/deps"
SRC="${PREFIX}/src"
JOBS="$(nproc 2>/dev/null || echo 4)"

if [[ "${1:-}" == "--clean" ]]; then
  rm -rf "${PREFIX}"
fi
mkdir -p "${SRC}"

# name, git URL, tag, extra CMake options
build() {
  local name="$1" url="$2" tag="$3"
  shift 3
  if [[ -f "${PREFIX}/.${name}-${tag}" ]]; then
    echo "${name} ${tag}: already built"
    return
  fi
  echo "${name} ${tag}: building"
  rm -rf "${SRC:?}/${name}"
  git -c advice.detachedHead=false clone --quiet --depth 1 --branch "${tag}" "${url}" "${SRC}/${name}"
  cmake -S "${SRC}/${name}" -B "${SRC}/${name}/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF "$@" >/dev/null
  cmake --build "${SRC}/${name}/build" -j "${JOBS}" >/dev/null
  cmake --install "${SRC}/${name}/build" >/dev/null
  touch "${PREFIX}/.${name}-${tag}"
}

build libsmb2 https://github.com/sahlberg/libsmb2.git v6.1.0 -DENABLE_EXAMPLES=OFF -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF
build libnfs https://github.com/sahlberg/libnfs.git libnfs-5.0.3 -DENABLE_EXAMPLES=OFF -DENABLE_TESTS=OFF -DENABLE_UTILS=OFF \
  -DENABLE_DOCUMENTATION=OFF

echo "done: ${PREFIX}"
