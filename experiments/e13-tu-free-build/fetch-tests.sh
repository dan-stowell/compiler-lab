#!/bin/sh
# Fetch the official lua-5.4.6-tests tarball into $1 (default: ./lua-5.4.6-tests)
# and verify its sha256 (the value listed at https://www.lua.org/tests/).
set -eu
dest=${1:-lua-5.4.6-tests}
sha=63ed5f5bcfd15dfd2f72e04c2f8c10585bb8de9e558df62b335cb9e5c1a4ef34
tgz=lua-5.4.6-tests.tar.gz
[ -f "$tgz" ] || curl -sSfLo "$tgz" https://www.lua.org/tests/$tgz
echo "$sha  $tgz" | sha256sum -c -
if [ ! -d "$dest" ]; then
  mkdir -p "$dest" && tar xzf "$tgz" -C "$dest" --strip-components=1
fi
