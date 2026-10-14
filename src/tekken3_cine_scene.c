/* The cinematic engine loads its own scene: tekken3_cine_play_scene() plays
 * a .cine (docs/CINE-FORMAT.md) from anywhere, in or out of a fight, with
 * the arena and the two characters the file names, then gives the hand
 * back (src/tekken3_cine.c plays it once loaded).
 *
 * How the game loads a fight (traced on the attract's demo fight, 2026-10-13):
 * mode 6 (0x800AFA88) is that demo, a fight without a selector. The attract
 * writes the fighters (0x800ADD5C + 2 * player, costumes 0x800ADD98 + 2 *
 * player) and goes to state 7 or 11. State 7 (0x80050318) blanks the
 * screen, loads the fight overlay (file +0x16 of the mode block into
 * 0x800B0548, if +0x17 says another one is there) and the fight's sounds
 * (0x80055884), then state 8 phase 0. The fight's phase machine (0x80050428,
 * table 0x800224D0): phase 0 for mode 6 skips the selector, 1 and 2 do the
 * mode's setup (none for mode 6; the arena from the CPU's fighter record,
 * 0x8004F3B0), then the loading screen (state 11, 0x80052520: the mode's
 * screen from the fight overlay, portraits), which goes on to the state and
 * phase at +0x14 / +0x15 of the mode block: state 8 phase 3. Phase 3 loads
 * everything in one call, 0x8002A40C: both fighters (0x8004F274 each:
 * model, textures, palettes, moves; guests through the TTT1 catalogue,
 * src/tekken3_ttt1_*.c), the arena 0x800ADC84 (0x8006C618: files, VRAM,
 * light, background sprites), the camera. Phase 4 waits 60 frames, 5 to 7
 * start the round, 8 fights.
 *
 * So the engine does what the attract does: mode 6, its fighters, arena and
 * dark look written (and held, the setup rewrites the arena), state 7 phase
 * 0; the loading screen, or none (CINE_SCENE_BLACK: state 11 sent on to
 * phase 3 at once, the image cleared until then); natives on their TTT1
 * moves (the poses go through that decoder); the cinematic plays as phase 7
 * comes (tekken3_cine_play, as the Arcade's ending), before its round setup
 * (0x8002A914), which draws the arena once without the fighters: an empty
 * stage between the loading and the cinematic (King's, with its clear sky). At its end everything it
 * wrote is put back as the caller had it (mode block, fighters, arena, dark
 * look, human flags, guests), then `on_done` decides what comes next; without one, the attract (state 2, as
 * the demo fight ends). */
#include "mod_plugins.h"
#include "psx_runtime.h"
#include "gpu.h"
#include "gpu_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STATE=0x800ae204, PHASE=0x800ae224, MODE=0x800afa88, MODE_SIZE=0x30, IDS=0x800add5c, COSTUMES=0x800add98,
       ARENA=0x800adc84, ARENA2=0x800afaa2, DARK=0x800afaa0, HUMAN=0x800ae1f8, SIDES=0x800adf10, AI_STAGE=0x800ae211,
       DEMO=6, CINE_SCENE_BLACK=1, PRE=30 };
extern int tekken3_cine_prepare(const char *path);
extern int tekken3_cine_play(const char *path,int pre,void (*on_done)(void));
extern int tekken3_cine_scene_of(int *arena,int *dark,char character[2][32],int costume[2]);
extern int tekken3_guest_id(const char *key);
extern int tekken3_t3_key_id(const char *key);
extern void tekken3_guest_follow(unsigned player,unsigned id,unsigned costume);
extern int tekken3_native_moves_force(unsigned player,unsigned id);

static enum { IDLE, LOADING, PLAYING } stage;
static int flags, arena, dark, id[2], costume[2];
static void (*done)(void);
/* What the caller had, put back at the end. */
static struct { unsigned state,phase; uint8_t mode[MODE_SIZE]; uint16_t id[2],costume[2],human[2],winner_side; uint8_t arena,arena2,dark,ai_stage; } saved;

int tekken3_cine_scene_loading(void) { return stage==LOADING; }

static int character(const char *token) {
    if(*token>='0' && *token<='9') return atoi(token);
    int k=tekken3_guest_id(token);
    return k>=0?k:tekken3_t3_key_id(token);
}

