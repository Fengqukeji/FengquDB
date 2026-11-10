#!/bin/sh
# The Store concurrency suite is the single race-check implementation.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec sh "$root/scripts/verify-store-tsan.sh" "$@"
