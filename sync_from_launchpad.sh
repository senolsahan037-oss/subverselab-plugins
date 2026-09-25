#!/bin/bash
# Syncs plugin sources from the SubverseLab-Launchpad monorepo (the source of
# truth for these plugins) into this standalone CI-only repo.
#
# Only sources are copied: CMakeLists.txt, Source/, tests/, Assets/,
# tools/*.py, tools/*.cpp, README.md. Build artefacts, demos, docs, media
# and logs are excluded.
#
# Usage: ./sync_from_launchpad.sh [path-to-Launchpad-01_AI_Tools]
set -euo pipefail

SRC_ROOT="${1:-/Users/senolsahan/Desktop/SubverseLab-Launchpad/01_AI_Tools}"
DEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

EXCLUDES=(
  --exclude 'build/'
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

echo "Syncing subverselab-plugin-common..."
rsync -av --prune-empty-dirs \
  "${EXCLUDES[@]}" \
  --include '*/' \
  --include 'Licensing/***' \
  --include 'README.md' \
  --exclude '*' \
  "$SRC_ROOT/subverselab-plugin-common/" "$DEST_ROOT/subverselab-plugin-common/"

echo "Sync complete."
