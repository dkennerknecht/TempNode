#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SITE_DIR="$ROOT_DIR/site"

cd "$ROOT_DIR"

python3 scripts/contract_test_openapi.py
npx --yes @redocly/cli@latest lint docs/openapi.json
mkdir -p "$SITE_DIR"
npx --yes @redocly/cli@latest build-docs docs/openapi.json --output "$SITE_DIR/index.html"
cp docs/openapi.json "$SITE_DIR/openapi.json"
touch "$SITE_DIR/.nojekyll"

echo "API docs generated: $SITE_DIR/index.html"
