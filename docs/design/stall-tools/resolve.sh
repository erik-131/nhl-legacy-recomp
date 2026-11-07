#!/bin/bash
# resolve.sh ADDR... : for each guest return address, show containing function and the call made
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)/generated/default"
for a in "$@"; do
  hit=$(grep -n -m1 "ctx.lr = 0x$a;" *.cpp | head -1); [ -z "$hit" ] && { echo "$a: no call site"; continue; }
  f=${hit%%:*}; rest=${hit#*:}; ln=${rest%%:*}
  fn=$(head -n $ln $f | grep -n 'DEFINE_REX_FUNC(sub_' | tail -1 | sed -E 's/.*\((sub_[0-9A-F]+)\).*/\1/')
  call=$(sed -n "$((ln+1)),$((ln+2))p" $f | tr -s ' \t' ' ' | head -c 140)
  echo "$a: in $fn calls:$call"
done
