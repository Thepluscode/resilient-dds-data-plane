#!/usr/bin/env bash
set -euo pipefail

GEN=${FASTDDSGEN:-fastddsgen}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT="$ROOT/generated"
mkdir -p "$OUT"

"$GEN" -replace -d "$OUT" "$ROOT/idl/SystemTelemetry.idl"
echo "Generated Fast DDS type support in $OUT"
