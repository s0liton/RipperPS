#!/usr/bin/env bash
# PS5 Vulkan Template - upload dist/<TITLE_ID>/ to the console.
#
#   ps5/tools/deploy.sh [--all]
#
# PS5_Vulkan's deploy tool uploads what changed (eboot.bin always), over the
# console's FTP server, with the address from PS5_Vulkan's .env.
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: MIT
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}" && pwd)
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$ps5/sce_sys/param.json")
[[ -d $root/dist/$title_id ]] || { echo "no dist/$title_id: run ps5/tools/build.sh first" >&2; exit 2; }
# The payload RipperPS carries changes without changing size often enough
# that a size check can't tell: it always goes too.
exec python3 "$vulkan/tools/deploy-title-folder.py" --always eboot.bin --always assets/ripper/ripper.elf "$root/dist/$title_id" "$@"
