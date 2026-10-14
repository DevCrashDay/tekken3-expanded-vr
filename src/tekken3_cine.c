/* The cinematic engine: tekken3_cine_play() plays a .cine file
 * (docs/CINE-FORMAT.md) in a fight, on a frame loop of its own, in place of
 * the old way (src/tekken3_embu_scenes.c), which played our cutscenes
 * through Ogre's (the Arcade overlay's phases 16 and 17, rewritten words,
 * clips added to the .jmv packs as fake moves).
 *
 * The loop. While a cinematic plays, the fight's phase is held at 17 (so
 * every check made for Ogre's cutscene holds: no HUD, no fighting), but the
 * fight's frame function (0x80050428) never reaches its phase machine or the
 * overlay: the wrapper of its piece 0x80050450 runs one frame of ours
 * instead (a coroutine, see "Calling the game"), calling the game's
 * own routines as the overlay's 0x800B3CA0 (setup) and 0x800B4060 (a frame)
 * do, without their script: each actor's animation step (0x8003A818), the
 * camera (0x800670FC, its view built from our track by the wrapper of
 * 0x80063EEC), the arena, the actors, the effects, the fade.
 *
 * The poses. Each actor plays a record of the engine's own (guest memory,
 * never in a pack or the move graph): a copy of its stance record whose clip
 * is a sentinel. The game's decoder (0x80038B4C / 0x800389C0, wrapped in
 * src/tekken3_ttt1_combat.c) asks tekken3_cine_clip_pose() for a sentinel's
 * pose: the actor's track at the current frame, 57 TTT1 channels. So the
 * root goes through the game's own root motion and a native's bones through
 * its basis change (0x8003AD10, src/tekken3_ttt1_motion.c), as for a TTT1
 * move; the record's frame is held, its timed properties are the actor's
 * aura for this frame. */
#include "mod_plugins.h"
#include "psx_runtime.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ACTORS_MAX=8, PLAYED=2, MODELS_MAX=8, FADES_MAX=64, BONES_MAX=32, CHANNELS=57, HAND_STEPS=512 };
enum { SPARKS_MAX=64, SPRITES_MAX=16, SPRITE_POINTS_MAX=32 };
enum { ACTOR0=0x800a9228, ACTOR_SIZE=0x188c, STATE=0x800ae204, PHASE=0x800ae224 };
typedef struct { int from,to,a,b; } Fade;
typedef struct { int frame,actor,bone,kind; } Spark;
/* A sprite block: k soft puffs a frame, each x y z (stage), radius, alpha (0: not drawn). */
typedef struct { int first,count,points; uint32_t colour; int32_t (*p)[5]; } Sprites;
typedef struct {
    char character[32]; int costume;
    int place[4];
    struct { int frame; char prefix[96]; } models[MODELS_MAX]; int model_count;
    char effect[512];
    int aura_from,aura_to,aura_step,aura_bones[BONES_MAX],aura_bone_count;
    int pose_first,pose_count; uint16_t (*poses)[CHANNELS];
    struct { int frame,index; } hands[2][HAND_STEPS]; int hand_count[2];
} Actor;
typedef struct {
    char name[32],dir[1024];
    int length,arena,dark,background;           /* background: -1 the arena's */
    int actor_count; Actor actors[ACTORS_MAX];
    int cam_first,cam_count; int16_t (*cam)[6];
    Fade fades[FADES_MAX]; int fade_count;
    Spark sparks[SPARKS_MAX]; int spark_count;
    Sprites sprites[SPRITES_MAX]; int sprite_count;
    char music[1024],music_fallback[1024]; int music_frame,music_fallback_frame;
    int light;                                  /* -1 none */
} Cine;

static Cine *cine;                  /* the prepared or playing cinematic */
static int playing, tick, preroll;
static void (*done)(void);

static void cine_free(Cine *c) {
    if(!c) return;
    for(int i=0;i<ACTORS_MAX;i++) free(c->actors[i].poses);
    for(int i=0;i<c->sprite_count;i++) free(c->sprites[i].p);
    free(c->cam); free(c);
}

/* ---- The .cine file ---------------------------------------------------- */

static int words(char *line,char **w,int max) {
    char *hash=strchr(line,'#'); if(hash) *hash=0;
    int n=0;
    for(char *s=strtok(line," \t\r\n");s && n<max;s=strtok(NULL," \t\r\n")) w[n++]=s;
    return n;
}
static int num(const char *s) { return (int)strtol(s,NULL,0); }
static Actor *actor_of(Cine *c,const char *index) {
    int i=num(index);
    if(i<0 || i>=ACTORS_MAX) return NULL;
    if(i>=c->actor_count) c->actor_count=i+1;
    return &c->actors[i];
}
/* A block of n lines of `width` integers after its header line (as16: int16,
 * 0: uint16, 32: int32). */
