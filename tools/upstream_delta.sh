#!/usr/bin/env bash
# Regenerate docs/UPSTREAM_DELTA.md: what this repo changed on top of its pinned upstreams.
# Baselines are cloned into .zcode/ (gitignored) on first run; CRLF-insensitive.
# usage: tools/upstream_delta.sh > docs/UPSTREAM_DELTA.md
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cache="$root/.zcode"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Pinned upstreams (keep in sync with README "pinned ..." claims).
KRKRSDL2_REPO="https://github.com/krkrsdl2/krkrsdl2.git"; KRKRSDL2_PIN="bf207f2"
KRKRZ_REPO="https://github.com/krkrsdl2/krkrz.git";       KRKRZ_PIN="b11c43a"

ensure_clone() { # repo pin dir
    local repo="$1" pin="$2" dir="$3"
    if [ ! -d "$dir/.git" ]; then
        echo "cloning $repo -> $dir" >&2
        git clone --filter=blob:none --no-checkout "$repo" "$dir" >&2
    fi
    git -C "$dir" checkout --quiet --detach "$pin" 2>/dev/null \
        || { git -C "$dir" fetch --quiet origin "$pin" >&2; git -C "$dir" checkout --quiet --detach "$pin"; }
    git -C "$dir" rev-parse --short HEAD
}

# diff -rq into $tmp/diff.txt, honoring basename-only excludes
run_diff() { # base ours excludes...
    local base="$1" ours="$2"; shift 2
    local ex=(); for e in "$@"; do ex+=(-x "$e"); done
    diff -rq --strip-trailing-cr "$base" "$ours" "${ex[@]}" 2>/dev/null > "$tmp/diff.txt" || true
}

print_modified() { # base ours -> markdown table sorted by change size
    local base="$1" ours="$2" f d
    while IFS= read -r f; do
        d=$(diff -u --strip-trailing-cr "$base/$f" "$ours/$f" 2>/dev/null | grep -c '^[+-][^+-]' || true)
        echo "$d $f"
    done < <(grep 'differ$' "$tmp/diff.txt" \
        | sed "s|^Files $base/||; s| and .*||") | sort -rn | awk '{printf "| %s | `%s` |\n", $1, $2}'
}

print_added() { # ours-rootpath -> list of added paths (dirs get trailing /)
    local ours="$1"
    grep "^Only in $ours" "$tmp/diff.txt" | sed "s|^Only in $ours||; s|: |/|; s|^/||" \
        | while IFS= read -r p; do
            [ -d "$ours/$p" ] && echo "$p/" || echo "$p"
        done | sort
}

echo "# KRKR-ns 上游差异清单（UPSTREAM_DELTA）"
echo
echo "> 生成时间：$(date '+%Y-%m-%d %H:%M') · 生成方式：\`tools/upstream_delta.sh > docs/UPSTREAM_DELTA.md\`（换行符不敏感）"
echo "> 只统计源码；第三方 vendored 目录（krkrz submodule 内容、zlib/SDL/FAudio/simde 等）不参与对比。"
echo

k2_pin=$(ensure_clone "$KRKRSDL2_REPO" "$KRKRSDL2_PIN" "$cache/upstream-krkrsdl2-canonical")
krkrz_pin=$(ensure_clone "$KRKRZ_REPO" "$KRKRZ_PIN" "$cache/upstream-krkrsdl2-krkrz")

echo "## 引擎层：\`krkrsdl2/\` vs krkrsdl2/krkrsdl2 @ $k2_pin"
echo
run_diff "$cache/upstream-krkrsdl2-canonical" "$root/krkrsdl2" \
    .git krkrz .gradle zlib SDL simde FAudio meson_toolchains
n_mod=$(grep -c 'differ$' "$tmp/diff.txt" || true)
echo "### 我们的修改（$n_mod 个文件，按改动行数排序）"
echo
echo "| 改动行数 | 文件 |"
echo "|---|---|"
print_modified "$cache/upstream-krkrsdl2-canonical" "$root/krkrsdl2"
echo
echo "### 我们的新增（非第三方）"
echo
print_added "$root/krkrsdl2" | sed 's|^|- `|; s|$|`|'
echo

echo "## 内嵌引擎层：\`krkrsdl2/external/krkrz/\` vs krkrsdl2/krkrz @ $krkrz_pin"
echo
run_diff "$cache/upstream-krkrsdl2-krkrz" "$root/krkrsdl2/external/krkrz" \
    .git .github .gradle android-project \
    angle baseclasses freetype glm jxrlib libjpeg-turbo libogg lpng onig opus opusfile zlib
n_mod=$(grep -c 'differ$' "$tmp/diff.txt" || true)
echo "### 我们的修改（$n_mod 个文件，按改动行数排序）"
echo
echo "| 改动行数 | 文件 |"
echo "|---|---|"
print_modified "$cache/upstream-krkrsdl2-krkrz" "$root/krkrsdl2/external/krkrz"
echo
echo "### 我们的新增（非第三方）"
echo
print_added "$root/krkrsdl2/external/krkrz" | sed 's|^|- `|; s|$|`|' || true
