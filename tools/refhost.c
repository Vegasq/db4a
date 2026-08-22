/* Headless libretro host: runs a reference core for N frames and dumps the
 * framebuffer as PPM. Used purely as a development oracle for comparing
 * against the recompiled build -- it is never linked into db4a itself.
 *
 *   refhost <core.so> <rom> <frames> [ppm-out] [--hash]
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "libretro.h"

static void *CORE;
static uint8_t FB[512][512][3];
static unsigned FBW, FBH;
static int have_frame;
/* Whatever format the core actually chose. GPGX asks for RGB565 and keeps
   using it even if the host refuses, so we must honour its choice rather
   than assume ours. Misreading 565 as 8888 consumes two pixels per read,
   which halves the apparent width and scrambles the channels. */
static enum retro_pixel_format PIXFMT = RETRO_PIXEL_FORMAT_0RGB1555;

/* dlsym returns void*; assigning it to a function pointer needs the cast to
   go through a data pointer, which is the usual POSIX-sanctioned dance. */
#define LOAD(var, name) \
    do { *(void **)(&(var)) = dlsym(CORE, name); \
         if (!(var)) { fprintf(stderr, "missing symbol %s\n", name); return 1; } } while (0)

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        PIXFMT = *(enum retro_pixel_format *)data;
        return true;                     /* we convert all three */
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;  return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = ".";  return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = (struct retro_variable *)data;
        v->value = NULL;  return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        return false;
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;                     /* duped frame */
    FBW = w; FBH = h; have_frame = 1;
    const uint8_t *base = (const uint8_t *)data;
    for (unsigned y = 0; y < h && y < 512; y++) {
        const uint8_t *row = base + y * pitch;
        for (unsigned x = 0; x < w && x < 512; x++) {
            uint8_t r, g, b;
            if (PIXFMT == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t p = ((const uint32_t *)row)[x];
                r = (uint8_t)(p >> 16); g = (uint8_t)(p >> 8); b = (uint8_t)p;
            } else if (PIXFMT == RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t p = ((const uint16_t *)row)[x];
                r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
                g = (uint8_t)(((p >>  5) & 0x3F) * 255 / 63);
                b = (uint8_t)(( p        & 0x1F) * 255 / 31);
            } else {                       /* 0RGB1555 */
                uint16_t p = ((const uint16_t *)row)[x];
                r = (uint8_t)(((p >> 10) & 0x1F) * 255 / 31);
                g = (uint8_t)(((p >>  5) & 0x1F) * 255 / 31);
                b = (uint8_t)(( p        & 0x1F) * 255 / 31);
            }
            FB[y][x][0] = r; FB[y][x][1] = g; FB[y][x][2] = b;
        }
    }
}
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t f) { (void)d; return f; }
/* Scripted input, same "frame:button" syntax as the native build's
   DB4A_PRESS, so both sides can be driven identically. Comparing anything
   past the title screen is meaningless unless the reference receives the
   same button presses at the same frames. */
#define MAXSCRIPT 32
static struct { unsigned at; unsigned id; } script[MAXSCRIPT];
static unsigned nscript, cur_frame;
static const unsigned HOLD = 8;

static void parse_script(const char *sp) {
    if (!sp) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", sp);
    for (char *tok = strtok(buf, ","); tok && nscript < MAXSCRIPT; tok = strtok(NULL, ",")) {
        char name[32]; unsigned at;
        if (sscanf(tok, "%u:%31s", &at, name) != 2) continue;
        int id = -1;
        if      (!strcmp(name, "start")) id = RETRO_DEVICE_ID_JOYPAD_START;
        else if (!strcmp(name, "a"))     id = RETRO_DEVICE_ID_JOYPAD_Y;
        else if (!strcmp(name, "b"))     id = RETRO_DEVICE_ID_JOYPAD_B;
        else if (!strcmp(name, "c"))     id = RETRO_DEVICE_ID_JOYPAD_A;
        else if (!strcmp(name, "up"))    id = RETRO_DEVICE_ID_JOYPAD_UP;
        else if (!strcmp(name, "down"))  id = RETRO_DEVICE_ID_JOYPAD_DOWN;
        else if (!strcmp(name, "left"))  id = RETRO_DEVICE_ID_JOYPAD_LEFT;
        else if (!strcmp(name, "right")) id = RETRO_DEVICE_ID_JOYPAD_RIGHT;
        if (id >= 0) { script[nscript].at = at; script[nscript].id = (unsigned)id; nscript++; }
    }
}

