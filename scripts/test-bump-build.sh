#!/bin/zsh
# Tests for scripts/bump-build.sh, run on a throwaway copy of project.yml. Usage: scripts/test-bump-build.sh
set -uo pipefail
root="${0:A:h}/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/scripts"
cp "$root/scripts/bump-build.sh" "$tmp/scripts/"
fails=0
check() { # name, expected exit, expected build afterwards, args... (CHECK_ENV replaces the default env assignments)
  local name=$1 want_exit=$2 want_build=$3; shift 3
  printf '        MARKETING_VERSION: "1.0"\n        CURRENT_PROJECT_VERSION: "5"\n' > "$tmp/project.yml"
  env ${=CHECK_ENV:-KV_BUMP_SKIP_XCODEGEN=1} zsh "$tmp/scripts/bump-build.sh" "$@" >/dev/null 2>&1
  local got_exit=$?
  local got_build=$(grep -oE 'CURRENT_PROJECT_VERSION: "[0-9]+"' "$tmp/project.yml" | grep -oE '[0-9]+')
  if [[ $got_exit == $want_exit && $got_build == $want_build ]]; then
    echo "PASS  $name"
  else
    echo "FAIL  $name (exit $got_exit, build $got_build; wanted exit $want_exit, build $want_build)"
    fails=$((fails + 1))
  fi
}
check "no argument raises by one" 0 6
check "a higher number is set" 0 9 9
check "leading zeros are read as decimal" 0 8 08
check "the same number is refused" 1 5 5
check "a lower number is refused" 1 5 4
check "letters are refused" 2 5 7a
check "a negative number is refused" 2 5 -7
check "a decimal is refused" 2 5 6.1
check "an expression is refused" 2 5 '5+1'
check "a shell substitution is refused" 2 5 '$(echo 9)'
check "an empty argument is refused" 2 5 ''
# xcodegen missing: refused before project.yml changes (PATH without xcodegen, regeneration not skipped)
CHECK_ENV="PATH=/usr/bin:/bin" check "a missing xcodegen changes nothing" 3 5
exit $fails
