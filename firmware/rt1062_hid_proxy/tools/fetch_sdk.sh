#!/usr/bin/env bash
# Fetch the open-source MCUXpresso SDK pieces this project builds against,
# pinned to one release, into ./sdk (or the directory given as $1).
#
#   tools/fetch_sdk.sh            -> firmware/rt1062_hid_proxy/sdk
#   tools/fetch_sdk.sh /opt/mcux  -> /opt/mcux  (then pass -DMCUX_SDK_DIR=/opt/mcux)
#
# Only the four repositories the firmware needs are cloned (shallow), not the
# whole west manifest. Re-running skips repositories that already exist.
set -euo pipefail

TAG="${MCUX_SDK_TAG:-MCUX_2.16.000}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:-$HERE/sdk}"
BASE="https://github.com/nxp-mcuxpresso"

# <repository> <path below DEST>
REPOS=(
    "mcux-sdk                 core"
    "mcux-sdk-middleware-usb  middleware/usb"
    "FreeRTOS-Kernel          rtos/freertos-kernel"
    "CMSIS_5                  CMSIS"
)

mkdir -p "$DEST"
for entry in "${REPOS[@]}"; do
    read -r repo path <<<"$entry"
    if [ -d "$DEST/$path/.git" ]; then
        echo "skip  $path (already present)"
        continue
    fi
    echo "clone $repo@$TAG -> $path"
    mkdir -p "$(dirname "$DEST/$path")"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$TAG" "$BASE/$repo.git" "$DEST/$path"
done
echo "SDK ready in $DEST"
