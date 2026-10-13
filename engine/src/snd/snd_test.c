/* snd/snd_test.c — the audio selftest (M0 + WP5 checks): decode every NWA and OVK entry, resolve koe numbers,
 * check loop/crossfade/ducking/fade/volume rules on the null sink, and render a short mixed WAV.
 *
 * Threads: the OVK check decodes on worker threads (ovk_decode is thread-safe once ovk_parse ran on the main
 * thread, which ovk_load_files does first); the workers share only atomics and read-only job tables.
 * Memory: file data from fs_read (fs_free), indexes from ovk_parse (ovk_free), job tables in a scratch arena. */
#include "snd/audio.h"
#include "snd/snd_internal.h"
#include "core/mem.h"
#include "core/str.h"
#include "fmt/nwa.h"
#include "fmt/ovk.h"
#include "plat/fs.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>                                 /* abs */
#include <string.h>

enum { ST_THREADS_MAX = 16, ST_ERR_MAX = 256, ST_KOE_REF_MAX_LINES = 100000, ST_RATE = 22050 };
enum { ST_NWA_FILES = 47, ST_OVK_FILES = 134, ST_OVK_ENTRIES = 2607 };   /* this game's data (AST §2–3) */

typedef struct StCtx { int fails; } StCtx;

static void CORE_PRINTF(3, 4) st_check(StCtx *t, bool ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  %s ", ok ? "ok  " : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) t->fails++;
}

static void run_ms(int ms, int dt)
{
    for (int t = 0; t < ms; t += dt) audio_frame(dt);
}

static double dbfs(double rms) { return rms > 0 ? 20.0 * log10(rms) : -999.0; }

static bool has_prefix_fold(const char *name, const char *prefix)
{
    Str n = str_c(name), p = str_c(prefix);
    return n.n >= p.n && str_eq_fold(str_sub(n, 0, p.n), p);
}

/* "vfs:<sub>/<name>"; false if it does not fit. */
static bool vfs_path(StrBuf *b, const char *sub, const char *name)
{
    sb_reset(b);
    return sb_fmt(b, "vfs:%s/%s", sub, name);
}

/* ------------------------------------------------------------------------------------------------ 1. NWA */

/* AST §2.2: raw PCM, size == 0x2c + datasize, samplecount * bytes == datasize, frames * channels == samples. */
static bool nwa_file_ok(const uint8_t *data, size_t len, double *secs)
{
    NwaHeader h;
    NwaSound s;
    if (!data || !nwa_header(data, len, &h) || h.complevel != -1 || h.blocks != 0) return false;
    if (h.datasize < 0 || len != (size_t)NWA_HEADER_SIZE + (size_t)h.datasize) return false;
    if ((int64_t)h.samplecount * (h.bits / 8) != h.datasize || !nwa_decode(data, len, &s)) return false;
    bool ok = (int64_t)s.frames * s.channels == h.samplecount;
    *secs += (double)s.frames / s.rate;
    nwa_free(&s);
    return ok;
}

/* Checks every <sub>/ .nwa file; returns the file count. */
static int nwa_dir(const char *sub, int *ok, double *secs)
{
    char **m = NULL;
    int n = fs_list(sub, "nwa", &m);
    SB_ON_STACK(path, 1024);
    for (int i = 0; i < n; i++) {
        size_t len = 0;
        uint8_t *data = vfs_path(&path, sub, m[i]) ? fs_read(sb_cstr(&path), &len) : NULL;
        if (nwa_file_ok(data, len, secs)) (*ok)++;
        else printf("  bad NWA %s\n", sb_cstr(&path));
        fs_free(data);
    }
    fs_list_free(m, n);
    return n;
}

static void check_nwa(StCtx *t)
{
    int ok = 0;
    double secs = 0;
    int nb = nwa_dir("bgm", &ok, &secs), nw = nwa_dir("wav", &ok, &secs), total = nb + nw;
    st_check(t, ok == total && total == ST_NWA_FILES, "NWA %d/%d decoded (bgm %d, wav %d), %.1f s total", ok, total,
             nb, nw, secs);
}