/* The scene's fighters, arena and dark look, as the attract writes its
 * demo's; held until phase 3 has loaded them. */
static void write_scene(void) {
    for(unsigned p=0;p<2;p++) {
        psx_mod_write_half(IDS+p*2,(uint16_t)id[p]); psx_mod_write_half(COSTUMES+p*2,(uint16_t)costume[p]);
    }
    psx_mod_write_byte(ARENA,(uint8_t)arena); psx_mod_write_byte(ARENA2,(uint8_t)arena); psx_mod_write_byte(DARK,(uint8_t)dark);
}

static void finished(void) {
    stage=IDLE;
    /* The overlay loaded (+0x17) stays what it is: state 7 loads the
     * caller's again if it needs another. */
    uint8_t loaded=psx_mod_read_byte(MODE+0x17);
    for(unsigned k=0;k<MODE_SIZE;k++) psx_mod_write_byte(MODE+k,saved.mode[k]);
    psx_mod_write_byte(MODE+0x17,loaded);
    for(unsigned p=0;p<2;p++) {
        psx_mod_write_half(IDS+p*2,saved.id[p]); psx_mod_write_half(COSTUMES+p*2,saved.costume[p]);
        tekken3_guest_follow(p,saved.id[p],saved.costume[p]);
    }
    psx_mod_write_byte(ARENA,saved.arena); psx_mod_write_byte(ARENA2,saved.arena2); psx_mod_write_byte(DARK,saved.dark);
    /* The demo's phases 0 and 2 zero the human flags and the side mirror
     * the Arcade's ending reads (state 19 phase 0 picks the winner's ending
     * from it; 0 = none, and phase 1 then waited for ever). */
    for(unsigned p=0;p<2;p++) psx_mod_write_half(HUMAN+p*2,saved.human[p]);
    psx_mod_write_half(SIDES,saved.winner_side); psx_mod_write_byte(AI_STAGE,saved.ai_stage);
    void (*then)(void)=done; done=NULL;
    fprintf(stderr,"Cine scene: over, %s\n",then?"to the caller":"to the attract");
    if(then) { then(); return; }
    /* As a fight that ends towards the title does (0x8004F8F8(3), fight
     * phase 24): the target at 0x80097F38, the state it comes from at +1,
     * then state 2, which loads the attract's overlay. Target 4 (the modes
     * menu) without its overlay froze the game. */
    psx_mod_write_byte(0x80097f38,3); psx_mod_write_byte(0x80097f39,8); psx_mod_write_byte(0x80097f3a,0);
    psx_mod_write_half(STATE,2); psx_mod_write_half(PHASE,0);
}

/* Loads the prepared cinematic's scene (or `path`'s) and plays it: from a
 * fight (state 8, any phase), state 18, or out of one (the modes menu, the
 * attract). 1 if started. */
int tekken3_cine_play_scene(const char *path,int scene_flags,void (*on_done)(void)) {
    if(stage!=IDLE || (path && !tekken3_cine_prepare(path))) return 0;
    char who[2][32];
    if(!tekken3_cine_scene_of(&arena,&dark,who,costume)) { fprintf(stderr,"Cine scene: nothing prepared\n"); return 0; }
    for(unsigned p=0;p<2;p++)
        if((id[p]=character(who[p]))<0) { fprintf(stderr,"Cine scene: no character \"%s\"\n",who[p]); return 0; }
    saved.state=psx_mod_read_half(STATE); saved.phase=psx_mod_read_half(PHASE);
    for(unsigned k=0;k<MODE_SIZE;k++) saved.mode[k]=psx_mod_read_byte(MODE+k);
    for(unsigned p=0;p<2;p++) { saved.id[p]=psx_mod_read_half(IDS+p*2); saved.costume[p]=psx_mod_read_half(COSTUMES+p*2); }
    saved.arena=psx_mod_read_byte(ARENA); saved.arena2=psx_mod_read_byte(ARENA2); saved.dark=psx_mod_read_byte(DARK);
    for(unsigned p=0;p<2;p++) saved.human[p]=psx_mod_read_half(HUMAN+p*2);
    saved.winner_side=psx_mod_read_half(SIDES); saved.ai_stage=psx_mod_read_byte(AI_STAGE);
    flags=scene_flags; done=on_done; stage=LOADING;
    /* The demo fight's mode, the fight overlay (file 0) wanted, the
     * loading screen going on to phase 3. */
    psx_mod_write_word(MODE,DEMO);
    psx_mod_write_byte(MODE+0x14,8); psx_mod_write_byte(MODE+0x15,3); psx_mod_write_byte(MODE+0x16,0);
    write_scene();
    for(unsigned p=0;p<2;p++) tekken3_guest_follow(p,(unsigned)id[p],(unsigned)costume[p]);
    psx_mod_write_half(STATE,7); psx_mod_write_half(PHASE,0);
    fprintf(stderr,"Cine scene: loading %d (costume %d) and %d (costume %d) in arena %d%s, from state %u phase %u\n",
            id[0],costume[0],id[1],costume[1],arena,dark?" (dark)":"",saved.state,saved.phase);
    return 1;
}

