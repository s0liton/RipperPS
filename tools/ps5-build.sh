#!/usr/bin/env bash
# Builds the PS5 payload in the SDK's Docker image (src/platform/ps5/docker).
#
#   tools/ps5-build.sh             build/ps5/ripper.elf
#   tools/ps5-build.sh clean all   any make targets
#   tools/ps5-build.sh --shell     a shell in the image
#
# PS5_IMAGE picks the image; RommPS's (romm-sync-ps5sdk) is the same SDK and is
# used when it's there, else this repository's Dockerfile is built.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${PS5_IMAGE:-}"
if [[ -z "${IMAGE}" ]]; then
  if docker image inspect romm-sync-ps5sdk:v0.43 >/dev/null 2>&1; then
    IMAGE=romm-sync-ps5sdk:v0.43
  else
    IMAGE=ripper-ps5sdk:v0.43
    docker image inspect "${IMAGE}" >/dev/null 2>&1 || docker build -t "${IMAGE}" "${ROOT}/src/platform/ps5/docker"
  fi
fi
RUN=(docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "${ROOT}":/src -w /src "${IMAGE}")
if [[ "${1:-}" == "--shell" ]]; then
  exec "${RUN[@]}" bash
fi
exec "${RUN[@]}" make -f src/platform/ps5/ps5.mk "$@"