/* ------------------------------------------------------------------------------------------------ 2. OVK */

typedef struct OvkJob { const uint8_t *data; size_t n; const OvkEntry *e; int file_no; } OvkJob;
typedef struct OvkFile { uint8_t *data; size_t n; OvkIndex idx; int file_no; } OvkFile;
typedef struct OvkWork {
    const OvkJob *jobs;
    int njobs;
    SDL_AtomicInt next, ok, bad;
    char first_err[ST_ERR_MAX];                     /* written once, by the worker that saw bad == 0 */
} OvkWork;

static bool decode_job(OvkWork *w, const OvkJob *j, int *ch, int *rate, int *frames)
{
    int16_t *pcm = ovk_decode(j->data, j->n, j->e, ch, rate, frames);
    bool ok = pcm && *ch == 1 && *rate == ST_RATE && (uint32_t)*frames == j->e->samples;
    fmt_pcm_free(pcm);
    return ok;
}

static int SDLCALL ovk_worker(void *ud)
{
    OvkWork *w = ud;
    for (int k = SDL_AddAtomicInt(&w->next, 1); k < w->njobs; k = SDL_AddAtomicInt(&w->next, 1)) {
        const OvkJob *j = &w->jobs[k];
        int ch = 0, rate = 0, frames = 0;
        if (decode_job(w, j, &ch, &rate, &frames)) { (void)SDL_AddAtomicInt(&w->ok, 1); continue; }
        if (SDL_AddAtomicInt(&w->bad, 1) != 0) continue;
        bool whole = str_fmt(w->first_err, sizeof w->first_err, "z%04d id %u: ch=%d rate=%d frames=%d header=%u",
                             j->file_no, j->e->id, ch, rate, frames, j->e->samples);
        (void)whole;                                /* a cut diagnostic is still useful */
    }
    return 0;
}

/* Reads and indexes koe/z*.ovk; returns the number of files (bad ones counted in *bad). */
static int ovk_load_files(Arena *a, OvkFile **out, int *bad)
{
    char **m = NULL;
    int n = fs_list("koe", "ovk", &m);
    OvkFile *f = ARENA_NEW_ARRAY(a, OvkFile, (size_t)(n > 0 ? n : 1));
    SB_ON_STACK(path, 1024);
    int nf = 0;
    for (int i = 0; i < n; i++) {
        if (!has_prefix_fold(m[i], "z")) continue;
        OvkFile *x = &f[nf++];
        size_t pos = 1;
        int32_t no = -1;
        if (!str_scan_i32(str_c(m[i]), &pos, &no)) no = -1;
        x->file_no = no;
        x->data = vfs_path(&path, "koe", m[i]) ? fs_read(sb_cstr(&path), &x->n) : NULL;
        if (!x->data || no < 0 || !ovk_parse(x->data, x->n, &x->idx)) (*bad)++;
    }
    fs_list_free(m, n);
    *out = f;
    return nf;
}

static OvkJob *ovk_jobs(Arena *a, const OvkFile *f, int nf, int *njobs)
{
    size_t total = 0;
    for (int i = 0; i < nf; i++) total += (size_t)f[i].idx.n;
    OvkJob *jobs = ARENA_NEW_ARRAY(a, OvkJob, total > 0 ? total : 1);
    size_t k = 0;
    for (int i = 0; i < nf; i++)
        for (int e = 0; e < f[i].idx.n; e++) jobs[k++] = (OvkJob){ f[i].data, f[i].n, &f[i].idx.e[e], f[i].file_no };
    *njobs = (int)total;
    return jobs;
}