static int block(FILE *f,int n,int width,void *out,int as16) {
    enum { WIDTH_MAX=5*SPRITE_POINTS_MAX };
    static char line[4*1024]; char *w[WIDTH_MAX+1];
    for(int k=0;k<n;k++) {
        if(!fgets(line,sizeof line,f) || words(line,w,WIDTH_MAX+1)!=width) return 0;
        for(int j=0;j<width;j++) {
            if(as16==32) ((int32_t*)out)[k*width+j]=num(w[j]);
            else if(as16) ((int16_t*)out)[k*width+j]=(int16_t)num(w[j]);
            else ((uint16_t*)out)[k*width+j]=(uint16_t)num(w[j]);
        }
    }
    return 1;
}
static Cine *cine_read(const char *path) {
    FILE *f=fopen(path,"r");
    if(!f) { fprintf(stderr,"Cine: no %s\n",path); return NULL; }
    Cine *c=calloc(1,sizeof *c);
    if(!c) { fclose(f); return NULL; }
    snprintf(c->dir,sizeof c->dir,"%s",path);
    char *slash=strrchr(c->dir,'/'),*back=strrchr(c->dir,'\\');
    if(back>slash) slash=back;
    if(slash) *slash=0; else snprintf(c->dir,sizeof c->dir,".");
    c->background=-1; c->light=-1;
    for(int i=0;i<ACTORS_MAX;i++) c->actors[i].place[3]=0x4000;
    char line[4096],*w[64]; int version=0,ok=1,lineno=0;
    while(ok && fgets(line,sizeof line,f)) {
        lineno++;
        int n=words(line,w,64);
        if(!n) continue;
        Actor *a;
        if(!strcmp(w[0],"cine") && n==2) version=num(w[1]);
        else if(!version) { fprintf(stderr,"Cine: %s does not start with \"cine 1\"\n",path); ok=0; }
        else if(!strcmp(w[0],"name") && n==2) snprintf(c->name,sizeof c->name,"%s",w[1]);
        else if(!strcmp(w[0],"length") && n==2) c->length=num(w[1]);
        else if(!strcmp(w[0],"arena") && n==2) c->arena=num(w[1]);
        else if(!strcmp(w[0],"dark") && n==2) c->dark=num(w[1]);
        else if(!strcmp(w[0],"background") && n==2) c->background=strcmp(w[1],"arena")?(int)strtol(w[1],NULL,16):-1;
        else if(!strcmp(w[0],"light") && n==2) c->light=(int)strtol(w[1],NULL,16);
        else if(!strcmp(w[0],"actor") && n==4 && (a=actor_of(c,w[1]))) {
            snprintf(a->character,sizeof a->character,"%s",w[2]); a->costume=num(w[3]);
        }
        else if(!strcmp(w[0],"place") && n==6 && (a=actor_of(c,w[1])))
            for(int k=0;k<4;k++) a->place[k]=num(w[2+k]);
        else if(!strcmp(w[0],"model") && n==4 && (a=actor_of(c,w[1])) && a->model_count<MODELS_MAX) {
            snprintf(a->models[a->model_count].prefix,sizeof a->models[0].prefix,"%s",w[2]);
            a->models[a->model_count++].frame=num(w[3]);
        }
        else if(!strcmp(w[0],"hand") && n==5 && (a=actor_of(c,w[1])) && num(w[2])>=0 && num(w[2])<2) {
            int h=num(w[2]);
            if(a->hand_count[h]<HAND_STEPS) {
                a->hands[h][a->hand_count[h]].frame=num(w[3]); a->hands[h][a->hand_count[h]++].index=num(w[4]);
            }
        }
        else if(!strcmp(w[0],"effect") && n==3 && (a=actor_of(c,w[1])))
            snprintf(a->effect,sizeof a->effect,"%s/%s",c->dir,w[2]);
        else if(!strcmp(w[0],"aura") && n>=6 && (a=actor_of(c,w[1]))) {
            a->aura_from=num(w[2]); a->aura_to=num(w[3]); a->aura_step=num(w[4])>0?num(w[4]):1;
            for(int k=5;k<n && a->aura_bone_count<BONES_MAX;k++) a->aura_bones[a->aura_bone_count++]=num(w[k]);
        }
        else if(!strcmp(w[0],"fade") && n==5 && c->fade_count<FADES_MAX)
            c->fades[c->fade_count++]=(Fade){num(w[1]),num(w[2]),num(w[3]),num(w[4])};
        else if(!strcmp(w[0],"music") && n==3) {
            snprintf(c->music,sizeof c->music,"%s/%s",c->dir,w[1]); c->music_frame=num(w[2]);
        }
        else if(!strcmp(w[0],"music-fallback") && n==3) {
            snprintf(c->music_fallback,sizeof c->music_fallback,"%s/%s",c->dir,w[1]); c->music_fallback_frame=num(w[2]);
        }
        else if(!strcmp(w[0],"camera") && n==3) {
            c->cam_first=num(w[1]); c->cam_count=num(w[2]);
            free(c->cam);
            ok=c->cam_count>0 && (c->cam=malloc(sizeof *c->cam*(size_t)c->cam_count)) && block(f,c->cam_count,6,c->cam,1);
        }
        else if(!strcmp(w[0],"poses") && n==4 && (a=actor_of(c,w[1]))) {
            /* Blocks follow one another: the first block's start, grown. */
            int first=num(w[2]),count=num(w[3]);
            if(!a->pose_count) a->pose_first=first;
            int at=first-a->pose_first;
            void *m=count>0 && at>=a->pose_count?realloc(a->poses,sizeof *a->poses*(size_t)(at+count)):NULL;
            ok=m!=NULL;
            if(ok) {
                a->poses=m;
                for(int k=a->pose_count;k<at;k++) memcpy(a->poses[k],a->poses[a->pose_count-1],sizeof *a->poses);
                ok=block(f,count,CHANNELS,a->poses[at],0);
                a->pose_count=at+count;
            }
        }
        else if(!strcmp(w[0],"spark") && n==5 && c->spark_count<SPARKS_MAX) {
            Spark *s=&c->sparks[c->spark_count++];
            s->frame=num(w[1]); s->actor=num(w[2]); s->bone=num(w[3]); s->kind=num(w[4]);
        }
        else if(!strcmp(w[0],"sprite") && n==5 && c->sprite_count<SPRITES_MAX) {
            Sprites *s=&c->sprites[c->sprite_count];
            s->first=num(w[1]); s->count=num(w[2]); s->points=num(w[3]); s->colour=(uint32_t)strtoul(w[4],NULL,16);
            ok=s->count>0 && s->points>0 && s->points<=SPRITE_POINTS_MAX
               && (s->p=malloc(sizeof *s->p*(size_t)s->count*(size_t)s->points))
               && block(f,s->count,5*s->points,s->p,32);
            if(ok) c->sprite_count++;
            else { free(s->p); s->p=NULL; }
        }
        else if(!strcmp(w[0],"sound") || !strcmp(w[0],"devil")) {}
                                                         /* reserved: not played by version 1 */
        else fprintf(stderr,"Cine: %s line %d: \"%s\" ignored\n",path,lineno,w[0]);
        if(!ok) fprintf(stderr,"Cine: %s line %d: bad \"%s\"\n",path,lineno,w[0]);
    }
    fclose(f);
    if(ok && (version!=1 || c->length<=0 || c->actor_count<1)) {
        fprintf(stderr,"Cine: %s: version %d, %d frames, %d actors: not playable\n",path,version,c->length,c->actor_count);
        ok=0;
    }
    if(!ok) { cine_free(c); return NULL; }
    fprintf(stderr,"Cine: %s \"%s\", %d frames, %d actors, camera %d frames, %d fades, %d sparks, %d sprite blocks\n",
            path,c->name,c->length,c->actor_count,c->cam_count,c->fade_count,c->spark_count,c->sprite_count);
    if(c->actor_count>PLAYED)
        fprintf(stderr,"Cine: actors %d and after are not drawn yet (two players only)\n",PLAYED);
    return c;
}

