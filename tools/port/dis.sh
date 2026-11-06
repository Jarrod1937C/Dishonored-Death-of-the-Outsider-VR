#!/bin/bash
# dis.sh d2|doto <rva-hex> [len-hex]
case $1 in d2) E=$HOME/mnt/DH2/Dishonored2.exe;; *) E=$HOME/mnt/Dishonored_DeathOfTheOutsider/Dishonored_DO.exe;; esac
S=$((0x140000000 + 0x$2)); N=$((0x${3:-80}))
objdump -d -M intel --insn-width=12 --start-address=$S --stop-address=$((S+N)) $E | grep -E '^\s+[0-9a-f]+:' | sed -E 's/^\s+14([0-9a-f]+):/\1:/'