/* Runs the workers; returns the thread count used. */
static int ovk_run(OvkWork *w)
{
    int nth = SDL_GetNumLogicalCPUCores();
    nth = nth < 1 ? 1 : nth > ST_THREADS_MAX ? ST_THREADS_MAX : nth;
    SDL_Thread *th[ST_THREADS_MAX];
    for (int t = 0; t < nth; t++) th[t] = SDL_CreateThread(ovk_worker, "ovk", w);
    for (int t = 0; t < nth; t++) {
        if (th[t]) SDL_WaitThread(th[t], NULL);
        else (void)ovk_worker(w);                   /* no thread: do the remaining jobs here */
    }
    return nth;
}

static void check_ovk(StCtx *t)
{
    Scratch s = mem_scratch_begin(NULL);
    OvkFile *f = NULL;
    int bad_files = 0, nf = ovk_load_files(s.arena, &f, &bad_files);
    OvkWork *w = ARENA_NEW(s.arena, OvkWork);
    w->jobs = ovk_jobs(s.arena, f, nf, &w->njobs);
    Uint64 t0 = SDL_GetTicks();
    int nth = ovk_run(w);
    Uint64 t1 = SDL_GetTicks();
    int ok = SDL_GetAtomicInt(&w->ok), bad = SDL_GetAtomicInt(&w->bad);
    st_check(t, nf == ST_OVK_FILES && bad_files == 0 && w->njobs == ST_OVK_ENTRIES && ok == w->njobs,
             "OVK %d files, %d entries, %d decoded with frames == header (%d bad) in %.1f s on %d threads", nf,
             w->njobs, ok, bad, (double)(t1 - t0) / 1000.0, nth);
    if (bad) printf("       first error: %s\n", w->first_err);
    for (int i = 0; i < nf; i++) { fs_free(f[i].data); ovk_free(&f[i].idx); }
    mem_scratch_end(&s);
}

/* ------------------------------------------------------------------------------------------------ 3. koe */

/* koe number of one koe_ref line: the last tab-separated column. False for comments and blank lines. */
static bool koe_ref_line(Str line, int32_t *no)
{
    line = str_trim(line);
    if (!line.n || line.p[0] == '#') return false;
    size_t tab;
    Str col = str_rfind_char(line, '\t', &tab) ? str_sub(line, tab + 1, line.n) : line;
    if (!str_to_i32(str_trim(col), no)) *no = -1;  /* counted as unresolved */
    return true;
}

static void check_koe_ref(StCtx *t, const char *koe_ref)
{
    size_t len = 0;
    uint8_t *txt = fs_read(koe_ref, &len);
    Str rest = str_make((const char *)txt, txt ? len : 0), line;
    int total = 0, okc = 0;
    for (int guard = 0; guard < ST_KOE_REF_MAX_LINES && str_split(&rest, '\n', &line); guard++) {
        int32_t no;
        if (!koe_ref_line(line, &no)) continue;
        total++;
        if (snd_koe_exists(no)) okc++;
        else if (total - okc <= 5) printf("       unresolved koe %d\n", (int)no);
    }
    fs_free(txt);
    st_check(t, total > 0 && okc == total, "static koe numbers resolved: %d/%d (%s)", okc, total, koe_ref);
}

/* z0050 id 37 (M0), then the optional list of every static koe number. */
static void check_koe(StCtx *t, const char *koe_ref)
{
    MixSound *s = snd_load_koe(5000037);
    st_check(t, s && s->frames == 81920, "koe 5000037 -> z0050.ovk id 37: %u frames (expect 81920)",
             s ? s->frames : 0);
    snd_sound_unref(s);
    if (koe_ref) check_koe_ref(t, koe_ref);
}

/* ------------------------------------------------------------------------------------------------ 4. behaviour */

/* Without Gameexe: the documented table (AST §2.3). */
static void use_builtin_bgm_table(bool note)
{
    if (bgm_table_count()) return;
    (void)bgm_table_add("RAIN", "RAIN", 0, 99999999, 80000);   /* fits: the table is empty */
    for (int i = 0; i <= 13; i++) {
        char b[16];
        if (str_fmt(b, sizeof b, "BGM%02d", i)) (void)bgm_table_add(b, b, 0, 99999999, 0);
    }
    if (note) printf("  note: #BGM table not available from gameexe; using the AST §2.3 table\n");
}

