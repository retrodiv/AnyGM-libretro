#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>

set -eu

status=0
map_paths=$(sed -n 's/^| `\([^`]*\)` |.*/\1/p' docs/CODE-MAP.md)

if ! awk -F'|' '
  /^\| `(src|tests)\// {
    if(NF!=11){
      printf "CODE_MAP row has %d columns instead of 9: %s\n",NF-2,$0 > "/dev/stderr";
      failed=1;
      next;
    }
    for(i=2;i<=10;i++){
      value=$i;
      gsub(/^[[:space:]]+|[[:space:]]+$/,"",value);
      if(value==""){
        printf "CODE_MAP row has an empty field: %s\n",$0 > "/dev/stderr";
        failed=1;
      }
    }
    command=$10;
    if(command !~ /`make [^`]+`/){
      printf "CODE_MAP row has no focused make command: %s\n",$0 > "/dev/stderr";
      failed=1;
    }
  }
  END { exit failed?1:0 }
' docs/CODE-MAP.md; then
  status=1
fi

source_list=$(sed -n 's/^[[:space:]]*\(src\/[^[:space:]\\]*\.c\)[[:space:]\\]*$/\1/p' \
  Makefile.common | LC_ALL=C sort -u)

tree_list=$(find src -type f -name '*.c' -not -path 'src/third_party/*' | LC_ALL=C sort)
for source in $tree_list; do
  if ! printf '%s\n' "$source_list" | grep -F -x "$source" >/dev/null; then
    printf '%s\n' "Source is not owned by a Makefile.common group: $source" >&2
    status=1
  fi
done

for source in $source_list; do
  if [ ! -f "$source" ]; then
    printf '%s\n' "Makefile.common names a missing source: $source" >&2
    status=1
    continue
  fi
  owned=0
  for path in $map_paths; do
    case "$source" in
      "$path"*) owned=1; break ;;
    esac
  done
  if [ "$owned" -ne 1 ]; then
    printf '%s\n' "Source has no CODE_MAP owner: $source" >&2
    status=1
  fi
done

if [ "$status" -ne 0 ]; then exit "$status"; fi
printf '%s\n' "code map inventory: ok"
