#!/bin/zsh
# Play Kazuya's Arcade yourself, his TTT ending in place of the ending movie
# (tools/ps2/play_kazuya_arcade_self.py): builds the packs, opens the game
# window through the slot queue.
#   tools/ps2/play_kazuya_arcade_self.sh [--skip]     # --skip: your first fight is stage 9, Ogre next
ROOT=${0:A:h:h:h}
cd "$ROOT" || exit 1
AURA_STEP=2 zsh tools/ps2/play_kazuya_ending.sh --pack-only || exit 1
exec python3 ../ressources/slot.py --kind game --who "embu-tests" --prio 2 -- \
    python3 tools/ps2/play_kazuya_arcade_self.py "$@"