static uint32_t nwa_frames(const char *sub, const char *name)
{
    MixSound *probe = snd_load_nwa(sub, name);
    uint32_t frames = probe ? probe->frames : 0;
    snd_sound_unref(probe);
    return frames;
}

static uint32_t bgm_pos(void) { return snd_bgm_voice() >= 0 ? mixer_pos(snd_bgm_voice()) : 0; }

/* Loop point: start 50 ms before EOF; 100 ms later the position is ~50 ms past repeat (80000). Replaying the
 * looping track does not restart it. */
static void check_loop(StCtx *t, const WStr *rain)
{
    uint32_t frames = nwa_frames("bgm", "RAIN");
    bgm_play(rain, true, false, 0, 0, (int)frames - ST_RATE / 20, 0);
    run_ms(100, 5);
    uint32_t pos = bgm_pos();
    int expect = 80000 + ST_RATE / 20;
    st_check(t, bgm_playing() && abs((int)pos - expect) < 60, "RAIN loop: frames=%u, pos after wrap %u (expect ~%d)",
             frames, pos, expect);
    int voice = snd_bgm_voice();
    bgm_play(rain, true, false, 0, 0, -1, 0);
    run_ms(20, 5);
    uint32_t pos2 = bgm_pos();
    st_check(t, snd_bgm_voice() == voice && pos2 > pos, "no restart of the same looping BGM (pos %u -> %u)", pos,
             pos2);
}

/* Crossfade: the new track on the other stream, the old one fades out. */
static void check_crossfade(StCtx *t)
{
    int old = snd_bgm_voice();
    WStr b1 = {0};
    ws_set_utf8(&b1, "BGM01");
    bgm_play(&b1, true, false, 500, 500, -1, 0);
    bool both = snd_stream_active(MIX_BGM0) && snd_stream_active(MIX_BGM1);
    run_ms(510, 5);
    st_check(t, both && snd_bgm_voice() != old && !snd_stream_active(old) && bgm_playing(),
             "crossfade 500/500: both streams during the fade, old stream gone after it");
    ws_free(&b1);
}

/* Ducking: ramp to 192 over 1000 ms while a voice plays, hold 2000 ms after it, back to 255 by 3000 ms. */
static void check_ducking(StCtx *t)
{
    (void)koe_play(5000037, -1, 100);               /* checked by check_koe */
    run_ms(500, 5);
    int d500 = snd_duck_level();
    run_ms(600, 5);
    int d1100 = snd_duck_level();
    koe_stop(0);
    run_ms(1990, 5);
    int dhold = snd_duck_level();
    run_ms(1020, 5);
    int dend = snd_duck_level();
    st_check(t, abs(d500 - (255 - 63 / 2)) <= 1 && d1100 == 192 && dhold == 192 && dend == 255,
             "ducking: 500ms=%d 1100ms=%d hold@2000=%d released@3000=%d", d500, d1100, dhold, dend);
}

/* stop(1000) ends between 990 and 1010 ms; set_volume ramps from the previous target. */
static void check_fades(StCtx *t)
{
    bgm_stop(1000);
    run_ms(990, 5);
    bool alive = bgm_playing();
    run_ms(20, 5);
    st_check(t, alive && !bgm_playing(), "bgm stop(1000): alive at 990 ms, stopped by 1010 ms");
    bgm_set_volume(0, 1000);
    run_ms(500, 5);
    int mid = snd_bgm_volume_now();
    run_ms(500, 5);
    st_check(t, abs(mid - 128) <= 2 && snd_bgm_volume_now() == 0 && bgm_get_volume() == 0,
             "bgm set_volume(0,1000): %d at 500 ms", mid);
    bgm_set_volume(255, 0);
}

