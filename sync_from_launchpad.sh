#!/bin/bash
# Syncs plugin sources from the SubverseLab-Launchpad monorepo (the source of
# truth for these plugins) into this standalone CI-only repo.
#
# Only sources are copied: CMakeLists.txt, Source/, tests/, Assets/,
# tools/*.py, tools/*.cpp, tools/*.h (verbatim engine headers the test tools include), README.md. Build artefacts, demos, docs, media
# and logs are excluded.
#
# Usage: ./sync_from_launchpad.sh [path-to-Launchpad-01_AI_Tools]
set -euo pipefail

SRC_ROOT="${1:-/Users/senolsahan/Desktop/SubverseLab-Launchpad/01_AI_Tools}"
DEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

EXCLUDES=(
  --exclude 'build/'
  --exclude 'build-release/'
  --exclude 'build-*/'
  --exclude 'tests/corpus/'
  --exclude 'demos/'
  --exclude 'docs/'
  --exclude '*.wav'
  --exclude '*.mp4'
  --exclude '*.log'
  --exclude '__pycache__/'
  --exclude '.DS_Store'
)

INCLUDES_FILTER=(
  --include '*/'
  --include 'CMakeLists.txt'
  --include 'Source/***'
  --include 'tests/***'
  --include 'Assets/***'
  --include 'tools/*.py'
  --include 'tools/*.cpp'
  --include 'tools/*.h'
  --include 'README.md'
  --exclude '*'
)

sync_plugin() {
  local name="$1"
  echo "Syncing $name..."
  rsync -av --prune-empty-dirs \
    "${EXCLUDES[@]}" \
    "${INCLUDES_FILTER[@]}" \
    "$SRC_ROOT/$name/" "$DEST_ROOT/$name/"
}

sync_plugin subverselab-reverb
sync_plugin subverselab-kaset
sync_plugin subverselab-koro
sync_plugin subverselab-yanki
sync_plugin subverselab-hava
sync_plugin subverselab-tutkal
sync_plugin subverselab-mihenk
sync_plugin subverselab-taban
sync_plugin subverselab-kor        # ALEV (working name KOR)

# ELEK (project subverselab-splitter-desktop): also its Resources/ (fonts, brand mark, app icon), but NOT the 302 MB model
# htdemucs.onnx (over GitHub's file limit): the elek-windows job downloads it from ELEK_MODEL_URL. Not _deps/ either: the job
# downloads ONNX Runtime for Windows itself.
echo "Syncing subverselab-splitter-desktop (ELEK)..."
rsync -av --prune-empty-dirs \
  "${EXCLUDES[@]}" \
  --exclude '*.onnx' --exclude '_deps/' --exclude 'work/' --exclude 'listen_*/' \
  --include '*/' \
  --include 'CMakeLists.txt' --include 'Source/***' --include 'Resources/***' \
  --include '*.cpp' --include 'README.md' \
  --exclude '*' \
  "$SRC_ROOT/subverselab-splitter-desktop/" "$DEST_ROOT/subverselab-splitter-desktop/"

echo "Syncing subverselab-plugin-common..."
rsync -av --prune-empty-dirs \
  "${EXCLUDES[@]}" \
  --include '*/' \
  --include 'Licensing/***' \
  --include 'cmake/***' \
  --include 'tools/app_icon/***' \
  --include 'README.md' \
  --exclude '*' \
  "$SRC_ROOT/subverselab-plugin-common/" "$DEST_ROOT/subverselab-plugin-common/"

echo "Sync complete."
