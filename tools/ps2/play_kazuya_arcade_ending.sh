#!/bin/zsh
# Kazuya's Arcade ending with his Tekken Tag Tournament (PS2) ending in place
# of the ending movie, in a game window (debug build, through the slot
# queue): Kazuya (TTT1 guest) goes through the Arcade on his own (every
# round won for him, in turbo; stage 1 counts as stage 9, so Ogre comes
# second), True Ogre is beaten in real time, then the mod plays the ending
# in the same fight, Jin reloaded as player 2 in Ogre's temple, and the
# credits follow (TEKKEN3_ENDING, src/tekken3_embu_scenes.c).
#   tools/ps2/play_kazuya_arcade_ending.sh [--shots DIR]    # --shots: headless, captures
# Same inputs as tools/ps2/play_kazuya_ending.sh, which builds the packs.
ROOT=${0:A:h:h:h}
cd "$ROOT" || exit 1
W=workspace/ttt-ps2
# Denser lightning than the Embu's (the fight draws fewer of them at once).
AURA_STEP=2 zsh tools/ps2/play_kazuya_ending.sh --pack-only || exit 1
if [ "$1" = "--shots" ]; then
    exec python3 ../ressources/slot.py --kind game --who "embu-tests" --prio 2 -- \
        python3 tools/embu/arcade_end.py "$2" --plan $W/kazuya-plan.json --skip
fi
exec python3 ../ressources/slot.py --kind game --who "embu-tests" --prio 2 -- \
    python3 tools/embu/arcade_end.py workspace/embu/arcade-show --plan $W/kazuya-plan.json --skip --visible
