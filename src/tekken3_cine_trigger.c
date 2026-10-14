/* The cinematic engine's triggers: a table of events, each playing a .cine
 * file, read from mods/ttt-cinematics/triggers.txt (written by
 * tools/ttt_cinematics.py; docs/CINE-FORMAT.md, "Déclencheurs"). No
 * environment variables: everything an event plays with is in its line and
 * in the .cine itself (arena, dark look, actors), explicit per cinematic,
 * never taken from the fight it interrupts.
 *
 *   triggers 1
 *   arcade-end p1=kazuya cine=kazuya.cine then=credits
 *
 * Played today: arcade-end (player 1 = that character, any costume, wins
 * the Arcade's last stage: the cinematic in place of the ending movie, then
 * the credits). The others are read and kept, not played yet (a message):
 * boss (before a boss fight), mode-start / mode-end, attract, theater.
 *
 * The engine loads the scene itself (src/tekken3_cine_scene.c,
 * tekken3_cine_play_scene: arena and actors from the .cine, then it puts
 * back the mode, fighters and arena it found); this file only chooses the
 * cinematic and what comes after it. */
#include "mod_plugins.h"
#include "psx_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TRIGGERS_MAX=32, CINE_SCENE_BLACK=1 };
typedef enum { EV_ARCADE_END, EV_BOSS, EV_MODE_START, EV_MODE_END, EV_ATTRACT, EV_THEATER, EV_COUNT } Event;
static const char *const event_names[EV_COUNT]={"arcade-end","boss","mode-start","mode-end","attract","theater"};
typedef enum { THEN_CREDITS, THEN_ATTRACT } Then;
typedef struct {
    Event event;
    char p1[32];            /* character key or ID, "" any */
    int p1_id;              /* resolved, -2 not yet, -1 any */
    char mode[16];          /* reserved events: arcade, survival… */
    int stage, slot;        /* reserved events: boss stage, attract / theater slot */
    char cine[1024];        /* full path */
    Then then;
} Trigger;
static Trigger triggers[TRIGGERS_MAX];
static int trigger_count;

extern int tekken3_guest_id(const char *key);
extern int tekken3_t3_key_id(const char *key);
static int character(const char *token) {
    if(*token>='0' && *token<='9') return atoi(token);
    int id=tekken3_guest_id(token);
    return id>=0?id:tekken3_t3_key_id(token);
}

/* ---- The table ----------------------------------------------------------- */

/* Reads `dir`/triggers.txt; the number of triggers, 0 without the file. */
int tekken3_cine_triggers_load(const char *dir) {
    char path[4200],line[2048];
    snprintf(path,sizeof path,"%s/triggers.txt",dir);
    FILE *f=fopen(path,"r");
    if(!f) return 0;
    int version=0,lineno=0;
    while(fgets(line,sizeof line,f)) {
        lineno++;
        char *hash=strchr(line,'#'); if(hash) *hash=0;
        char *w[16]; int n=0;
        for(char *s=strtok(line," \t\r\n");s && n<16;s=strtok(NULL," \t\r\n")) w[n++]=s;
        if(!n) continue;
        if(!strcmp(w[0],"triggers") && n==2) { version=atoi(w[1]); continue; }
        if(version!=1) { fprintf(stderr,"Cine triggers: %s does not start with \"triggers 1\"\n",path); break; }
        int e=0;
        while(e<EV_COUNT && strcmp(w[0],event_names[e])) e++;
        if(e==EV_COUNT) { fprintf(stderr,"Cine triggers: %s line %d: event \"%s\" ignored\n",path,lineno,w[0]); continue; }
        if(trigger_count==TRIGGERS_MAX) { fprintf(stderr,"Cine triggers: more than %d, line %d on ignored\n",TRIGGERS_MAX,lineno); break; }
        Trigger *t=&triggers[trigger_count];
        memset(t,0,sizeof *t);
        t->event=(Event)e; t->p1_id=-2; t->then=e==EV_ARCADE_END?THEN_CREDITS:THEN_ATTRACT;
        int ok=1;
        for(int k=1;k<n;k++) {
            char *eq=strchr(w[k],'=');
            if(!eq) { ok=0; break; }
            *eq=0; const char *v=eq+1;
            if(!strcmp(w[k],"p1")) snprintf(t->p1,sizeof t->p1,"%s",strcmp(v,"*")?v:"");
            else if(!strcmp(w[k],"cine")) snprintf(t->cine,sizeof t->cine,"%s/%s",dir,v);
            else if(!strcmp(w[k],"then")) {
                if(!strcmp(v,"credits")) t->then=THEN_CREDITS;
                else if(!strcmp(v,"attract")) t->then=THEN_ATTRACT;
                else ok=0;
            }
            else if(!strcmp(w[k],"mode")) snprintf(t->mode,sizeof t->mode,"%s",v);
            else if(!strcmp(w[k],"stage")) t->stage=atoi(v);
            else if(!strcmp(w[k],"slot")) t->slot=atoi(v);
            else fprintf(stderr,"Cine triggers: %s line %d: \"%s\" ignored\n",path,lineno,w[k]);
        }
        if(!ok || !t->cine[0]) { fprintf(stderr,"Cine triggers: %s line %d: bad trigger\n",path,lineno); continue; }
        if(!t->p1[0]) t->p1_id=-1;
        if(t->event!=EV_ARCADE_END)
            fprintf(stderr,"Cine triggers: line %d, \"%s\": not played yet\n",lineno,event_names[t->event]);
        trigger_count++;
    }
    fclose(f);
    fprintf(stderr,"Cine triggers: %s, %d triggers\n",path,trigger_count);
    return trigger_count;
}