static void play_chime(void)
{
    PcmchArgs a = { .volume_type = 2, .se_no = -1 };
    ws_set_utf8(&a.name, "chime1");
    pcmch_play(0, &a);
    ws_free(&a.name);
}

static void check_oneshot(StCtx *t)
{
    play_chime();
    bool on = pcmch_playing(0);
    const MixSound *ps = snd_stream_sound(MIX_PCMCH0);
    int ms = ps ? (int)((uint64_t)ps->frames * 1000 / (uint32_t)ps->rate) : 0;
    run_ms(ms + 50, 5);
    st_check(t, on && !pcmch_playing(0), "pcmch 0 chime1 (%d ms) plays and ends", ms);
}

static void check_behaviour(StCtx *t)
{
    st_check(
        t, snd_gain_of(255) == 1.0f && snd_gain_of(0) == 0.0f && fabs(snd_gain_of(128) - pow(128 / 255.0, 2.5)) < 0.01,
        "gain curve: g(128)=%.4f vs (128/255)^2.5=%.4f", snd_gain_of(128), pow(128 / 255.0, 2.5));
    WStr rain = {0};
    ws_set_utf8(&rain, "rain");
    check_loop(t, &rain);
    check_crossfade(t);
    check_ducking(t);
    check_fades(t);
    check_oneshot(t);
    ws_free(&rain);
    audio_stop_all(0);
    run_ms(10, 5);
}

/* ------------------------------------------------------------------------------------------------ 5. mixed render */

/* BGM11 fade-in, voice 10100134 (seen101, 4.8 s; ducks the BGM), SE, BGM fade-out; 16 ms frames. */
static void check_render(StCtx *t, const char *wav_out)
{
    const char *out = wav_out ? wav_out : "test.wav";
    if (!audio_capture_start(out)) t->fails++;
    WStr b = {0};
    ws_set_utf8(&b, "bgm11");
    bgm_play(&b, true, false, 500, 0, -1, 0);
    ws_free(&b);
    run_ms(1504, 16);
    (void)koe_play(10100134, -1, 100);              /* checked through the window level below */
    snd_capture_window_reset();
    int vms = 0;
    while (koe_playing() && vms < 20000) { audio_frame(16); vms += 16; }
    double vrms = snd_capture_window_rms();
    play_chime();
    run_ms(800, 16);
    bgm_stop(1000);
    run_ms(1200, 16);
    AudioStats st;
    snd_capture_stats(&st);
    audio_capture_stop();
    st_check(t, vms > 4000 && dbfs(vrms) > -40.0, "voice 10100134 window: %d ms, rms %.1f dBFS (> -40)", vms,
             dbfs(vrms));
    st_check(t, st.frames > 0 && st.peak > 1000, "capture %s: %.3f s, peak %d (%.1f dBFS), rms %.4f (%.1f dBFS)",
             out, (double)st.frames / AUDIO_RATE, st.peak, dbfs(st.peak / 32768.0), st.rms, dbfs(st.rms));
}

int audio_selftest_ex(const char *gamedata, const char *wav_out, const char *koe_ref)
{
    StCtx t = { 0 };
    if (!gamedata) return 1;
    if (*gamedata && !fs_mounted() && !fs_init(gamedata)) return 1;   /* "" = the data already mounted */
    printf("audio selftest: %s\n", gamedata);
    check_nwa(&t);
    check_ovk(&t);
    (void)audio_init(false);                        /* null sink: false is the expected result */
    use_builtin_bgm_table(true);
    check_koe(&t, koe_ref);
    check_behaviour(&t);
    (void)audio_init(false);
    use_builtin_bgm_table(false);
    check_render(&t, wav_out);
    audio_shutdown();
    printf("audio selftest: %s (%d failure%s)\n", t.fails ? "FAILED" : "passed", t.fails, t.fails == 1 ? "" : "s");
    return t.fails ? 1 : 0;
}