/* Replay a db4a recording (DB4A_REPLAY), the same file the native build
   consumes. A recording carries exact press AND release frames, so unlike the
   scripted form there is no HOLD approximation and both sides hold every
   button for precisely the same frames. That is what makes a real playthrough
   usable as an oracle comparison rather than only a crash test. */
#define MAXREPLAY 8192
static struct { unsigned at; unsigned id; unsigned char down; } rep[MAXREPLAY];
static unsigned nrep, rep_next;
static unsigned char rep_held[16];

static int button_id(const char *name) {
    if (!strcmp(name, "start")) return RETRO_DEVICE_ID_JOYPAD_START;
    if (!strcmp(name, "a"))     return RETRO_DEVICE_ID_JOYPAD_Y;
    if (!strcmp(name, "b"))     return RETRO_DEVICE_ID_JOYPAD_B;
    if (!strcmp(name, "c"))     return RETRO_DEVICE_ID_JOYPAD_A;
    if (!strcmp(name, "up"))    return RETRO_DEVICE_ID_JOYPAD_UP;
    if (!strcmp(name, "down"))  return RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (!strcmp(name, "left"))  return RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (!strcmp(name, "right")) return RETRO_DEVICE_ID_JOYPAD_RIGHT;
    return -1;
}

static void load_replay(const char *path) {
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); return; }
    char line[256];
    while (fgets(line, sizeof line, f) && nrep < MAXREPLAY) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned at, dn; char name[32];
        if (sscanf(line, "%u %31s %u", &at, name, &dn) != 3) continue;
        int id = button_id(name);
        if (id < 0) { fprintf(stderr, "replay: unknown button '%s'\n", name); continue; }
        rep[nrep].at = at; rep[nrep].id = (unsigned)id;
        rep[nrep].down = (unsigned char)(dn != 0);
        nrep++;
    }
    fclose(f);
    printf("replaying %u input events from %s%s\n", nrep, path,
           nrep == MAXREPLAY ? " (TRUNCATED)" : "");
}

/* Apply every event scheduled for this frame. Events are in frame order in the
   file, so a single advancing cursor is enough. */
static void replay_frame(unsigned frame) {
    while (rep_next < nrep && rep[rep_next].at <= frame) {
        rep_held[rep[rep_next].id & 15] = rep[rep_next].down;
        rep_next++;
    }
}

