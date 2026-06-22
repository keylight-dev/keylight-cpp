#!/usr/bin/env bash
set -euo pipefail
SRC="${1:-$HOME/Coding/Projects/keylight/worker/conformance/vectors.json}"
cp "$SRC" "$(dirname "$0")/../tests/fixtures/vectors.json"
echo "synced vectors from $SRC"
