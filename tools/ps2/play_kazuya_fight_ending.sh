#!/bin/zsh
# Kazuya's Tekken Tag Tournament (PS2) ending as a cutscene of an Arcade
# fight in Ogre's temple (arena 6), in a game window (debug build, through
# the slot queue): the fight's own cutscene phases play our script
# (TEKKEN3_FIGHT_CINE, src/tekken3_embu_scenes.c), 3 s into the first round.
#   tools/ps2/play_kazuya_fight_ending.sh [--shots DIR]     # --shots: headless, captures
# Same inputs as tools/ps2/play_kazuya_ending.sh, which builds the packs.
ROOT=${0:A:h:h:h}
cd "$ROOT" || exit 1
W=workspace/ttt-ps2
# Denser lightning than the Embu's (the fight draws fewer of them at once).
AURA_STEP=2 zsh tools/ps2/play_kazuya_ending.sh --pack-only || exit 1
if [ "$1" = "--shots" ]; then
    exec python3 ../ressources/slot.py --kind game --who "embu-tests" --prio 2 -- \
        python3 tools/ps2/fight_ending.py "$2" $W/kazuya-plan.json
fi
exec python3 ../ressources/slot.py --kind game --who "embu-tests" --prio 2 -- \
    python3 tools/ps2/fight_ending.py $W/fight-show $W/kazuya-plan.json --visible