/* No loading screen: the image shown is cleared every frame while it
 * loads (the game draws nothing then), else the last one stays on screen
 * (a fight's ROUND 1 for 135 frames). The display turned off (GP1(03h))
 * changed nothing: the game turns it on again. */
static void black(void) {
    static uint16_t row[1024];
    GpuDisplayInfo d;
    gpu_get_display_info(&d);
    unsigned w=d.width>1024-d.display_x?1024-d.display_x:d.width,h=d.height>512-d.display_y?512-d.display_y:d.height;
    for(unsigned y=0;y<h;y++) gr_vram_transfer_in((int)d.display_x,(int)(d.display_y+y),(int)w,1,row);
}

/* Every frame (src/tekken3_embu_scenes.c). */
static void test_tick(void);
void tekken3_cine_scene_tick(void) {
    test_tick();
    if(stage!=LOADING) return;
    unsigned state=psx_mod_read_half(STATE),phase=psx_mod_read_half(PHASE);
    write_scene();
    if(flags&CINE_SCENE_BLACK) black();
    if(state==11 && (flags&CINE_SCENE_BLACK)) {
        /* No loading screen: straight to the loading, the screen shown as
         * state 11 would have it (0x8002960C(0)). */
        psx_mod_write_word(0x8009542c,1);
        psx_mod_write_half(STATE,8); psx_mod_write_half(PHASE,3);
        state=8; phase=3;
    }
    if(state!=8) return;
    if(phase==3) {
        /* After the loading screen chose their moves: natives on their TTT1
         * moves (mode 4), whose decoder plays the poses. */
        for(unsigned p=0;p<2;p++)
            if(id[p]<23 && !tekken3_native_moves_force(p,(unsigned)id[p]))
                fprintf(stderr,"Cine scene: no TTT1 moves for native %d, its poses will not play\n",id[p]);
    } else if(phase==7) {
        stage=PLAYING;
        if(!tekken3_cine_play(NULL,PRE,finished)) { fprintf(stderr,"Cine scene: could not play\n"); finished(); }
    }
}

/* A test: TEKKEN3_CINE_SCENE_TEST="file.cine[@state[:frames[:black]]]" plays
 * it from that state (default 4, the modes menu) that many frames after it
 * is reached (default 30), with the loading screen unless `black` is 1. */
static void test_tick(void) {
    static int parsed,want=-1,delay,count,black;
    static char path[1024];
    if(!parsed) {
        parsed=1;
        const char *e=getenv("TEKKEN3_CINE_SCENE_TEST");
        if(!e || !*e) return;
        const char *at=strrchr(e,'@');
        size_t n=at?(size_t)(at-e):strlen(e);
        snprintf(path,sizeof path,"%.*s",(int)n,e);
        want=4; delay=30;
        if(at) sscanf(at+1,"%d:%d:%d",&want,&delay,&black);
        fprintf(stderr,"Cine scene: test %s from state %d after %d frames%s\n",path,want,delay,black?", black":"");
    }
    if(want<0 || stage!=IDLE) return;
    /* In a fight (8): once a round runs. */
    if((int)psx_mod_read_half(STATE)!=want || (want==8 && psx_mod_read_half(PHASE)!=8)) { count=0; return; }
    if(++count>=delay) { want=-1; tekken3_cine_play_scene(path,black?CINE_SCENE_BLACK:0,NULL); }
}
