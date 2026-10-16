#!/bin/sh
# Copies Core and Windows to the Windows dev box (ssh host "edy") at %USERPROFILE%\src\AsyncMusic.
set -e
cd "$(dirname "$0")/.."
# No AppleDouble (._*) files: MSBuild's wildcards would compile them.
export COPYFILE_DISABLE=1
tar czf - --exclude='Core/build' --exclude='Core/build-*' --exclude='Windows/build-*' --exclude='Windows/packages' --exclude='Windows/x64' --exclude='Windows/ARM64' --exclude='Windows/*/x64' --exclude='Windows/*/ARM64' --exclude='.vs' Core Windows \
    | ssh "${ASYNCMUSIC_WINDOWS_HOST:-edy}" "mkdir src\\AsyncMusic 2>nul & cd /d src\\AsyncMusic && tar xzf -"
