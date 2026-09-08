#!/usr/bin/env bash
# FFmpeg configure host-compiler adapter for Git Bash + MSVC.
set -euo pipefail

args=()
for arg in "$@"; do
    case "$arg" in
        -Fo/*) args+=("-Fo$(cygpath -w "${arg#-Fo}")") ;;
        -Fe/*) args+=("-Fe$(cygpath -w "${arg#-Fe}")") ;;
        /Fo/*) args+=("/Fo$(cygpath -w "${arg#/Fo}")") ;;
        /Fe/*) args+=("/Fe$(cygpath -w "${arg#/Fe}")") ;;
        *) args+=("$arg") ;;
    esac
done

exec cl "${args[@]}"
