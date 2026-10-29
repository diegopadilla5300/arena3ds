#!/bin/sh
# Builds arena.3dsx and arena.cia inside the devkitPro Docker image:
#   ./build.sh
# An Are.na OAuth app can be baked in (sdmc:/3ds/arena/oauth.txt overrides):
#   ARENA_CLIENT_ID=... ARENA_REDIRECT_URI=... ./build.sh
set -e
cd "$(dirname "$0")"
if [ -n "${ARENA_CLIENT_ID:-}" ] || [ ! -f source/config.h ]; then
  printf '#define ARENA_CLIENT_ID "%s"\n#define ARENA_REDIRECT_URI "%s"\n' \
    "${ARENA_CLIENT_ID:-}" "${ARENA_REDIRECT_URI:-}" > source/config.h
fi
sleep 1
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src devkitpro/devkitarm:latest sh -c '
set -e
make -j8
tools/bt/linux-x86_64/bannertool makebanner -i assets/banner.png -a assets/silence.wav -o build/banner.bnr
'
# makerom needs a newer glibc than the devkitPro image ships.
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src ubuntu:24.04 \
  tools/makerom -f cia -o arena.cia -target t -exefslogo \
  -elf arena.elf -rsf app.rsf -icon arena.smdh -banner build/banner.bnr \
  -DAPP_TITLE="Are.na 3DS" -DAPP_PRODUCT_CODE=CTR-P-ARNA -DAPP_UNIQUE_ID=0x7A4E8 \
  -DAPP_ROMFS=romfs -DAPP_CATEGORY=Application -DAPP_USE_ON_SD=true \
  -DAPP_ENCRYPTED=false -DAPP_MEMORY_TYPE=Application -DAPP_SYSTEM_MODE=64MB \
  -DAPP_SYSTEM_MODE_EXT=Legacy -DAPP_CPU_SPEED=804MHz -DAPP_ENABLE_L2_CACHE=true \
  -DAPP_VERSION_MAJOR=1 -major 1 -minor 0 -micro 0
ls -la arena.3dsx arena.cia