/* ---- What the rest of the game asks ----------------------------------- */

/* The frame of the cinematic, -1 before its first (the pre-roll) or when
 * none plays: the frame its tracks, model steps and fades are read at. */
int tekken3_cine_engine_frame(void) {
    if(!playing || tick<2) return -1;
    int f=tick-2-preroll;
    return f<0?-1:f;
}
int tekken3_cine_engine_active(void) { return playing; }
static int frame_or_zero(void) { int f=tekken3_cine_engine_frame(); return f<0?0:f; }

/* Model files a player is drawn with (a prefix under the TTT1 catalogue),
 * NULL for its own; from the cinematic's preparation on, so that a player
 * reloaded for it loads the first ones already. */
extern int tekken3_cine_pending(void);
const char *tekken3_cine_model(unsigned player,int *known) {
    *known=cine && (playing || tekken3_cine_pending()) && player<PLAYED && player<(unsigned)cine->actor_count && cine->actors[player].model_count;
    if(!*known) return NULL;
    const Actor *a=&cine->actors[player];
    int f=frame_or_zero();
    /* Before its first step the actor already has the first model files:
     * only textures change during the cinematic, and the geometry it is
     * loaded with must match them. */
    const char *out=a->models[0].prefix;
    for(int i=0;i<a->model_count;i++) if(f>=a->models[i].frame) out=a->models[i].prefix;
    return out;
}
/* A player's hand level (0 open .. 3 fist; the pose of the model's own table nearest
 * to it is shown, tekken3_ttt1_mod.c hand_pose) at this frame,
 * from its "hand" steps (the first one held before it), -1 for the game's own. */
int tekken3_cine_hand(unsigned player,unsigned hand) {
    if(!playing || player>=PLAYED || player>=(unsigned)cine->actor_count || hand>1) return -1;
    const Actor *a=&cine->actors[player];
    if(!a->hand_count[hand]) return -1;
    int f=frame_or_zero(),out=a->hands[hand][0].index;
    for(int i=0;i<a->hand_count[hand] && a->hands[hand][i].frame<=f;i++) out=a->hands[hand][i].index;
    return out;
}
/* A player's lightning pack and the strong-hit light while it plays. */
const char *tekken3_cine_effect(unsigned player) {
    if(!playing || player>=PLAYED || player>=(unsigned)cine->actor_count) return NULL;
    return cine->actors[player].effect[0]?cine->actors[player].effect:"";
}
int tekken3_cine_light(void) { return playing?cine->light:-2; }

/* The camera at this frame, held before its first and after its last. */
int tekken3_cine_camera(int16_t eye[3],int16_t target[3]) {
    int f=tekken3_cine_engine_frame();
    if(!playing || !cine->cam_count || f<0) return 0;     /* the pre-roll keeps the game's camera */
    int i=f-cine->cam_first;
    if(i<0) i=0;
    if(i>=cine->cam_count) i=cine->cam_count-1;
    for(int k=0;k<3;k++) { eye[k]=cine->cam[i][k]; target[k]=cine->cam[i][3+k]; }
    return 1;
}

/* The fade level at a frame (-256 black, 0 none, 256 white); the pre-roll is black. */
static int fade_level(int *on) {
    int f=tekken3_cine_engine_frame();
    *on=1;
    if(f<0) return -256;
    for(int i=0;i<cine->fade_count;i++) {
        const Fade *d=&cine->fades[i];
        if(f<d->from || f>d->to) continue;
        int level=d->to>d->from?d->a+(d->b-d->a)*(f-d->from)/(d->to-d->from):d->b;
        return level<-256?-256:level>256?256:level;
    }
    *on=0;
    return 0;
}

/* The music: raw 16-bit stereo PCM at 44 100 Hz from its frame, the game's
 * own muted for the whole cinematic, dimmed by fades to black. Returns
 * whether the engine answers (then *mute: mute the game's music). */
static int16_t *music; static uint32_t music_frames,music_pos; static int music_from=-2;
int tekken3_cine_music_mix(int16_t *out,int frames,int audible,int gain,int *mute) {
    if(!playing) { music_pos=0; return 0; }
    if(music_from==-2) {
        music_from=-1;
        const char *paths[2]={cine->music,cine->music_fallback}; int at[2]={cine->music_frame,cine->music_fallback_frame};
        for(int k=0;k<2 && music_from<0;k++) {
            FILE *f=paths[k][0]?fopen(paths[k],"rb"):NULL;
            if(!f) continue;
            fseek(f,0,SEEK_END); long size=ftell(f); fseek(f,0,SEEK_SET);
            free(music); music=malloc((size_t)size);
            if(music && fread(music,1,(size_t)size,f)==(size_t)size) { music_frames=(uint32_t)(size/4); music_from=at[k]; }
            fclose(f);
            fprintf(stderr,"Cine: music %s, %u frames from frame %d\n",paths[k],music_frames,music_from);
        }
    }
    *mute=1;
    int f=tekken3_cine_engine_frame();
    if(music_from<0 || f<music_from || music_pos>=music_frames) return 1;
    int on,level=fade_level(&on);
    int scale=gain*(level<0?level+256:256)/256;
    for(int i=0;i<frames && music_pos<music_frames;i++,music_pos++)
        for(int ch=0;ch<2;ch++) {
            int32_t v=audible?music[music_pos*2+ch]*scale/200:0, mixed=out[i*2+ch]+v;
            out[i*2+ch]=(int16_t)(mixed>32767?32767:mixed<-32768?-32768:mixed);
        }
    return 1;
}

/* ---- Poses through the game's own decoder ------------------------------ */

/* Per played actor: the sentinel clip, the record it plays (a copy of its
 * stance with that clip), its aura's timed properties, and what it played
 * before (put back at the end). */
static uint32_t sentinel[PLAYED],record[PLAYED],props[PLAYED],saved_record[PLAYED][2];
enum { HELD=2, PROPS_MAX=4 };       /* the record's frame window: 1 -> 2 every frame */

const uint16_t *tekken3_cine_clip_pose(uint32_t clip) {
    if(!playing || !clip) return NULL;
    for(unsigned p=0;p<PLAYED;p++) {
        if(clip!=sentinel[p]) continue;
        const Actor *a=&cine->actors[p];
        if(!a->pose_count) return NULL;
        int i=frame_or_zero()-a->pose_first;
        if(i<0) i=0;
        if(i>=a->pose_count) i=a->pose_count-1;
        return a->poses[i];
    }
    return NULL;
}

