#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SITE_DIR="$ROOT_DIR/site"

cd "$ROOT_DIR"

python3 scripts/contract_test_openapi.py
npx --yes @redocly/cli@latest lint docs/openapi.json
SUPPRESS_NO_CONFIG_WARNING=true NODE_ENV=development npx --yes @asyncapi/cli@latest validate docs/asyncapi.yaml
mkdir -p "$SITE_DIR"
npx --yes @redocly/cli@latest build-docs docs/openapi.json --output "$SITE_DIR/index.html"
cp docs/openapi.json "$SITE_DIR/openapi.json"
mkdir -p "$SITE_DIR/mqtt"
SUPPRESS_NO_CONFIG_WARNING=true NODE_ENV=development npx --yes @asyncapi/cli@latest generate fromTemplate docs/asyncapi.yaml @asyncapi/html-template --output "$SITE_DIR/mqtt" --force-write --no-interactive --install --param singleFile=true
cp docs/asyncapi.yaml "$SITE_DIR/asyncapi.yaml"
touch "$SITE_DIR/.nojekyll"

echo "REST docs generated: $SITE_DIR/index.html"
echo "MQTT docs generated: $SITE_DIR/mqtt/index.html"
