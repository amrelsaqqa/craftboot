#!/bin/bash
# Boot checks for the mt7621 machine, with explicit pass/fail expectations.
#
#   tools/mt7621-boot.sh [-k kernel] [-g guest-seconds] [case ...]
#
# Cases:
#   base     Dl=2, no cache model  -> must reach a shell (the proven baseline)
#   dl4      Dl=4, no cache model  -> must fail at Breed's fifo allocation
#   cache2   Dl=2, cache model     -> must reach a shell
#   cache4   Dl=4, cache model     -> must reach a shell
#
# Exit status is the number of failing cases, so this is usable in a loop.
set -u

cd "$(dirname "$0")/.." || exit 1
QEMU=${QEMU:-build/mipsel/qemu-system-mipsel}
KERNEL=${KERNEL:-$HOME/full.bin}
BOARD=${BOARD:-lg-gapm7200}
MEM=${MEM:-256M}
GUEST=${GUEST:-150}
OUT=${OUT:-$HOME}

usage() { sed -n '2,14p' "$0"; exit 2; }

while getopts "k:g:b:m:o:h" opt; do
    case $opt in
    k) KERNEL=$OPTARG ;;
    g) GUEST=$OPTARG ;;
    b) BOARD=$OPTARG ;;
    m) MEM=$OPTARG ;;
    o) OUT=$OPTARG ;;
    *) usage ;;
    esac
done
shift $((OPTIND - 1))

cases=("$@")
[ ${#cases[@]} -eq 0 ] && cases=(base dl4 cache2 cache4)

run_case() {
    local name=$1; shift
    local log=$OUT/boot-$name.log
    local -a fails=() passes=()
    local envs=()
    local want

    case $name in
    base)   envs=();                               passes=("root@") ;;
    dl4)    envs=("MT7621_DL=4");                  fails=("unable to allocate fifos") ;;
    cache2) envs=("MT7621_DCACHE=1");              passes=("root@") ;;
    cache4) envs=("MT7621_DCACHE=1" "MT7621_DL=4"); passes=("root@") ;;
    *) echo "unknown case: $name" >&2; return 2 ;;
    esac
    fails+=("Kernel panic" "Oops")

    local sock=${log%.log}.sock
    rm -f "$sock"
    local -a args=(tools/expect.py --serial "$log" --guest "$GUEST" --show 3
                   --sock "$sock" --wall 1200 --stall 150
                   --send 'press enter=\n'
                   --send 'press the [f] key=\n')
    for p in "${fails[@]}";  do args+=(--fail "$p"); done
    for p in "${passes[@]}"; do args+=(--pass "$p"); done
    # a console needs an input side, so the serial is a logging socket
    args+=(-- env "${envs[@]}" "$QEMU" -M "mt7621,board=$BOARD" -m "$MEM"
           -kernel "$KERNEL" -display none -monitor none
           -chardev "socket,id=ser,path=$sock,server=on,wait=off,logfile=$log"
           -serial chardev:ser)

    echo "=== $name: ${envs[*]:-no cache model}"
    "${args[@]}"
    local rc=$?
    case $rc in
    0) echo "    ok" ;;
    1) echo "    FAIL" ;;
    2) echo "    no verdict within ${GUEST}s of guest time" ;;
    *) echo "    ERROR ($rc)" ;;
    esac
    return $rc
}

bad=0
for c in "${cases[@]}"; do
    run_case "$c" || bad=$((bad + 1))
done
echo "=== $bad failing case(s) of ${#cases[@]}"
exit $bad
