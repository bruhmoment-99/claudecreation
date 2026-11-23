#!/usr/bin/env bash
# build-firmware.sh - build our own OVMF with SMM enabled and our MM handler
# baked into the firmware volume.
#
#   MM_MODEL=traditional (default) -> firmware/bin/            (Phases 4 + 6)
#   MM_MODEL=standalone            -> firmware/bin-standalone/ (Phase 7)
#
# Why a firmware build? MM drivers can't be loaded from a disk. The MM core
# dispatches them from the flash firmware volume before SMRAM is locked, so
# our handler has to be part of the firmware image itself.
#
# Linux / WSL only (EDK II's GCC toolchain). Windows users don't need this:
# the results are committed in firmware/bin*/.
#
# Needs: git gcc make nasm iasl (acpica-tools) uuid-dev python3
# Usage: [MM_MODEL=standalone] firmware/build-firmware.sh [path-to-edk2-checkout]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PHASE="$(dirname "$HERE")"                  # .../phase4-smm (contains LabSmmPkg)
EDK2_TAG=edk2-stable202608                  # pinned release
EDK2="${1:-$HERE/edk2}"
TARGET=${TARGET:-RELEASE}                   # RELEASE or DEBUG (DEBUG logs to QEMU debugcon port 0x402)
MM_MODEL=${MM_MODEL:-traditional}

case "$MM_MODEL" in
  traditional) STANDALONE=FALSE; OUTDIR="$HERE/bin" ;;
  standalone)  STANDALONE=TRUE;  OUTDIR="$HERE/bin-standalone" ;;
  *) echo "MM_MODEL must be traditional or standalone"; exit 1 ;;
esac

if [[ ! -d "$EDK2/.git" ]]; then
  git clone --depth 1 --branch "$EDK2_TAG" https://github.com/tianocore/edk2.git "$EDK2"
  git -C "$EDK2" submodule update --init --depth 1 --recursive
fi

# Start from OVMF's own platform files, then add our module in the branch of
# the "!if STANDALONE_MM_ENABLE" block that matches its module type:
#   traditional: LabSmmHandler.inf          after CpuHotplugSmm (DXE_SMM_DRIVER list)
#   standalone:  LabMmHandlerStandalone.inf after VariableStandaloneMm
DSC="$EDK2/OvmfPkg/OvmfPkgX64.dsc"
FDF="$EDK2/OvmfPkg/OvmfPkgX64.fdf"
git -C "$EDK2" checkout -- OvmfPkg/OvmfPkgX64.dsc OvmfPkg/OvmfPkgX64.fdf
python3 - "$DSC" "$FDF" <<'PY'
import sys
dsc, fdf = sys.argv[1], sys.argv[2]
def add_after(path, anchor, line):
    s = open(path).read()
    if s.count(anchor) != 1:
        sys.exit(f"anchor not unique in {path}: {anchor!r}")
    s = s.replace(anchor, anchor + line)
    open(path, 'w').write(s)
# DSC: components. VariableStandaloneMm has a {...} library block, so anchor on its closing.
add_after(dsc, "  OvmfPkg/CpuHotplugSmm/CpuHotplugSmm.inf\n",
               "  LabSmmPkg/LabSmmHandler/LabSmmHandler.inf\n")
add_after(dsc, "      NULL|MdeModulePkg/Library/VarCheckPolicyLib/VarCheckPolicyLibStandaloneMm.inf\n  }\n",
               "  LabSmmPkg/LabSmmHandler/LabMmHandlerStandalone.inf\n")
# FDF: what goes into the flash image.
add_after(fdf, "INF  OvmfPkg/CpuHotplugSmm/CpuHotplugSmm.inf\n",
               "INF  LabSmmPkg/LabSmmHandler/LabSmmHandler.inf\n")
add_after(fdf, "INF  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableStandaloneMm.inf\n",
               "INF  LabSmmPkg/LabSmmHandler/LabMmHandlerStandalone.inf\n")
PY

cd "$EDK2"
export PACKAGES_PATH="$EDK2:$PHASE"         # lets EDK II find LabSmmPkg outside the tree
export WORKSPACE="$EDK2"
# edksetup.sh isn't written for "set -eu", so relax it while sourcing.
set --                                       # edksetup.sh would parse our args as its own
set +eu; source edksetup.sh >/dev/null; set -eu
make -s -C BaseTools -j"$(nproc)" >/dev/null   # errors still go to stderr

build -a X64 -t GCC -b "$TARGET" -n "$(nproc)" -p OvmfPkg/OvmfPkgX64.dsc \
      -D SMM_REQUIRE=TRUE -D STANDALONE_MM_ENABLE=$STANDALONE \
      -D SECURE_BOOT_ENABLE=FALSE -D NETWORK_ENABLE=FALSE

OUT="$EDK2/Build/OvmfX64/${TARGET}_GCC/FV"
mkdir -p "$OUTDIR"
cp "$OUT/OVMF_CODE.fd" "$OUT/OVMF_VARS.fd" "$OUTDIR/"
echo "firmware ready ($MM_MODEL MM): $OUTDIR/OVMF_CODE.fd, OVMF_VARS.fd"
