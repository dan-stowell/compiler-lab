#!/bin/sh
# Fetch the official Lua 5.4.6 tarball from lua.org into $1 (default: ./lua-5.4.6)
# and verify its sha256 (the value lua.org/ftp/ publishes).
set -eu
dest=${1:-lua-5.4.6}
sha=7d5ea1b9cb6aa0b59ca3dde1c6adcb57ef83a1ba8e5432c0ecd06bf439b3ad88
tgz=lua-5.4.6.tar.gz
[ -f "$tgz" ] || curl -sSfLo "$tgz" https://www.lua.org/ftp/$tgz
echo "$sha  $tgz" | sha256sum -c -
if [ ! -d "$dest" ]; then
  mkdir -p "$dest" && tar xzf "$tgz" -C "$dest" --strip-components=1
fi
