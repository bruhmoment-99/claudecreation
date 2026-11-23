#!/usr/bin/env bash
# build-firmware.sh - build our own OVMF with SMM enabled and LabSmmHandler
# baked into the firmware volume. Output: firmware/bin/OVMF_CODE.fd + OVMF_VARS.fd
#
# Why a firmware build? SMM drivers can't be loaded from a disk. The SMM core
# dispatches them from the flash firmware volume before SMRAM is locked, so
# our handler has to be part of the firmware image itself.
#
# Linux / WSL only (EDK II's GCC toolchain). Windows users don't need this:
# the result is committed in firmware/bin/.
#
# Needs: git gcc make nasm iasl (acpica-tools) uuid-dev python3
# Usage: firmware/build-firmware.sh [path-to-edk2-checkout]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PHASE="$(dirname "$HERE")"                  # .../phase4-smm (contains LabSmmPkg)
EDK2_TAG=edk2-stable202608                  # pinned release
EDK2="${1:-$HERE/edk2}"
TARGET=${TARGET:-RELEASE}                   # RELEASE or DEBUG (DEBUG logs to QEMU debugcon port 0x402)

if [[ ! -d "$EDK2/.git" ]]; then
  git clone --depth 1 --branch "$EDK2_TAG" https://github.com/tianocore/edk2.git "$EDK2"
  git -C "$EDK2" submodule update --init --depth 1 --recursive
fi

# Add our module to the OVMF platform, right after SmmCommunicationBufferDxe
# (inside the SMM_REQUIRE block of both files). Idempotent.
DSC="$EDK2/OvmfPkg/OvmfPkgX64.dsc"
FDF="$EDK2/OvmfPkg/OvmfPkgX64.fdf"
if ! grep -q LabSmmHandler "$DSC"; then
  sed -i 's|^\(\s*\)MdeModulePkg/Universal/SmmCommunicationBufferDxe/SmmCommunicationBufferDxe.inf|&\n\1LabSmmPkg/LabSmmHandler/LabSmmHandler.inf|' "$DSC"
fi
if ! grep -q LabSmmHandler "$FDF"; then
  sed -i 's|^INF  MdeModulePkg/Universal/SmmCommunicationBufferDxe/SmmCommunicationBufferDxe.inf|&\nINF  LabSmmPkg/LabSmmHandler/LabSmmHandler.inf|' "$FDF"
fi
grep -q LabSmmHandler "$DSC" && grep -q LabSmmHandler "$FDF" || { echo "could not patch OVMF DSC/FDF"; exit 1; }

cd "$EDK2"
export PACKAGES_PATH="$EDK2:$PHASE"         # lets EDK II find LabSmmPkg outside the tree
export WORKSPACE="$EDK2"
# edksetup.sh isn't written for "set -eu", so relax it while sourcing.
set --                                       # edksetup.sh would parse our args as its own
set +eu; source edksetup.sh >/dev/null; set -eu
make -s -C BaseTools -j"$(nproc)" >/dev/null   # errors still go to stderr

build -a X64 -t GCC -b "$TARGET" -n "$(nproc)" -p OvmfPkg/OvmfPkgX64.dsc \
      -D SMM_REQUIRE=TRUE -D SECURE_BOOT_ENABLE=FALSE -D NETWORK_ENABLE=FALSE

OUT="$EDK2/Build/OvmfX64/${TARGET}_GCC/FV"
mkdir -p "$HERE/bin"
cp "$OUT/OVMF_CODE.fd" "$OUT/OVMF_VARS.fd" "$HERE/bin/"
echo "firmware ready: $HERE/bin/OVMF_CODE.fd, OVMF_VARS.fd"
