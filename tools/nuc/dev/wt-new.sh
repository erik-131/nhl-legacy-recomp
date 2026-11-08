#!/bin/bash
# wt-new.sh NAME [BASE_BRANCH]   New worktree in nhl-work/NAME with everything prepared, in ~1 min instead of 30+ min.
#   - git worktree on a new branch NAME from BASE (default dev/nuc-perf); never touches the main tree's checkout
#   - generated/ HARDLINKED from the main tree (384 MB, no extra disk) and the in-build `rexglue codegen` step switched off in the copy
#     (NHL_SKIP_CODEGEN): a fresh build dir otherwise runs the full single-threaded codegen (measured 16+ min, 100 % of one core)
#     and a symlinked generated/ would overwrite the main tree's files. Run codegen by hand only if the manifest changes:
#       ninja -C out/linux-rel -DNHL_SKIP_CODEGEN=OFF  ->  cmake -S . -B out/linux-rel -DNHL_SKIP_CODEGEN=OFF && ninja -C out/linux-rel nhllegacy_codegen
#   - game/ and third_party/ symlinked, build dir out/linux-rel configured with clang-20, lld, x86-64-v3 and ccache
#   - the shared ccache (base_dir + no hash_dir, see tools/nuc/dev/ccache.conf) makes the SDK and generated objects cache hits
# Then: ninja -C out/linux-rel nhllegacy   and   tools/nuc/bench/bq.sh deploy
set -euo pipefail
NAME=${1:?usage: wt-new.sh NAME [BASE]}; BASE=${2:-dev/nuc-perf}
MAIN=${MAIN_TREE:-$HOME/nhl-legacy-recomp}; W=${NHL_WORK:-$HOME/nhl-work}; WT=$W/$NAME
[ -e "$WT" ] && { echo "$WT exists"; exit 1; }
git -C "$MAIN" worktree add -b "$NAME" "$WT" "$BASE"
ln -s "$MAIN/game" "$WT/game"; ln -s "$MAIN/third_party" "$WT/third_party"
mkdir -p "$WT/generated"; cp -a "$MAIN/generated/rexglue.cmake" "$WT/generated/"; cp -al "$MAIN/generated/default" "$WT/generated/default"   # hardlinks (generated/default is 384 MB)
# main's generated/ is only reusable when complete and made from the same manifests; otherwise use the content-addressed cache
if [ -z "$(ls "$WT"/generated/default/*.cpp 2>/dev/null | head -1)" ] || ! cmp -s "$MAIN/nhllegacy_functions.toml" "$WT/nhllegacy_functions.toml" || ! cmp -s "$MAIN/nhllegacy_manifest.toml" "$WT/nhllegacy_manifest.toml"; then
  "$(dirname "$0")/codegen-cached.sh" "$WT"; fi
python3 - "$WT/generated/rexglue.cmake" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
if 'NHL_SKIP_CODEGEN' not in s:
    a=s.index('add_custom_command(\n    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp"'); b=s.index('# Include DLL module')
    s=s[:a]+'if(NOT NHL_SKIP_CODEGEN)\n'+s[a:b].rstrip()+'\nelse()\n    add_custom_target(nhllegacy_codegen)\nendif()\n\n'+s[b:]
    open(p,'w').write(s)
PY
export CCACHE_CONFIGPATH=${CCACHE_CONFIGPATH:-$W/ccache.conf}
SDK=$MAIN/third_party/rexglue-sdk-main   # real path: same ccache keys as the main tree
touch "$WT/generated/.skip-codegen"
cmake -S "$WT" -B "$WT/out/linux-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3 -DREXSDK_DIR="$SDK" -DREXGLUE_ENABLE_TRACY=OFF \
  -DCMAKE_EXE_LINKER_FLAGS=--ld-path=/usr/bin/ld.lld-20 -DCMAKE_SHARED_LINKER_FLAGS=--ld-path=/usr/bin/ld.lld-20 \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DNHL_SKIP_CODEGEN=ON \
  -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON   # PCH binaries embed the tree path: with PCH a second worktree gets 0 hits on the generated TUs, without 99.9 %
echo "ready: ninja -C $WT/out/linux-rel nhllegacy"
