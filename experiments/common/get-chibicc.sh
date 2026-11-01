#!/bin/sh
# Fetch chibicc pinned at 90d1f7f into $1 (default: ./chibicc). Full history kept for E2.
set -eu
dest=${1:-chibicc}
[ -d "$dest/.git" ] || git clone -q https://github.com/rui314/chibicc "$dest"
git -C "$dest" checkout -q 90d1f7f
echo "$dest at $(git -C "$dest" rev-parse --short HEAD)"
