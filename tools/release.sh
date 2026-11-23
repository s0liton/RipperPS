#!/usr/bin/env bash
# Builds a RipperPS release from scratch, from a clean commit:
#
#   build/release/PPSA76700.zip   RipperPS, carrying the payload (what people install)
#   build/release/ripper.elf      the payload on its own
#   build/release/SHA256SUMS
#
#   tools/release.sh v0.1.0              build and check everything
#   tools/release.sh v0.1.0 --publish    then publish it as a GitHub pre-release (gh)
#
# The version must already be the tag's in all three places (ps5.mk, svc.h,
# RipperPS's param.json), and docs/releases/<tag>.md written: it's the release
# notes. RipperPS's notices name the commit of every part, so uncommitted
# changes stop it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/platform/ps5/app/ripper"
TITLE=PPSA76700
TAG="${1:?usage: tools/release.sh vX.Y.Z [--publish]}"
PUBLISH="${2:-}"
OUT="$ROOT/build/release"
die() { echo "release: $*" >&2; exit 1; }

[[ "$TAG" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || die "$TAG isn't a vX.Y.Z tag"
VERSION="${TAG#v}"
# The store tells consoles about an update from contentVersion: 0.1.0 is 00.001.000.
want=$(printf "%02d.%03d.%03d" "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}")
have=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["contentVersion"])' "$APP/ps5/sce_sys/param.json")
[[ "$have" == "$want" ]] || die "contentVersion is $have, $TAG needs $want ($APP/ps5/sce_sys/param.json)"
grep -q "^VERSION ?= $VERSION\$" "$ROOT/src/platform/ps5/ps5.mk" || die "VERSION in src/platform/ps5/ps5.mk isn't $VERSION"
grep -q "#define RIPPER_VERSION \"$VERSION\"" "$ROOT/src/service/svc.h" || die "RIPPER_VERSION in src/service/svc.h isn't $VERSION"
[[ -s "$ROOT/docs/releases/$TAG.md" ]] || die "docs/releases/$TAG.md is missing: write the release notes first"
[[ -z "$(git -C "$ROOT" status --porcelain)" ]] || die "uncommitted changes: commit them first"
if [[ "$PUBLISH" == --publish ]]; then
    command -v gh >/dev/null || die "--publish needs the gh CLI"
    ! gh release view "$TAG" >/dev/null 2>&1 || die "$TAG is published already"
fi

echo "==> host build and tests"
cmake -S "$ROOT" -B "$ROOT/build/host" -G Ninja > /dev/null
cmake --build "$ROOT/build/host" > /dev/null
(cd "$ROOT/build/host" && ctest --output-on-failure > /dev/null) || die "the host tests failed"

echo "==> payload $VERSION"
rm -f "$ROOT/build/ps5/ripper.elf"
"$ROOT/tools/ps5-build.sh" VERSION="$VERSION" > /dev/null
[[ -s "$ROOT/build/ps5/ripper.elf" ]] || die "the payload didn't build"

echo "==> RipperPS"
for art in pic0 pic1; do
    [[ -s "$APP/ps5/sce_sys/$art.dds" ]] ||
        "$ROOT/tools/ps5-app-build.sh" bash /repo/platform/ps5/app/tools/title-art.sh
done
rm -rf "$APP/build/ps5" "$APP/dist/$TITLE"
"$ROOT/tools/ps5-app-build.sh" bash -c 'cd /repo/platform/ps5/app/ripper && bash ps5/tools/build.sh ripper' > /dev/null
dirty=$(python3 -c 'import json,sys; print(" ".join(p["id"] for p in json.load(open(sys.argv[1])) if p["source"].get("dirty")))' \
    "$APP/dist/$TITLE/licenses/components.json")
[[ -z "$dirty" ]] || die "RipperPS was built from uncommitted changes in: $dirty"
cmp -s "$ROOT/build/ps5/ripper.elf" "$APP/dist/$TITLE/assets/ripper/ripper.elf" ||
    die "the ripper.elf in RipperPS isn't build/ps5/ripper.elf"

echo "==> packaging"
rm -rf "$OUT"
mkdir -p "$OUT"
(cd "$APP/dist" && zip -qrX "$OUT/$TITLE.zip" "$TITLE" -x "$TITLE/screenshots/*")
cp "$ROOT/build/ps5/ripper.elf" "$OUT/ripper.elf"
(cd "$OUT" && sha256sum "$TITLE.zip" ripper.elf > SHA256SUMS && cat SHA256SUMS)

if [[ "$PUBLISH" == --publish ]]; then
    echo "==> publishing $TAG as a pre-release"
    gh release create "$TAG" --prerelease --target "$(git -C "$ROOT" rev-parse HEAD)" \
        --title "RipperPS $VERSION" --notes-file "$ROOT/docs/releases/$TAG.md" \
        "$OUT/$TITLE.zip" "$OUT/ripper.elf" "$OUT/SHA256SUMS"
fi