/* The first trigger of `event` whose player 1 is `p1` (keys resolved once
 * the roster is read), -1 for none. */
static int find(Event event,unsigned p1) {
    for(int i=0;i<trigger_count;i++) {
        Trigger *t=&triggers[i];
        if(t->event!=event) continue;
        if(t->p1_id==-2 && (t->p1_id=character(t->p1))<0) { t->p1_id=-2; continue; }   /* not known yet */
        if(t->p1_id<0 || t->p1_id==(int)p1) return i;
    }
    return -1;
}

/* ---- What comes after ---------------------------------------------------- */

extern int tekken3_cine_prepare(const char *path);
extern int tekken3_cine_play_scene(const char *path,int flags,void (*on_done)(void));

static const Trigger *active;       /* from the trigger to the end of what follows */
static int credits;                 /* 1: on the way to the credits */

/* The Arcade's ending goes on as after its movie: state 18 towards 19, the
 * ending state; its phase 2 would play the movie (0x800FDC78, its `jal` at
 * 0x800FF2CC in the ending overlay): that call is dropped (credits_tick),
 * then put back once the credits run. The mode is the Arcade's again (the
 * scene put it back). */
static void to_credits(void) {
    fprintf(stderr,"Cine trigger: \"%s\" over, the credits\n",active?active->cine:"?");
    credits=1;
    psx_mod_write_byte(0x80097f38,19); psx_mod_write_byte(0x80097f39,8);
    psx_mod_write_half(0x800ae224,0); psx_mod_write_half(0x800ae204,18);
}
static void credits_tick(unsigned state) {
    if(!credits || state!=19) return;
    unsigned phase=psx_mod_read_half(0x800ae224);
    if(phase<=2 && psx_mod_read_word(0x800ff2cc)==0x0c03f71eu) {
        psx_mod_write_code_word(0x800ff2cc,0);
        fprintf(stderr,"Cine trigger: no ending movie\n");
    }
    if(phase>=3) {
        if(!psx_mod_read_word(0x800ff2cc)) psx_mod_write_code_word(0x800ff2cc,0x0c03f71eu);
        credits=0; active=NULL;
        fprintf(stderr,"Cine trigger: credits, all back to normal\n");
    }
}
/* The attract, as a demo fight ends (what the scene does without on_done). */
static void to_attract(void) {
    active=NULL;
    psx_mod_write_byte(0x80097f38,3); psx_mod_write_byte(0x80097f39,8);
    psx_mod_write_half(0x800ae224,0); psx_mod_write_half(0x800ae204,2);
}

/* The wrapper of 0x8004FF74 (state 18, its target at 0x80097F38) asks: the
 * Arcade's own ending (target 19, mode 0, stage 10 cleared, the Arcade
 * overlay), player 1 with a trigger. 1 if taken: the handler is skipped. */
int tekken3_cine_trigger_arcade_end(void) {
    if(!trigger_count || active || psx_mod_read_byte(0x80097f38)!=19 || psx_mod_read_byte(0x800afa88)!=0 ||
       psx_mod_read_byte(0x800afaac)!=10 || psx_mod_read_word(0x800b2870)!=0x24020014u) return 0;
    unsigned p1=psx_mod_read_half(0x800add5c);
    int i=find(EV_ARCADE_END,p1);
    if(i<0) return 0;
    const Trigger *t=&triggers[i];
    if(!tekken3_cine_prepare(t->cine)) { fprintf(stderr,"Cine trigger: %s cannot be played, the ending movie\n",t->cine); return 0; }
    active=t;
    /* No loading screen: the last fight's "YOU WIN" goes to black. */
    if(!tekken3_cine_play_scene(NULL,CINE_SCENE_BLACK,t->then==THEN_CREDITS?to_credits:to_attract)) {
        fprintf(stderr,"Cine trigger: %s could not load its scene, the ending movie\n",t->cine);
        active=NULL; return 0;
    }
    fprintf(stderr,"Cine trigger: arcade-end, player 1 %u: %s\n",p1,t->cine);
    return 1;
}

void tekken3_cine_trigger_tick(unsigned state) { credits_tick(state); }