/* This frame's strong-hit effects on the actor (timed properties fired by
 * 0x800458C8 between the record's frames 1 and 2, at actor + 0x908 + 68 ·
 * bone): the aura's lightning, and the sparks (measured in the engine: 7
 * gives the actor its own effect, 6 the other one's: Kunimitsu's yellow and
 * Yoshimitsu's green were crossed with 6 for the own one). */
static void aura(unsigned p) {
    const Actor *a=&cine->actors[p];
    int f=tekken3_cine_engine_frame(),n=0;
    if(a->aura_bone_count && f>=a->aura_from && f<=a->aura_to && !((f-a->aura_from)%a->aura_step)) {
        int bone=a->aura_bones[(f-a->aura_from)/a->aura_step%a->aura_bone_count];
        psx_mod_write_half(props[p],HELD); psx_mod_write_half(props[p]+2,(uint16_t)(6<<8|(bone&255)));
        n=1;
    }
    for(int i=0;i<cine->spark_count && n<PROPS_MAX-1;i++) {
        const Spark *s=&cine->sparks[i];
        if(s->frame!=f || s->actor!=(int)p) continue;
        psx_mod_write_half(props[p]+n*4,HELD);
        psx_mod_write_half(props[p]+n*4+2,(uint16_t)((s->kind==(int)p?7:6)<<8|(s->bone&255)));
        n++;
    }
    psx_mod_write_word(props[p]+n*4,0);
}

/* ---- Calling the game ---------------------------------------------------- */

/* The engine never runs the game's code from C (psx_dispatch_call from a
 * wrapper is not something to build on): it is a coroutine in the fight's
 * frame function 0x80050428, at 0x80050450, the piece after its first call
 * (0x8005101C, non-zero while the game is paused) where the phase machine
 * starts. A call sets the arguments and jumps to the function with
 * 0x80050450 as its return address: the dispatcher brings its return back
 * into the wrapper, which goes on where it stopped, v0 in hand. The frame
 * ends at the function's own epilogue (0x80050EC0, its saved registers and
 * return). FRAME bytes below the stack pointer hold a fifth argument
 * (sp + 16) and some scratch (sp + SCRATCH). */
enum { ENTRY=0x80050450, EPILOGUE=0x80050ec0, FRAME=64, SCRATCH=32 };
static int line;                    /* where the coroutine goes on, 0 to start */
static int tracing(void) {
    static int t=-1;
    if(t<0) t=getenv("TEKKEN3_CINE_TRACE")!=NULL;
    return t;
}
static void issue(CPUState *cpu,uint32_t fn,int argc,const uint32_t *args) {
    if(tracing()) { fprintf(stderr,"Cine: tick %d calls %08X sp %08X\n",tick,fn,cpu->gpr[29]); fflush(stderr); }
    for(int i=0;i<argc && i<4;i++) cpu->gpr[4+i]=args[i];
    if(argc>4) psx_mod_write_word(cpu->gpr[29]+16,args[4]);
    cpu->gpr[31]=ENTRY; cpu->pc=fn;
}
/* One call a line: the line is its resume point. */
#define CALL(fn,...) do { const uint32_t args_[]={__VA_ARGS__}; \
    issue(cpu,(fn),(int)(sizeof args_/sizeof *args_),args_); line=__LINE__; return 0; case __LINE__:; } while(0)
#define CALL0(fn) do { issue(cpu,(fn),0,NULL); line=__LINE__; return 0; case __LINE__:; } while(0)
#define H(a) ((uint32_t)(int32_t)(int16_t)psx_mod_read_half(a))

/* ---- The loop ------------------------------------------------------------ */

void tekken3_cine_floor_before(void);                          /* src/tekken3_cine_floor.c */
void tekken3_cine_floor_after(CPUState *cpu,uint32_t p1,uint32_t p2);

static uint32_t tiles;          /* the black tile at the display list's far end, one per buffer */
static uint32_t ot(void) { return psx_mod_read_word(psx_mod_read_word(0x800a8c54)+4); }
static void far_tile(void) {
    if(psx_mod_read_word(0x800adca4)) return;          /* a dark arena links its own (0x80048374) */
    uint32_t t=tiles+16*(psx_mod_read_word(0x800adefc)&1);
    psx_mod_write_word(t+4,0x60000000u); psx_mod_write_word(t+8,0); psx_mod_write_word(t+12,0x01e00170u);
    uint32_t at=ot()+0xffc;
    /* Never twice in the list (a loop the GPU would walk for ever). */
    uint32_t n=at;
    for(unsigned k=0;k<16384 && (n&0x00ffffffu)!=0x00ffffffu;k++) {
        if((n&0x00ffffffu)==(t&0x00ffffffu)) {
            static int told;
            if(!told++) fprintf(stderr,"Cine: the far tile was linked already (tick %d)\n",tick);
            return;
        }
        n=0x80000000u|(psx_mod_read_word(0x80000000u|(n&0x00ffffffu))&0x00ffffffu);
    }
    psx_mod_write_word(t,(psx_mod_read_word(at)&0x00ffffffu)|0x03000000u);
    psx_mod_write_word(at,(psx_mod_read_word(at)&0xff000000u)|(t&0x00ffffffu));
}

/* ---- Sprites ------------------------------------------------------------- */

/* Each `sprite` point of this frame, a soft round puff facing the camera: a
 * fan of Gouraud-shaded additive polygons (no texture: the colour falls from
 * the centre to black at the rim, the soft disc of TTT's effect texture),
 * radius in stage units divided by the depth. Projected through the view
 * (rotation in the GTE as the floor left it, translation 0x800ADF70 + 0x14:
 * camera = R · p + T), linked near the front of the display list (one packet
 * a puff: E1 additive, then its polygons). */
