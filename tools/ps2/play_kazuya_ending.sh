#!/bin/zsh
# Kazuya's Tekken Tag Tournament (PS2) ending, captured and converted, played
# by Tekken 3 in a game window (debug build, through the slot queue):
#   tools/ps2/play_kazuya_ending.sh [--shots DIR]      # --shots: headless, captures
# Kazuya on player 1; Jin on player 2 (as the Devil guest, drawn with his own
# TTT1 model, then with the devil-jin branch's Devil Jin model from the
# transformation, capture frame 832). Rebuilds the packs first (a build of
# build-opt-dbg stages the stock roster over them). Needs
# workspace/ttt-ps2/{kazuya,jin}-ttt1.json (tools/ps2/ttt_retarget.py) and
# workspace/ttt-ps2/jin-plain (tools/ttt1_devil_jin.py --plain), Devil Jin without
# wings on the same geometry, workspace/ttt-ps2/devil-jin-end (the roster's Jin-TTT1 has
# release's wings: another geometry, only textures change at frame 832), and Jin's red TTT1
# lightning, workspace/ttt1-import/jin/Jin-TTT1-hiteffect.tim (tools/ttt1_jin_hit_effect.py), and
# the endings' music, $W/ending-music.pcm (tools/ps2/ttt_music.py).
ROOT=${0:A:h:h:h}
cd "$ROOT" || exit 1
W=workspace/ttt-ps2
python3 tools/ps2/ttt_ending_pack.py $W/kazuya-plan.json --p1 $W/kazuya-ttt1.json --p2 $W/jin-ttt1.json \
    --p2-guest devil --p2-models "JinPlain-TTT1@0,DevilJinEnd-TTT1@832" --p2-aura 832-1023 --aura-step ${AURA_STEP:-4} \
    --p2-effect workspace/ttt1-import/jin/Jin-TTT1-hiteffect.tim --p1-effect workspace/ttt1-import/jin/Jin-TTT1-hiteffect.tim \
    --effect-light ${EFFECT_LIGHT:-ff2020} --behind 1:946-1021:4000 --hold-camera ${KAZ_HOLD:-749:24:836} --fade 40:90 --music $W/ending-music.pcm \
    --stage $W/jin-plain/Jin-TTT1-arcade-P1:JinPlain-TTT1-arcade-P1 \
    --stage $W/devil-jin-end/Jin-TTT1-arcade-P1:DevilJinEnd-TTT1-arcade-P1 || exit 1
[ "$1" = "--pack-only" ] && exit 0
if [ "$1" = "--shots" ]; then
    exec python3 ../ressources/slot.py --kind game --who "embu-tests" --prio 2 -- \
        python3 tools/embu/play.py "$2" $W/kazuya-plan.json --shots
fi
exec python3 ../ressources/slot.py --kind game --who "embu-tests" --prio 2 -- \
    python3 tools/embu/play.py $W/show $W/kazuya-plan.json --visible