static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) {
    (void)dev; (void)idx;
    if (port != 0) return 0;
    if (nrep) return rep_held[id & 15] ? 1 : 0;
    for (unsigned i = 0; i < nscript; i++)
        if (script[i].id == id && cur_frame >= script[i].at && cur_frame < script[i].at + HOLD)
            return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <core.so> <rom> <frames> [out.ppm]\n", argv[0]);
        return 2;
    }
    unsigned frames = (unsigned)strtoul(argv[3], NULL, 0);

    CORE = dlopen(argv[1], RTLD_NOW);
    if (!CORE) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    void  *(*retro_get_memory_data)(unsigned);
    size_t (*retro_get_memory_size)(unsigned);
    void (*retro_init)(void);
    bool (*retro_load_game)(const struct retro_game_info *);
    void (*retro_run)(void);
    void (*retro_get_system_av_info)(struct retro_system_av_info *);

    LOAD(retro_set_environment,        "retro_set_environment");
    LOAD(retro_set_video_refresh,      "retro_set_video_refresh");
    LOAD(retro_set_audio_sample,       "retro_set_audio_sample");
    LOAD(retro_set_audio_sample_batch, "retro_set_audio_sample_batch");
    LOAD(retro_set_input_poll,         "retro_set_input_poll");
    LOAD(retro_set_input_state,        "retro_set_input_state");
    LOAD(retro_init,                   "retro_init");
    LOAD(retro_load_game,              "retro_load_game");
    LOAD(retro_run,                    "retro_run");
    LOAD(retro_get_system_av_info,     "retro_get_system_av_info");
    LOAD(retro_get_memory_data,        "retro_get_memory_data");
    LOAD(retro_get_memory_size,        "retro_get_memory_size");

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *rom = malloc((size_t)n);
    if (fread(rom, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);

    struct retro_game_info gi;
    memset(&gi, 0, sizeof gi);
    gi.path = argv[2]; gi.data = rom; gi.size = (size_t)n;
    if (!retro_load_game(&gi)) { fprintf(stderr, "load_game failed\n"); return 1; }

    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    printf("reference core: %ux%u, %.2f fps\n",
           av.geometry.base_width, av.geometry.base_height, av.timing.fps);

    load_replay(getenv("DB4A_REPLAY"));
    if (!nrep) {
        parse_script(getenv("DB4A_PRESS"));
        if (nscript) printf("input script: %u events\n", nscript);
    }
    /* DB4A_SHOTS="100,200" dumps those frames to <out>.<frame>.ppm during the
       run, matching the native harness. Without it only the final frame is
       written. Capturing several frames from ONE run matters: each run costs a
       full re-simulation, so comparing a span of reference frames against one
       of ours was otherwise prohibitively slow. */
    unsigned shots[64], nshots = 0;
    const char *sp = getenv("DB4A_SHOTS");
    if (sp) {
        char sb[256]; snprintf(sb, sizeof sb, "%s", sp);
        for (char *t = strtok(sb, ","); t && nshots < 64; t = strtok(NULL, ","))
            shots[nshots++] = (unsigned)strtoul(t, NULL, 0);
    }
    for (unsigned i = 0; i < frames; i++) {
        cur_frame = i;
        if (nrep) replay_frame(i);
        retro_run();
        for (unsigned k = 0; k < nshots; k++)
            if (shots[k] == i && have_frame && argc > 4) {
                char path[512];
                snprintf(path, sizeof path, "%s.%u.ppm", argv[4], i);
                FILE *o = fopen(path, "wb");
                if (o) {
                    fprintf(o, "P6\n%u %u\n255\n", FBW, FBH);
                    for (unsigned y = 0; y < FBH; y++) fwrite(FB[y], 1, FBW * 3, o);
                    fclose(o);
                    printf("  [frame %u] wrote %s\n", i, path);
                }
                /* 68K work RAM alongside the frame. Comparing RAM tells a
                   rendering difference apart from a game-logic one, which
                   pixels alone cannot: if the two sides agree on RAM but
                   disagree on screen it is ours to fix in the renderer, and
                   if RAM already differs the divergence happened earlier. */
                if (getenv("DB4A_RAMDUMP")) {
                    void *rd = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
                    size_t rs = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
                    if (rd && rs) {
                        char rp[512];
                        snprintf(rp, sizeof rp, "%s.%u.ram", argv[4], i);
                        FILE *r = fopen(rp, "wb");
                        if (r) {
                            fwrite(rd, 1, rs, r);
                            fclose(r);
                            printf("  [frame %u] wrote %s (%zu bytes)\n", i, rp, rs);
                        }
                    }
                }
            }
    }
    printf("ran %u frames, last frame %ux%u, pixel format %s\n", frames, FBW, FBH,
           PIXFMT == RETRO_PIXEL_FORMAT_XRGB8888 ? "XRGB8888" :
           PIXFMT == RETRO_PIXEL_FORMAT_RGB565   ? "RGB565"   : "0RGB1555");

    if (argc > 4 && have_frame) {
        FILE *o = fopen(argv[4], "wb");
        fprintf(o, "P6\n%u %u\n255\n", FBW, FBH);
        for (unsigned y = 0; y < FBH; y++)
            fwrite(FB[y], 1, FBW * 3, o);
        fclose(o);
        printf("wrote %s\n", argv[4]);
    }
    return 0;
}