enum { WORLD_VIEW=0x800adf70, NEAR_PX=120, PUFFS_MAX=64, SEGMENTS=10, PUFF_WORDS=1+SEGMENTS*6+2*SEGMENTS*8 };
static uint32_t puffs;              /* PUFFS_MAX packets per buffer */
static void sprites(CPUState *cpu) {
    int f=tekken3_cine_engine_frame();
    if(getenv("TEKKEN3_CINE_GTE") && f>=0 && !(f%60))   /* the projection, against the PS2's focal */
        fprintf(stderr,"Cine: frame %d, GTE H %u, OFX %d, OFY %d\n",f,cpu->gte_ctrl[26]&0xffff,(int32_t)cpu->gte_ctrl[24]>>16,(int32_t)cpu->gte_ctrl[25]>>16);
    if(f<0 || !cine->sprite_count) return;
    if(!puffs && !(puffs=psx_mod_alloc_gpu_dma_memory(2*PUFFS_MAX*4*(1+PUFF_WORDS),16))) {
        static int told;
        if(!told++) fprintf(stderr,"Cine: no GPU-DMA memory left for the sprites' puffs, they are not drawn\n");
        return;
    }
    const uint32_t *c=cpu->gte_ctrl;
    double r[9]={(int16_t)c[0],(int16_t)(c[0]>>16),(int16_t)c[1],(int16_t)(c[1]>>16),(int16_t)c[2],
                 (int16_t)(c[2]>>16),(int16_t)c[3],(int16_t)(c[3]>>16),(int16_t)c[4]};
    double t[3],h=(double)(c[26]&0xffff),ofx=(int32_t)c[24]/65536.0,ofy=(int32_t)c[25]/65536.0;
    for(int i=0;i<3;i++) t[i]=(int32_t)psx_mod_read_word(WORLD_VIEW+0x14+4*(uint32_t)i);
    uint32_t base=puffs+(psx_mod_read_word(0x800adefc)&1)*PUFFS_MAX*4*(1+PUFF_WORDS),slot=ot()+4;
    static const double ring[3]={0.35,0.7,1.0},shade[4]={1.0,0.8,0.4,0.0};   /* the texture's falloff */
    int drawn=0;
    #define XY(x,y) ((((uint32_t)(int32_t)lround(y))&0xffffu)<<16|(((uint32_t)(int32_t)lround(x))&0xffffu))
    for(int b=0;b<cine->sprite_count;b++) {
        const Sprites *s=&cine->sprites[b];
        int row=f-s->first;
        if(row<0 || row>=s->count) continue;
        for(int k=0;k<s->points && drawn<PUFFS_MAX;k++) {
            const int32_t *q=s->p[row*s->points+k];
            if(q[4]<=0) continue;
            double v[3];
            for(int i=0;i<3;i++) v[i]=(r[3*i]*q[0]+r[3*i+1]*q[1]+r[3*i+2]*q[2])/4096.0+t[i];
            if(v[2]<64) continue;                               /* at or behind the camera */
            double sx=ofx+h*v[0]/v[2],sy=ofy+h*v[1]/v[2],rad=h*q[3]/v[2];
            /* A puff at the camera grows past what a PS1 polygon may span (1023
             * x 511: the GPU drops the bigger ones, a sharp arc was left):
             * faded out from NEAR_PX of radius, gone at twice that. */
            if(rad<1 || rad>=2*NEAR_PX || sx+rad<-512 || sx-rad>880 || sy+rad<0 || sy-rad>480) continue;
            double near=rad>NEAR_PX?(2*NEAR_PX-rad)/NEAR_PX:1.0;
            uint32_t col[4];
            for(int j=0;j<4;j++) {
                uint32_t rgb=0;
                for(int ch=0;ch<3;ch++) {
                    double x=((s->colour>>(8*(2-ch)))&255)*(q[4]>256?256:q[4])/256.0*shade[j]*near;
                    rgb|=(uint32_t)lround(x)<<(8*ch);                 /* BGR */
                }
                col[j]=rgb;
            }
            uint32_t pkt=base+(uint32_t)drawn*4*(1+PUFF_WORDS),o=pkt+4;
            psx_mod_write_word(o,0xE1000000u|1u<<5|1u<<9|1u<<10); o+=4;       /* additive */
            double px[3][SEGMENTS],py[3][SEGMENTS];
            for(int j=0;j<3;j++)
                for(int a=0;a<SEGMENTS;a++) {
                    double ang=a*2*3.141592653589793/SEGMENTS;
                    px[j][a]=sx+cos(ang)*rad*ring[j]; py[j][a]=sy+sin(ang)*rad*ring[j];
                }
            for(int a=0;a<SEGMENTS;a++) {
                int n=(a+1)%SEGMENTS;
                psx_mod_write_word(o,0x32000000u|col[0]); psx_mod_write_word(o+4,XY(sx,sy));
                psx_mod_write_word(o+8,col[1]);           psx_mod_write_word(o+12,XY(px[0][a],py[0][a]));
                psx_mod_write_word(o+16,col[1]);          psx_mod_write_word(o+20,XY(px[0][n],py[0][n]));
                o+=24;
                for(int j=0;j<2;j++) {
                    psx_mod_write_word(o,0x3A000000u|col[j+1]); psx_mod_write_word(o+4,XY(px[j][a],py[j][a]));
                    psx_mod_write_word(o+8,col[j+1]);           psx_mod_write_word(o+12,XY(px[j][n],py[j][n]));
                    psx_mod_write_word(o+16,col[j+2]);          psx_mod_write_word(o+20,XY(px[j+1][a],py[j+1][a]));
                    psx_mod_write_word(o+24,col[j+2]);          psx_mod_write_word(o+28,XY(px[j+1][n],py[j+1][n]));
                    o+=32;
                }
            }
            uint32_t words=(o-pkt-4)/4;
            psx_mod_write_word(pkt,(psx_mod_read_word(slot)&0xffffffu)|words<<24);
            psx_mod_write_word(slot,(psx_mod_read_word(slot)&0xff000000u)|(pkt&0xffffffu));
            drawn++;
        }
    }
    #undef XY
}

/* The engine's records: each actor's stance (its record when the cinematic
 * starts) copied, with the sentinel clip, no sounds, the aura's properties
 * and a length the held frame never reaches. Put on the actor every frame. */
