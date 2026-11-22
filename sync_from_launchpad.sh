#!/bin/bash
# Syncs plugin sources from the SubverseLab-Launchpad monorepo (the source of
# truth for these plugins) into this standalone CI-only repo.
#
# Only sources are copied: CMakeLists.txt, Source/, tests/, Assets/,
# tools/*.py, tools/*.cpp, tools/*.h (verbatim engine headers the test tools include), README.md. Build artefacts, demos, docs, media
# and logs are excluded.
#
# Usage: ./sync_from_launchpad.sh [path-to-Launchpad-01_AI_Tools]   (DRY=-n for a dry run)
# Video scripts, logs and test reference audio stay on the Mac (excluded below).
set -euo pipefail
DRY="${DRY:-}"

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
  --exclude 'logs/'
  --exclude 'tests/ref/'
  --exclude 'tests/ui_video.cpp'
  --exclude 'tools/video*'
  --exclude 'tools/make_video.py'
  --exclude 'tools/make_site_demo.py'
  --exclude '_bak_*/'               # dated source backups (e.g. BOE-B _bak_icon_2026-11-14/Source) stay on the Mac
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
  rsync -av --prune-empty-dirs $DRY \
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
sync_plugin subverselab-kiyma
sync_plugin subverselab-highstereo   # HIGH STEREO (free stereo tool; CI: .github/workflows/highstereo.yml)
# BOE-B (free dedication plug-in; CI: .github/workflows/boeb.yml, -DBOEB_PLUGIN=ON). Assets/Portrait/ (the downloaded source photo
# and its working copies) is not used by the build and stays on the Mac: only Assets/Images/portrait_ink.png is compiled in.
echo "Syncing subverselab-boeb (BOE-B)..."
rsync -av --prune-empty-dirs $DRY \
  "${EXCLUDES[@]}" --exclude 'Assets/Portrait/' \
  "${INCLUDES_FILTER[@]}" \
  "$SRC_ROOT/subverselab-boeb/" "$DEST_ROOT/subverselab-boeb/"

# ELEK: from 2.0.0 (20 Nov 2026) the sources live in Launchpad's elek2/ (same bundle id and plug-in code); they still land in this
# repo's subverselab-splitter-desktop/ so the CI paths stay. Also its Resources/ (fonts, brand mark, app icon), but NOT the models
# htdemucs.onnx, htdemucs_6s.onnx and drumsep.onnx (over GitHub's file limit): the elek-windows job downloads them
# (ELEK_MODEL_URL, ELEK_MODEL6_URL, ELEK_DRUM_URL). Not _deps/ either: the job downloads ONNX Runtime for Windows itself.
# --delete: files the 1.x project had and 2.0.0 no longer has go (excluded paths are never deleted).
echo "Syncing elek2 -> subverselab-splitter-desktop (ELEK)..."
rsync -av --prune-empty-dirs --delete $DRY \
  "${EXCLUDES[@]}" \
  --exclude '*.onnx' --exclude '_deps/' --exclude 'work/' --exclude 'listen_*/' --exclude 'build_ui/' --exclude 'marketing/' \
  --exclude 'scripts/' --exclude 'tools/' --exclude 'Source/_before_*/' \
  --include '*/' \
  --include 'CMakeLists.txt' --include 'Source/***' --include 'Resources/***' \
  --include '*.cpp' --include 'README.md' \
  --exclude '*' \
  "$SRC_ROOT/elek2/" "$DEST_ROOT/subverselab-splitter-desktop/"

sync_plugin subverselab-seda       # SEDA (release build: SVL_LICENSING OFF, no licence keys)
sync_plugin subverselab-analog-eq  # TORNA (graphic EQ, no licence keys)
sync_plugin subverselab-dyneq      # ADALET 1.0.0 (dynamic EQ, SVL_LICENSING OFF, no licence keys; demo_material WAVs stay on the Mac)

# JING FANG (project subverselab-studio): its factory presets (presets/<family>/*.xml) are compiled in, and its build trees are
# build/, build_ui/, build-release/ (build_ui holds JUCE's own CMakeLists.txt under _deps, so the root CMakeLists is anchored).
echo "Syncing subverselab-studio (JING FANG)..."
rsync -av --prune-empty-dirs --delete $DRY \
  "${EXCLUDES[@]}" \
  --exclude 'build_ui/' --exclude 'out/' --exclude 'marketing/' --exclude 'notes/' \
  --include '*/' \
  --include '/CMakeLists.txt' --include 'Source/***' --include 'tests/*.cpp' --include 'Assets/***' --include 'presets/***' \
  --include 'tools/svl.cpp' --include 'tools/svl_gen_checks.h' \
  --exclude '*' \
  "$SRC_ROOT/subverselab-studio/" "$DEST_ROOT/subverselab-studio/"

echo "Syncing subverselab-plugin-common..."
rsync -av --prune-empty-dirs $DRY \
  "${EXCLUDES[@]}" \
  --include '*/' \
  --include 'Licensing/***' \
  --include 'SourceSense/***' \
  --include 'cmake/***' \
  --include 'tools/app_icon/***' \
  --include 'README.md' \
  --exclude '*' \
  "$SRC_ROOT/subverselab-plugin-common/" "$DEST_ROOT/subverselab-plugin-common/"

echo "Sync complete."
