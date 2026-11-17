#!/bin/sh
# Builds dinput8.dll from Linux / WSL / macOS with LLVM (clang, lld, llvm-dlltool).
# No Windows SDK and no C runtime needed.
set -e
cd "$(dirname "$0")/.."
mkdir -p out
llvm-dlltool -m i386:x86-64 -d src/kernel32.def -l out/kernel32.lib
clang --target=x86_64-pc-windows-msvc -O2 -Wall -Wextra -Wno-unused-parameter \
      -fno-builtin -fno-stack-protector -ffreestanding \
      -c src/sekiro_crashfix.c -o out/sekiro_crashfix.obj
lld-link /dll /out:out/dinput8.dll /entry:DllMain /nodefaultlib /subsystem:windows \
         /machine:x64 out/sekiro_crashfix.obj out/kernel32.lib
echo "built out/dinput8.dll"