static int records(void) {
    if(!sentinel[0]) {
        uint32_t m=psx_mod_alloc_guest_memory(PLAYED*(16+56+PROPS_MAX*4),16);
        if(!m) return 0;
        for(unsigned p=0;p<PLAYED;p++) {
            sentinel[p]=m+p*16; record[p]=m+PLAYED*16+p*56; props[p]=m+PLAYED*(16+56)+p*PROPS_MAX*4;
            psx_mod_write_word(sentinel[p],0x454e4943u);            /* "CINE" */
            psx_mod_write_word(sentinel[p]+4,p);
        }
    }
    for(unsigned p=0;p<PLAYED;p++) {
        uint32_t x=ACTOR0+p*ACTOR_SIZE,stance=psx_mod_read_word(x+0x54);
        saved_record[p][0]=stance; saved_record[p][1]=psx_mod_read_word(x+0x4c);
        if(stance<0x80010000u) return 0;
        for(unsigned k=0;k<56;k+=4) psx_mod_write_word(record[p]+k,psx_mod_read_word(stance+k));
        psx_mod_write_word(record[p],sentinel[p]);
        psx_mod_write_word(record[p]+24,(psx_mod_read_word(record[p]+24)&0xffffff00u)|0xff);
        psx_mod_write_word(record[p]+28,0);                          /* no sounds */
        psx_mod_write_word(record[p]+32,props[p]);
        psx_mod_write_word(props[p],0);
    }
    return 1;
}
/* An actor on its engine record for this frame. A guest's stance is ready
 * only once its moves are in the guests' pool: copied again while the
 * pre-roll lasts. */
static void hold(unsigned p) {
    uint32_t x=ACTOR0+p*ACTOR_SIZE;
    if(tekken3_cine_engine_frame()<0 && psx_mod_read_word(x+0x54)!=record[p]) {
        uint32_t stance=psx_mod_read_word(x+0x54);
        for(unsigned k=4;k<56;k+=4) if(k!=24 && k!=28 && k!=32) psx_mod_write_word(record[p]+k,psx_mod_read_word(stance+k));
        saved_record[p][0]=stance; saved_record[p][1]=psx_mod_read_word(x+0x4c);
    }
    psx_mod_write_word(x+0x54,record[p]); psx_mod_write_word(x+0x4c,record[p]);
    psx_mod_write_half(x+0x5a,HELD-1); psx_mod_write_half(x+0x58,HELD); psx_mod_write_half(x+0x50,HELD);
    aura(p);
}

/* The fill tiles the arena's background is drawn with keep the colour of the
 * fight that loaded them: the cinematic's own (as ending_tick does in
 * src/tekken3_embu_scenes.c). */
static void background(void) {
    /* Version 1 loads no arena: in another one than the cinematic's (a
     * test in any fight), that arena's own colour. */
    int arena=psx_mod_read_byte(0x800adc84);
    if(arena!=cine->arena) {
        static int told;
        if(!told++) fprintf(stderr,"Cine: played in arena %d, not its own %d\n",arena,cine->arena);
    }
    int colour=cine->background;
    if(colour<0) colour=cine->dark || arena>=14?0:(int)(psx_mod_read_word(0x80097a64+(uint32_t)arena*4)&0x00ffffffu);
    for(unsigned i=0;i<4;i++) psx_mod_write_word(0x8009e73cu+i*16,0x60000000u|(uint32_t)colour);
    psx_mod_write_byte(0x800afaa0,(uint8_t)cine->dark);
}

/* One tick of the coroutine: 1 once the frame is done, 0 when it called
 * the game (it comes back here). Tick 0 is the overlay's setup (0x800B3CA0)
 * without its script or its change of the player's actor to Heihachi; tick
 * 1 its first frame (0x800B4060, state 0); then a frame each (state 1,
 * without the script). Everything kept across a call is static. */
static int run(CPUState *cpu) {
    const uint32_t p1=ACTOR0,p2=ACTOR0+ACTOR_SIZE;
    static unsigned p;
    static uint32_t x,a,b,s0,v,bit;
    switch(line) { case 0:
    if(tick==0) {
        CALL(0x800482b0,6);
        psx_mod_write_byte(0x800afaa1,1);
        CALL(0x8004b688,0);
        CALL(0x8003c258,p1);
        CALL(0x8003c258,p2);
        CALL(0x8002bd78,p1,p2);
        CALL(0x8002bd78,p2,p1);
        CALL0(0x80074f44);
        /* The side the human plays (0x800AFA88 + 0x1D bit 0) is the one
         * Ogre's cutscene turns into Heihachi; the rest of its setup kept. */
        a=psx_mod_read_byte(0x800afa88+0x1d)&1?p1:p2; b=a==p1?p2:p1;
        psx_mod_write_half(a+0x16,(uint16_t)((int16_t)psx_mod_read_half(a+0x14)/4));
        psx_mod_write_half(a+0x18,(uint16_t)((int16_t)psx_mod_read_half(a+0x14)/4));
        CALL(0x8006c618,(uint32_t)psx_mod_read_half(0x800adc84),H(p1+0x1c),H(p1+0x1c),H(p2+0x16),H(p2+0x16));
        s0=psx_mod_read_byte(b+0x1e);
        CALL(0x80036310,a,cpu->gpr[29]+SCRATCH,cpu->gpr[29]+SCRATCH+4);
        CALL(0x80035ce0,a,psx_mod_read_word(cpu->gpr[29]+SCRATCH),psx_mod_read_word(cpu->gpr[29]+SCRATCH+4));
        CALL(0x80035ce8,a,psx_mod_read_word(cpu->gpr[29]+SCRATCH),psx_mod_read_word(cpu->gpr[29]+SCRATCH+4));
        CALL(0x80035ed0,s0);
        v=cpu->gpr[2];
        CALL(0x80035eec,s0);
        CALL(0x80035ce8,b,v,cpu->gpr[2]);
        CALL(0x8006c63c,1,1);
        CALL(0x8006c63c,2,1);
        for(p=0;p<2;p++) {
            x=ACTOR0+p*ACTOR_SIZE;
            psx_mod_write_half(x+0x0e,0x4000); psx_mod_write_half(x+0x0c,0);
            psx_mod_write_word(x,0); psx_mod_write_word(x+4,0); psx_mod_write_word(x+8,0); psx_mod_write_half(x+0x10,0);
            psx_mod_write_half(x+0x2c,0x4000);
        }
        /* The actors' own placement (the default is the one above). */
        for(p=0;p<PLAYED && p<(unsigned)cine->actor_count;p++) {
            const int *pl=cine->actors[p].place; x=ACTOR0+p*ACTOR_SIZE;
            psx_mod_write_word(x,(uint32_t)pl[0]); psx_mod_write_word(x+4,(uint32_t)pl[1]); psx_mod_write_word(x+8,(uint32_t)pl[2]);
            psx_mod_write_half(x+0x0e,(uint16_t)pl[3]); psx_mod_write_half(x+0x2c,(uint16_t)pl[3]);
        }
        if(!records()) fprintf(stderr,"Cine: no stance record to copy\n");
        far_tile();
    } else if(tick==1) {
        CALL0(0x80062a10);
        CALL(0x800690a0,p1,p2);
        CALL(0x80065ed8,p1,p2);
        CALL(0x800670fc,2);
        far_tile();
    } else {
        background();
        for(p=0;p<2;p++) {
            x=ACTOR0+p*ACTOR_SIZE;
            if(p<(unsigned)cine->actor_count) hold(p);
            bit=psx_mod_read_word(psx_mod_read_word(x+0x54)+0x24)&0x40000000u;
            psx_mod_write_word(psx_mod_read_word(x+0x54)+0x24,psx_mod_read_word(psx_mod_read_word(x+0x54)+0x24)&0xbfffffffu);
            CALL(0x800458c8,x,p2);
            psx_mod_write_word(0x80096ea8,1);
            CALL(0x8003a818,x);
            psx_mod_write_word(psx_mod_read_word(x+0x54)+0x24,(psx_mod_read_word(psx_mod_read_word(x+0x54)+0x24)&0xbfffffffu)|bit);
        }
        CALL(0x800670fc,0);
        tekken3_cine_floor_before();
        CALL(0x80048240,p1,p2);
        tekken3_cine_floor_after(cpu,p1,p2);    /* the tiles near the camera it dropped */
        sprites(cpu);               /* the GTE still holds the view */
        CALL(0x8007745c,1);         /* the hit effects (Jin's lightning), as the fight's loop runs them */
        CALL0(0x8006d7cc);
        if(!psx_mod_read_word(0x800adca4)) far_tile();
        else CALL(0x80048374,psx_mod_read_word(0x800adefc));
        CALL(0x80039ddc,(uint32_t)psx_mod_read_half(0x800adc84));
        CALL(0x8003ec64,p1,p2);
        CALL0(0x80039f84);
        CALL0(0x80036000);
        CALL0(0x800378f8);
        {
            int on,level=fade_level(&on);
            if(on) {
                v=(uint32_t)(level+256);
                CALL(0x8004e000,psx_mod_read_word(0x800a9218)+8,psx_mod_read_word(0x800adf64),v);
                psx_mod_write_word(0x800adf64,cpu->gpr[2]);
            }
        }
    }
    }
    line=0;
    return 1;
}

