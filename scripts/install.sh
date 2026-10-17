#!/bin/sh
# Build and install dsh-eval-guard into a DSH profile.
# Usage: scripts/install.sh [profile]   (default: web)
set -e
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PROFILE="$1"
if [ -z "$PROFILE" ]; then PROFILE=web; fi
cd "$HERE"
if [ ! -x node_modules/.bin/tsdown ]; then
  echo "build dependencies missing; run npm install (or link node_modules) first" >&2
  exit 1
fi
node_modules/.bin/tsdown
node scripts/build-native.mjs
if command -v dsh >/dev/null 2>&1; then
  dsh plugin --profile "$PROFILE" add "link:$HERE"
elif [ -n "$DSH_CHECKOUT" ]; then
  ( cd "$DSH_CHECKOUT" && pnpm dsh plugin --profile "$PROFILE" add "link:$HERE" )
else
  echo "dsh CLI not found. Add this folder to ~/.dsh/profiles/$PROFILE/package.json and its bundle list by hand." >&2
  exit 1
fi
echo "installed. Restart dsh web to load the host half."
