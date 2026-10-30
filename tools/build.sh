#!/usr/bin/env bash
# Thin wrapper so Git Bash / MSYS users can build without leaving the shell.
# All the real work (vcvarsall, lib, cl) lives in build.bat.
set -e
cd "$(dirname "$0")/.."
MSYS2_ARG_CONV_EXCL='*' cmd.exe /c "tools\\build.bat"