extern void tekken3_ttt1_embu_models_reset(void);
extern void tekken3_ttt1_embu_effects_reset(void);
/* The fight as the cinematic found it: phase, the actors' placement (+0x00
 * to +0x2F: position, angles, base), the fill tiles' colour, the dark look,
 * and its camera: the setup starts the scripted camera (0x800670FC(2): its
 * mode at 0x800A0418, eye and target at 0x800A0428, the camera object at
 * 0x800A8C70), which the fight's own (0x800630D8, following the fighters)
 * never took back: it stayed on the cinematic's last shot. */
enum { CAM_MODE=0x800a0400, CAM_MODE_SIZE=0x80, CAM_OBJECT=0x800a8c40, CAM_OBJECT_SIZE=0xc0 };
static struct { unsigned phase; uint32_t actor[2][12],fill[4]; uint8_t dark;
                uint32_t mode[CAM_MODE_SIZE/4],object[CAM_OBJECT_SIZE/4]; } resume;
static void finish(void) {
    for(unsigned p=0;p<PLAYED && p<(unsigned)cine->actor_count;p++) {
        uint32_t x=ACTOR0+p*ACTOR_SIZE;
        if(saved_record[p][0]) { psx_mod_write_word(x+0x54,saved_record[p][0]); psx_mod_write_word(x+0x4c,saved_record[p][1]); }
    }
    playing=0;
    tekken3_ttt1_embu_effects_reset();
    fprintf(stderr,"Cine: \"%s\" over after %d frames\n",cine->name,tick-2-preroll);
    void (*then)(void)=done; done=NULL;
    if(then) { then(); return; }
    /* Nobody takes over (a test in a fight): the fight goes on where it was. */
    psx_mod_write_half(PHASE,(uint16_t)resume.phase);
    for(unsigned p=0;p<2;p++)
        for(unsigned k=0;k<0x30;k+=4) psx_mod_write_word(ACTOR0+p*ACTOR_SIZE+k,resume.actor[p][k/4]);
    for(unsigned i=0;i<4;i++) psx_mod_write_word(0x8009e73cu+i*16,resume.fill[i]);
    psx_mod_write_byte(0x800afaa0,resume.dark);
    for(unsigned k=0;k<CAM_MODE_SIZE;k+=4) psx_mod_write_word(CAM_MODE+k,resume.mode[k/4]);
    for(unsigned k=0;k<CAM_OBJECT_SIZE;k+=4) psx_mod_write_word(CAM_OBJECT+k,resume.object[k/4]);
}

/* The phase machine draws a phase's primitives from its display buffer's own
 * base (0x800AE040 + 28 * buffer + 24, buffer 0x800ADEFC) and gives the
 * pointer 0x800ADF64 back as it was afterwards: the engine's frame too. */
static uint32_t caller_prims;
extern void __real_func_80050450(CPUState *cpu);

/* A test: TEKKEN3_CINE_HOLD="f1,f2,..." holds each of these frames of the
 * cinematic HOLD_TICKS game frames ("Cine: hold frame N" in the log), long
 * enough for a bench to take its screenshot at that exact frame (a side by
 * side with the PS2's, tools/ps2/ttt_compare_shots.py). */
enum { HOLD_TICKS=60, HOLDS_MAX=256 };
static int held(void) {
    static int parsed,frames[HOLDS_MAX],count,next,left;
    if(!parsed) {
        parsed=1;
        const char *e=getenv("TEKKEN3_CINE_HOLD");
        for(const char *c=e;c && *c && count<HOLDS_MAX;) {
            frames[count++]=atoi(c);
            c=strchr(c,','); if(c) c++;
        }
    }
    if(left) { left--; return 1; }
    int f=tekken3_cine_engine_frame();
    while(next<count && frames[next]<f) next++;
    if(next<count && frames[next]==f) {
        next++; left=HOLD_TICKS-1;
        fprintf(stderr,"Cine: hold frame %d\n",f); fflush(stderr);
        return 1;
    }
    return 0;
}
void __wrap_func_80050450(CPUState *cpu) {
    if(playing && (cpu->pc==0 || cpu->pc==ENTRY) && (line || !cpu->gpr[2])) {
        if(!line) {                 /* the phase machine's turn, not paused: a frame begins */
            if(psx_mod_read_word(STATE)!=8) { playing=0; fprintf(stderr,"Cine: the fight ended under it\n"); }
            else {
                cpu->gpr[29]-=FRAME; psx_mod_write_half(PHASE,17);
                caller_prims=psx_mod_read_word(0x800adf64);
                psx_mod_write_word(0x800adf64,psx_mod_read_word(0x800ae040+28*psx_mod_read_word(0x800adefc)+24));
            }
        }
        if(playing) {
            if(!run(cpu)) return;   /* gone to the game, back here after */
            psx_mod_write_word(0x800adf64,caller_prims);
            cpu->gpr[29]+=FRAME;
            if(!held()) tick++;
            if(tick-2-preroll>=cine->length) finish();
            cpu->gpr[2]=0; cpu->pc=EPILOGUE;
            return;
        }
    }
    __real_func_80050450(cpu);
}

