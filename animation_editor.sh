#!/usr/bin/env bash
# Launch the voxelforge animation studio.
#
# Usage:
#   ./animation_editor.sh [micro/alpaca.vxw] [clip.anim]
#
# Opens the in-app animation studio (--anim) for the given micro layer,
# optionally loading a clip file. Defaults to the alpaca.
set -euo pipefail

cd "$(dirname "$0")"

LAYER="${1:-micro/alpaca.vxw}"
CLIP="${2:-}"

ARGS=(--anim --anim-layer "$LAYER")
if [[ -n "$CLIP" ]]; then
    ARGS+=(--anim-clip "$CLIP")
fi

exec ./build/voxelforge "${ARGS[@]}"