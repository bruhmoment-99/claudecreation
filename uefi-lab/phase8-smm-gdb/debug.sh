#!/usr/bin/env bash
# debug.sh - debug our ring -2 handler with GDB (default) or LLDB.
#
#   ./debug.sh                 interactive: stops in LabGuidHandler, gives you the prompt
#   ./debug.sh --demo          scripted tour, then exits (used by the self-test)
#   ./debug.sh --standalone    use the Standalone MM firmware (default: Traditional)
#   ./debug.sh --lldb          use LLDB instead of GDB
#
# How it works: QEMU runs with its GDB server on 127.0.0.1:1234. The app
# prints where the handler is loaded (DEBUG-HANDLER-ADDRESS=...) and waits.
# We subtract LabGuidHandler's offset in the symbol file to get the load
# base, load the symbols there, break on the handler and let the SMI happen.
set -euo pipefail
cd "$(dirname "$0")"
MODE=interactive; DBG=gdb; FW=../phase4-smm/firmware/bin; SYM=LabSmmHandler.debug
for a in "$@"; do
  case "$a" in
    --demo) MODE=demo ;;
    --lldb) DBG=lldb ;;
    --standalone) FW=../phase4-smm/firmware/bin-standalone; SYM=LabMmHandlerStandalone.debug ;;
    *) echo "unknown option $a"; exit 1 ;;
  esac
done
SYMPATH="$FW/symbols/$SYM"
[[ -f esp/EFI/BOOT/BOOTX64.EFI ]] || ./build.sh
cp "$FW/OVMF_VARS.fd" ovmf_vars.fd
rm -f serial.log

DISPLAY_ARGS=(-display none)
[[ $MODE == interactive ]] && DISPLAY_ARGS=()
qemu-system-x86_64 -machine q35,smm=on -m 256M -net none \
  -global mch.extended-tseg-mbytes=32 \
  -global driver=cfi.pflash01,property=secure,value=on -global ICH9-LPC.disable_s3=1 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$FW/OVMF_CODE.fd" \
  -drive if=pflash,format=raw,unit=1,file=ovmf_vars.fd \
  -drive format=raw,file=fat:rw:esp \
  -serial file:serial.log -gdb tcp:127.0.0.1:1234 "${DISPLAY_ARGS[@]}" </dev/null &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT

for _ in $(seq 1 300); do grep -q "DEBUG-HANDLER-ADDRESS=" serial.log 2>/dev/null && break; sleep 0.2; done
ADDR=$(grep -ao "DEBUG-HANDLER-ADDRESS=0x[0-9A-F]*" serial.log | head -1 | cut -d= -f2)
[[ -n "$ADDR" ]] || { echo "the app never printed the handler address"; exit 1; }
OFF=0x$(nm "$SYMPATH" | awk '$3=="LabGuidHandler"{print $1}')
BASE=$(printf '0x%X' $(( ADDR - OFF )))
echo "LabGuidHandler is at $ADDR; symbols load at $BASE ($SYM)"

CMDS=$(mktemp)
if [[ $DBG == gdb ]]; then
  {
    echo "set pagination off"; echo "set confirm off"
    echo "target remote 127.0.0.1:1234"
    echo "add-symbol-file $SYMPATH -o $BASE"
    if [[ $MODE == demo ]]; then
      echo 'echo \n=== DEMO 1: ring 0 reads the handler address ===\n'
      echo "x/8xb $ADDR"
    fi
    echo "break *$ADDR"
    echo "continue"
    if [[ $MODE == demo ]]; then
      echo 'echo \n=== DEMO 2: stopped INSIDE SMM, same address ===\n'
      echo "x/8xb $ADDR"
      echo 'echo \n=== DEMO 3: the request ring 0 sent ===\n'
      echo 'print/x ((LAB_SMM_MSG *)CommBuffer)->Arg'
      echo 'print ((LAB_SMM_MSG *)CommBuffer)->Command'
      echo "delete"
      echo "break ProcessMessage"
      echo "continue"
      echo 'echo \n=== DEMO 4: ProcessMessage works on a copy INSIDE SMRAM ===\n'
      echo "print Msg"
      echo "print/x Msg->Arg"
      echo "next"; echo "next"
      echo "delete"
      echo "detach"
    else
      echo 'echo \nStopped in LabGuidHandler, inside SMM. Try: bt, x/8xb $pc, print *(LAB_SMM_MSG *)CommBuffer, next, continue\n'
    fi
  } > "$CMDS"
  if [[ $MODE == demo ]]; then gdb -q -batch -x "$CMDS"; else gdb -q -x "$CMDS"; fi
else
  {
    echo "target create --arch x86_64 $SYMPATH"
    echo "target modules load --file $SYM --slide $BASE"
    echo "gdb-remote 127.0.0.1:1234"
    [[ $MODE == demo ]] && echo "memory read --size 1 --format x --count 8 $ADDR"   # ring 0 view
    echo "breakpoint set --address $ADDR"
    echo "process continue"
    if [[ $MODE == demo ]]; then
      echo "memory read --size 1 --format x --count 8 $ADDR"
      echo "expression -f x -- ((LAB_SMM_MSG *)CommBuffer)->Arg"
      echo "breakpoint delete --force"
      echo "breakpoint set --name ProcessMessage"
      echo "process continue"
      echo "frame variable Msg"
      echo "thread step-over"
      echo "breakpoint delete --force"
      echo "process detach"
    fi
  } > "$CMDS"
  if [[ $MODE == demo ]]; then lldb -b -s "$CMDS"; else lldb -s "$CMDS"; fi
fi
rm -f "$CMDS"
wait $QPID 2>/dev/null || true
trap - EXIT
grep -a "^\[app\]" serial.log | tail -3