/* ---- The recorder -------------------------------------------------------- */

/* TEKKEN3_CINE_RECORD=file: what a cinematic shows, frame by frame, the old
 * way's as the engine's, to compare them (tools/cine_compare.py). Records of
 * int32 frame, int32 kind, then for an actor (kind 0, 1) its skeleton's root
 * (actor + 0xF54, 32 bytes), its 18 bones' local matrices (+0xF74, 576) and
 * its position (+0x00, 16); for the camera (kind -1) the eye and target given
 * to 0x80063EEC (6 int32). All of them each time the view is built: after
 * both actors' animation step, in the old way's frame as in the engine's. */
extern int tekken3_cine_frame(void);
static FILE *recording;
static FILE *recorder(void) {
    static int tried;
    if(!tried) {
        tried=1;
        const char *e=getenv("TEKKEN3_CINE_RECORD");
        if(e && (recording=fopen(e,"wb"))) fprintf(stderr,"Cine: recording to %s\n",e);
    }
    return tekken3_cine_frame()>=0?recording:NULL;
}
void tekken3_cine_record_camera(const int32_t view[6]) {
    FILE *f=recorder();
    if(!f) return;
    int32_t head[2]={tekken3_cine_frame(),-1};
    fwrite(head,4,2,f); fwrite(view,4,6,f);
    /* Both actors as their animation step left them this frame. */
    for(unsigned p=0;p<2;p++) {
        uint32_t actor=ACTOR0+p*ACTOR_SIZE;
        uint8_t body[32+576+16];
        for(unsigned k=0;k<32+576;k++) body[k]=psx_mod_read_byte(actor+0xf54+k);
        for(unsigned k=0;k<16;k++) body[608+k]=psx_mod_read_byte(actor+k);
        head[1]=(int32_t)p;
        fwrite(head,4,2,f); fwrite(body,1,sizeof body,f);
    }
    fflush(f);
}

/* ---- The API ------------------------------------------------------------- */

/* The prepared cinematic's scene, for its loading (src/tekken3_cine_scene.c):
 * arena, dark look and the played actors' characters and costumes (a single
 * actor stands for both). Returns the number of actors, 0 if none prepared. */
int tekken3_cine_scene_of(int *arena,int *dark,char character[PLAYED][32],int costume[PLAYED]) {
    if(!cine) return 0;
    *arena=cine->arena; *dark=cine->dark;
    for(unsigned p=0;p<PLAYED;p++) {
        const Actor *a=&cine->actors[p<(unsigned)cine->actor_count?p:0];
        snprintf(character[p],32,"%s",a->character); costume[p]=a->costume;
    }
    return cine->actor_count;
}

/* Reads a cinematic ahead of its playing (a player reloaded for it takes its
 * first model files); 1 if it can be played. */
int tekken3_cine_prepare(const char *path) {
    if(playing) return 0;
    Cine *c=cine_read(path);
    if(!c) return 0;
    cine_free(cine); cine=c;
    free(music); music=NULL; music_frames=music_pos=0; music_from=-2;
    return 1;
}
/* Plays the prepared cinematic (or `path`) in the running fight, after
 * `pre` black frames (actors still settling), then calls `on_done`. */
int tekken3_cine_play(const char *path,int pre,void (*on_done)(void)) {
    if(playing || (path && !tekken3_cine_prepare(path)) || !cine) return 0;
    if(psx_mod_read_word(STATE)!=8) { fprintf(stderr,"Cine: not in a fight\n"); return 0; }
    if(!tiles && !(tiles=psx_mod_alloc_gpu_dma_memory(32,16))) return 0;
    playing=1; tick=0; preroll=pre<0?0:pre; done=on_done;
    /* What a fight needs back when nobody takes over at the end (finish). */
    resume.phase=psx_mod_read_half(PHASE);
    for(unsigned p=0;p<2;p++)
        for(unsigned k=0;k<0x30;k+=4) resume.actor[p][k/4]=psx_mod_read_word(ACTOR0+p*ACTOR_SIZE+k);
    for(unsigned i=0;i<4;i++) resume.fill[i]=psx_mod_read_word(0x8009e73cu+i*16);
    resume.dark=psx_mod_read_byte(0x800afaa0);
    for(unsigned k=0;k<CAM_MODE_SIZE;k+=4) resume.mode[k/4]=psx_mod_read_word(CAM_MODE+k);
    for(unsigned k=0;k<CAM_OBJECT_SIZE;k+=4) resume.object[k/4]=psx_mod_read_word(CAM_OBJECT+k);
    tekken3_ttt1_embu_effects_reset();         /* its own lightning packs */
    psx_mod_write_half(PHASE,17);
    fprintf(stderr,"Cine: \"%s\" plays, %d frames after %d of pre-roll\n",cine->name,cine->length,preroll);
    return 1;
}

/* A test: TEKKEN3_CINE_TEST="file.cine@frames" plays it in any fight, that
 * many frames into its first round, with the fighters there. */
void tekken3_cine_test_tick(void) {
    static int parsed,delay=-1,count;
    static char path[1024];
    if(!parsed) {
        parsed=1;
        const char *e=getenv("TEKKEN3_CINE_TEST"),*at=e?strrchr(e,'@'):NULL;
        if(!at) return;
        snprintf(path,sizeof path,"%.*s",(int)(at-e),e);
        delay=atoi(at+1);
    }
    if(delay<0 || playing) return;
    if(psx_mod_read_word(STATE)!=8 || psx_mod_read_half(PHASE)!=8) { count=0; return; }
    if(++count==delay) { tekken3_cine_play(path,0,NULL); delay=-1; }
}
