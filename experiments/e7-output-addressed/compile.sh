#!/bin/sh
# compile.sh <chibicc> <work> <key> <path>: compile one materialized source with HEAD chibicc -S.
# Leaves asm/<key>.s, asm/<key>.cpu (user sys seconds), and asm/<key>.done or asm/<key>.fail.
cc=$1 work=$2 k=$3 p=$4
cd "$work/src/$k" || exit 1
if /usr/bin/time -f "%U %S" -o "$work/asm/$k.cpu" timeout 120 "$cc" -S -o "$work/asm/$k.s" "$p" 2>"$work/asm/$k.err"; then
  touch "$work/asm/$k.done"
else
  touch "$work/asm/$k.fail" "$work/asm/$k.done"
fi
rm -rf "$work/src/$k"
