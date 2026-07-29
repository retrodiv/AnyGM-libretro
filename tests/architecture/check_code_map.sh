#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>

set -eu

status=0
map_paths=$(sed -n 's/^| `\([^`]*\)` |.*/\1/p' docs/CODE-MAP.md)

if ! awk -F'|' '
  /^## Translation-unit ownership/ { translation_units=1; next }
  /^## Common change routes/ { translation_units=0; next }
  /^\| `(src|tests)\// || (translation_units && /^\| `ANYGM_[A-Z_]+`/) {
    expected=translation_units?9:11;
    columns=translation_units?7:9;
    if(NF!=expected){
      printf "CODE_MAP row has %d columns instead of %d: %s\n",NF-2,columns,$0 > "/dev/stderr";
      failed=1;
      next;
    }
    for(i=2;i<=expected-1;i++){
      value=$i;
      gsub(/^[[:space:]]+|[[:space:]]+$/,"",value);
      if(value==""){
        printf "CODE_MAP row has an empty field: %s\n",$0 > "/dev/stderr";
        failed=1;
      }
    }
    command=$(expected-1);
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

classic_test_list=$(awk '
  /^ANYGM_CLASSIC_TEST_SOURCES :=/ { active=1; next }
  active && NF==0 { exit }
  active {
    value=$0
    gsub(/^[[:space:]]+|[[:space:]\\]+$/,"",value)
    if(value!="") print value
  }
' Makefile.common | LC_ALL=C sort)
classic_test_tree=$(
  {
    printf '%s\n' tests/support/anygm_test_runner.c
    find tests/unit/content/classic -maxdepth 1 -type f -name '*.c'
  } | LC_ALL=C sort
)
if [ "$classic_test_list" != "$classic_test_tree" ]; then
  printf '%s\n' "ANYGM_CLASSIC_TEST_SOURCES differs from the classic test tree" >&2
  status=1
fi
if ! grep -F '$(TEST_DIR)/test_classic: $(ANYGM_CLASSIC_TEST_SOURCES)' \
  Makefile >/dev/null; then
  printf '%s\n' "Classic test target does not consume ANYGM_CLASSIC_TEST_SOURCES" >&2
  status=1
fi

software3d_test_list=$(awk '
  /^ANYGM_SOFTWARE3D_TEST_SOURCES :=/ { active=1; next }
  active && NF==0 { exit }
  active {
    value=$0
    gsub(/^[[:space:]]+|[[:space:]\\]+$/,"",value)
    if(value!="") print value
  }
' Makefile.common | LC_ALL=C sort)
software3d_test_tree=$(
  {
    printf '%s\n' tests/support/anygm_test_runner.c
    find tests/unit/video/software3d -maxdepth 1 -type f -name '*.c'
  } | LC_ALL=C sort
)
if [ "$software3d_test_list" != "$software3d_test_tree" ]; then
  printf '%s\n' "ANYGM_SOFTWARE3D_TEST_SOURCES differs from the software-3D test tree" >&2
  status=1
fi
if ! grep -F '$(TEST_DIR)/test_d3_state: $(ANYGM_SOFTWARE3D_TEST_SOURCES)' \
  Makefile >/dev/null; then
  printf '%s\n' "Software-3D test target does not consume ANYGM_SOFTWARE3D_TEST_SOURCES" >&2
  status=1
fi

persistent_test_list=$(awk '
  /^ANYGM_PERSISTENT_TEST_SOURCES :=/ { active=1; next }
  active && NF==0 { exit }
  active {
    value=$0
    gsub(/^[[:space:]]+|[[:space:]\\]+$/,"",value)
    if(value!="") print value
  }
' Makefile.common | LC_ALL=C sort)
persistent_test_tree=$(
  {
    printf '%s\n' tests/support/anygm_test_runner.c
    find tests/unit/runtime/persistent -maxdepth 1 -type f -name '*.c'
  } | LC_ALL=C sort
)
if [ "$persistent_test_list" != "$persistent_test_tree" ]; then
  printf '%s\n' "ANYGM_PERSISTENT_TEST_SOURCES differs from the persistent test tree" >&2
  status=1
fi
if ! grep -F '$(TEST_DIR)/test_persistent_room: $(ANYGM_PERSISTENT_TEST_SOURCES)' \
  Makefile >/dev/null; then
  printf '%s\n' "Persistent test target does not consume ANYGM_PERSISTENT_TEST_SOURCES" >&2
  status=1
fi
if ! sed -n '/^\$(TEST_DIR)\/test_datafile_security:/,/^$/p' Makefile |
  grep -F '$(ANYGM_MEDIA_SOURCES)' >/dev/null; then
  printf '%s\n' "Datafile security target does not consume ANYGM_MEDIA_SOURCES" >&2
  status=1
fi

for retired_test in \
  tests/unit/content/test_classic.c \
  tests/unit/runtime/test_d3_state.c \
  tests/unit/runtime/test_persistent_room.c
do
  if [ -e "$retired_test" ]; then
    printf '%s\n' "Retired aggregate test returned: $retired_test" >&2
    status=1
  fi
done

if find tests/unit/video/software3d tests/unit/runtime/persistent -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H 'raster_fixtures' {} + \
  2>/dev/null | grep . >/dev/null; then
  printf '%s\n' "The retired aggregate raster_fixtures function returned" >&2
  status=1
fi

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
