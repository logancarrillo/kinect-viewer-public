#!/bin/zsh
# Raise the iPhone app's build number (CURRENT_PROJECT_VERSION) by one, or set it, and regenerate the project.
# Usage: scripts/bump-build.sh [new-build-number]
# Every TestFlight upload needs a build number higher than the last one uploaded for the same version.
# KV_BUMP_SKIP_XCODEGEN=1 skips the project regeneration (used by scripts/test-bump-build.sh).
set -euo pipefail
cd "${0:A:h}/.."
# Check for xcodegen first, so a missing tool leaves project.yml untouched (nothing half done).
if [[ -z ${KV_BUMP_SKIP_XCODEGEN:-} ]] && ! command -v xcodegen >/dev/null; then
  echo "xcodegen is not on PATH; install it (brew install xcodegen), then run this again" >&2
  exit 3
fi
current=$(grep -E '^        CURRENT_PROJECT_VERSION: "[0-9]+"' project.yml | head -1 | grep -oE '[0-9]+')
if (( $# >= 1 )); then next=$1; else next=$((current + 1)); fi
if [[ ! $next =~ '^[0-9]+$' ]]; then
  echo "build number must be digits only (got '$next')" >&2
  exit 2
fi
if (( 10#$next <= 10#$current )); then
  echo "build $next is not higher than the current $current" >&2
  exit 1
fi
next=$((10#$next))
sed -i '' -E "s/^(        CURRENT_PROJECT_VERSION: \")[0-9]+(\")/\1${next}\2/" project.yml
if [[ -z ${KV_BUMP_SKIP_XCODEGEN:-} ]]; then
  xcodegen generate >/dev/null
fi
echo "iPhone app build number: $current -> $next (version $(grep -E '^        MARKETING_VERSION:' project.yml | grep -oE '[0-9.]+'))"
