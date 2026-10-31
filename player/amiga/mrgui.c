/* MintVID ReAction controller.
 *
 * The GUI stays on Workbench and launches the separate mrplay executable.
 * ReAction classes are opened explicitly, following MintAMP's working setup.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/tasks.h>
#include <exec/lists.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/displayinfo.h>
#include <graphics/gfxbase.h>
#include <cybergraphx/cybergraphics.h>
#include <classes/window.h>
#include <gadgets/button.h>
#include <gadgets/checkbox.h>
#include <gadgets/chooser.h>
#include <gadgets/getfile.h>
#include <gadgets/listbrowser.h>
#include <libraries/asl.h>
#include <gadgets/layout.h>
#include <gadgets/string.h>
#include <gadgets/slider.h>
#include <images/label.h>
#include <reaction/reaction.h>
#include <reaction/reaction_macros.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <clib/alib_protos.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <proto/cybergraphics.h>
#include <proto/button.h>
#include <proto/checkbox.h>
#include <proto/chooser.h>
#include <proto/getfile.h>
#include <proto/listbrowser.h>
#include <proto/label.h>
#include <proto/layout.h>
#include <proto/string.h>
#include <proto/slider.h>
#include <proto/window.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/mr_play_options.h"
#include "../iptv/mr_iptv.h"
#include "mr_master_options.h"
#include "mr_akiko.h"
#include "mr_gui_menu.h"
#include "mr_paula_pref.h"
#include "mr_last_dir.h"
#include "mr_player_status.h"
#include "mr_saved_options.h"
#include "mr_playlist.h"

MINTVID_DECLARE_VERSION(mintvid_version_tag, "MintVID-pi");

#ifndef MRGUI_CLASS_VERSION
#define MRGUI_CLASS_VERSION 44
#endif

#define MRPLAY_STACK_SIZE 320000UL

#if defined(__GNUC__)
static const char mrgui_stack_cookie[] __attribute__((used)) =
    "$STACK:131072";
#endif

/* Runtime library bases.  CyberGfxBase is optional: a planar Workbench must
 * still be able to run the AGA/HAM controller. */
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *UtilityBase;
struct Library *AslBase;
struct Library *CyberGfxBase;
struct Library *WindowBase;
struct Library *LayoutBase;
struct Library *ButtonBase;
struct Library *CheckBoxBase;
struct Library *ChooserBase;
struct Library *GetFileBase;
struct Library *ListBrowserBase;
struct Library *StringBase;
struct Library *SliderBase;
struct Library *LabelBase;

enum {
    G_FILE = 1,
    G_PLAY,
    G_PAUSE,
    G_STOP,
    G_FF,
    G_MODE,
    G_C2P,
    G_H264,
    G_LACE,
    G_SCALE,
    G_AUDIO_RATE,
    G_FAST_BUFFER,
    G_NO_AUDIO,
    G_MONO_AUDIO,
    G_VIDEO_MODE,
    G_IPTV,
    G_YOUTUBE,
    G_VOLUME,
    G_PLAYLIST,
    G_SKIP_AFTER,
    G_TIME,
    G_TIME_LABEL,
    G_AUDIO_TRACK,                      /* MKA1: audio track chooser */
    G_SUB_TRACK                         /* S2: subtitle file chooser */
};

/* Chooser rows are chipset-dependent, so never infer a display mode from a
 * hard-coded row number. This map is populated alongside the labels. */
static mr_display_mode mode_values[16];
static unsigned mode_count;
/* "Skip after" chooser (--skip-trigger=). Kept file-level rather than
 * threaded through every read_play_options() caller's argument list, since
 * there is exactly one of it for the life of the window. Row i is
 * skip_after_ms[i]; update_skip_after() greys it out whenever it would have
 * no effect (Video: All Frames, or VQ Smoosh). */
static Object *g_skip_after;
static const unsigned skip_after_ms[] = {200, 500, 700, 1000, 1500, 2000};
#define SKIP_AFTER_ROWS (sizeof skip_after_ms / sizeof skip_after_ms[0])

static ULONG skip_after_row(unsigned ms)
{
    ULONG i, best = 2;
    unsigned best_diff = ~0u;
    for (i = 0; i < SKIP_AFTER_ROWS; i++) {
        unsigned diff = ms > skip_after_ms[i] ? ms - skip_after_ms[i]
                                              : skip_after_ms[i] - ms;
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    return best;
}
/* MintVID-pi P96W59: only the H.264/PiStorm outputs are offered; the
 * classic mrplay options are created (read_play_options() still reads
 * them) but not shown, and their update functions do nothing. */
static const int g_pi_only = 1;
static mr_c2p_mode c2p_values[5];
static unsigned c2p_count;
static int add_chooser_node(struct List *list, const char *text);

static int chipset_has_aga(void)
{
    return GfxBase && (GfxBase->ChipRevBits0 & GFXF_AA_LISA) != 0;
}

static int __attribute__((unused)) chipset_has_ecs_denise(void)
{
    return GfxBase && (GfxBase->ChipRevBits0 & GFXF_HR_DENISE) != 0;
}

static int add_mode_node(struct List *list, const char *text,
                         mr_display_mode value)
{
    if (mode_count >= sizeof mode_values / sizeof mode_values[0] ||
        !add_chooser_node(list, text))
        return 0;
    mode_values[mode_count++] = value;
    return 1;
}

static int add_c2p_node(struct List *list, const char *text,
                        mr_c2p_mode value)
{
    if (c2p_count >= sizeof c2p_values / sizeof c2p_values[0] ||
        !add_chooser_node(list, text))
        return 0;
    c2p_values[c2p_count++] = value;
    return 1;
}

static ULONG c2p_row(mr_c2p_mode value)
{
    unsigned i;
    for (i = 0; i < c2p_count; i++)
        if (c2p_values[i] == value)
            return (ULONG)i;
    return 0;
}

/* Unlike c2p_row(), returns -1 rather than a fallback row: a saved display
 * mode from a previous, differently-equipped session (e.g. RTG on that
 * launch, plain AGA on this one) should leave the caller's own
 * hardware-detected default alone, not force row 0. */
static int mode_row(mr_display_mode value)
{
    unsigned i;
    for (i = 0; i < mode_count; i++)
        if (mode_values[i] == value)
            return (int)i;
    return -1;
}

static int open_reaction_classes(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary(
        (CONST_STRPTR)"graphics.library", 39);
    UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 39);
    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 39);

    /* Optional.  Its presence alone does not mean the current Workbench is
     * RTG; default_screen_is_rtg() checks the actual public-screen mode. */
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);

    WindowBase = OpenLibrary((CONST_STRPTR)"window.class",
                             MRGUI_CLASS_VERSION);
    LayoutBase = OpenLibrary((CONST_STRPTR)"gadgets/layout.gadget",
                             MRGUI_CLASS_VERSION);
    ButtonBase = OpenLibrary((CONST_STRPTR)"gadgets/button.gadget",
                             MRGUI_CLASS_VERSION);
    CheckBoxBase = OpenLibrary((CONST_STRPTR)"gadgets/checkbox.gadget",
                               MRGUI_CLASS_VERSION);
    ChooserBase = OpenLibrary((CONST_STRPTR)"gadgets/chooser.gadget",
                              MRGUI_CLASS_VERSION);
    GetFileBase = OpenLibrary((CONST_STRPTR)"gadgets/getfile.gadget",
                              MRGUI_CLASS_VERSION);
    ListBrowserBase = OpenLibrary((CONST_STRPTR)"gadgets/listbrowser.gadget",
                                  MRGUI_CLASS_VERSION);
    StringBase = OpenLibrary((CONST_STRPTR)"gadgets/string.gadget",
                             MRGUI_CLASS_VERSION);
    SliderBase = OpenLibrary((CONST_STRPTR)"gadgets/slider.gadget",
                             MRGUI_CLASS_VERSION);
    LabelBase = OpenLibrary((CONST_STRPTR)"images/label.image",
                            MRGUI_CLASS_VERSION);

    return IntuitionBase && GfxBase && UtilityBase && AslBase &&
           WindowBase && LayoutBase && ButtonBase && CheckBoxBase &&
           ChooserBase && GetFileBase && ListBrowserBase && StringBase &&
           SliderBase && LabelBase;
}

static void close_reaction_classes(void)
{
    if (LabelBase) {
        CloseLibrary(LabelBase);
        LabelBase = NULL;
    }
    if (StringBase) {
        CloseLibrary(StringBase);
        StringBase = NULL;
    }
    if (SliderBase) {
        CloseLibrary(SliderBase);
        SliderBase = NULL;
    }
    if (GetFileBase) {
        CloseLibrary(GetFileBase);
        GetFileBase = NULL;
    }
    if (ListBrowserBase) {
        CloseLibrary(ListBrowserBase);
        ListBrowserBase = NULL;
    }
    if (ChooserBase) {
        CloseLibrary(ChooserBase);
        ChooserBase = NULL;
    }
    if (CheckBoxBase) {
        CloseLibrary(CheckBoxBase);
        CheckBoxBase = NULL;
    }
    if (ButtonBase) {
        CloseLibrary(ButtonBase);
        ButtonBase = NULL;
    }
    if (LayoutBase) {
        CloseLibrary(LayoutBase);
        LayoutBase = NULL;
    }
    if (WindowBase) {
        CloseLibrary(WindowBase);
        WindowBase = NULL;
    }
    if (CyberGfxBase) {
        CloseLibrary(CyberGfxBase);
        CyberGfxBase = NULL;
    }
    if (AslBase) {
        CloseLibrary(AslBase);
        AslBase = NULL;
    }
    if (UtilityBase) {
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
    }
    if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
}

static int default_screen_is_rtg(void)
{
    struct Screen *screen;
    ULONG mode_id;
    int is_rtg;

    if (!CyberGfxBase || !GfxBase || !IntuitionBase)
        return 0;

    screen = LockPubScreen(NULL);
    if (!screen)
        return 0;

    mode_id = GetVPModeID(&screen->ViewPort);
    is_rtg = mode_id != (ULONG)INVALID_ID && IsCyberModeID(mode_id);
    UnlockPubScreen(NULL, screen);
    return is_rtg;
}

static int add_chooser_node(struct List *list, const char *text)
{
    struct Node *node;

    node = AllocChooserNode(CNA_Text, (ULONG)text, TAG_END);
    if (!node)
        return 0;

    AddTail(list, node);
    return 1;
}

static void free_chooser_nodes(struct List *list)
{
    struct Node *node;

    while ((node = RemHead(list)) != NULL)
        FreeChooserNode(node);
}

static struct Task *find_player(void)
{
    struct Task *task;

    Forbid();
    task = FindTask((STRPTR)"MintVID player");
    if (!task)
        task = FindTask((STRPTR)"MintVID-pi player");
    if (!task)
        task = FindTask((STRPTR)"MintVID Berry player");
    if (!task)
        task = FindTask((STRPTR)"MintVID VPberry player");
    Permit();
    return task;
}

static void signal_player(ULONG mask)
{
    struct Task *task;

    Forbid();
    task = FindTask((STRPTR)"MintVID player");
    if (!task)   /* P96W45: MintVID-pi mrberry knows Ctrl-D = pause */
        task = FindTask((STRPTR)"MintVID-pi player");
    if (task)
        Signal(task, mask);
    Permit();
}

static void stop_player_and_wait(void)
{
    struct Task *task;
    int guard;

    /* mrplay deliberately uses Ctrl-F so AmigaDOS cannot abort it before
     * cleanup.  The hardware-confirmed AC309J mrberry already handles
     * Ctrl-C, so give the two backends distinct task names and stop each
     * with its native signal.  This avoids changing the tested backend. */
    Forbid();
    task = FindTask((STRPTR)"MintVID-pi player");   /* MintVID-pi: mrberry */
    if (!task)
        task = FindTask((STRPTR)"MintVID Berry player");
    if (task)
        Signal(task, SIGBREAKF_CTRL_C);
    else {
        task = FindTask((STRPTR)"MintVID VPberry player");
        if (task)
            Signal(task, SIGBREAKF_CTRL_C);
        else {
            task = FindTask((STRPTR)"MintVID player");
            if (task) Signal(task, SIGBREAKF_CTRL_F);
        }
    }
    Permit();
    for (guard = 0; guard < 250 && find_player(); guard++)
        Delay(1);
}

static void set_info(Object *info, struct Window *window, const char *text);

static void read_play_options(Object *mode, Object *c2p, Object *h264,
                              Object *lace, Object *scale,
                              Object *audio_rate, Object *fast_buffer,
                              Object *no_audio,
                              Object *mono_audio,
                              Object *video_mode,
                              mr_play_options *options)
{
    ULONG selected = 0, selected_c2p = 0, selected_h264 = 0;
    ULONG checked_lace = 0, selected_scale = 0;
    ULONG selected_audio_rate = 0, selected_fast_buffer = 0;
    ULONG checked_no_audio = 0, checked_mono = 0;
    ULONG selected_video_mode = 0;
    mr_play_options_default(options);
    GetAttr(CHOOSER_Selected, mode, &selected);
    GetAttr(CHOOSER_Selected, c2p, &selected_c2p);
    GetAttr(CHOOSER_Selected, h264, &selected_h264);
    GetAttr(CHECKBOX_Checked, lace, &checked_lace);
    GetAttr(CHOOSER_Selected, scale, &selected_scale);
    GetAttr(CHOOSER_Selected, audio_rate, &selected_audio_rate);
    GetAttr(CHOOSER_Selected, fast_buffer, &selected_fast_buffer);
    GetAttr(CHECKBOX_Checked, no_audio, &checked_no_audio);
    GetAttr(CHECKBOX_Checked, mono_audio, &checked_mono);
    GetAttr(CHOOSER_Selected, video_mode, &selected_video_mode);
    options->display = selected < mode_count
                     ? mode_values[selected] : MR_DISPLAY_AGA;
    options->c2p = selected_c2p < c2p_count
                 ? c2p_values[selected_c2p] : MR_C2P_STANDARD;
    options->laced = checked_lace != 0;
    /* Scale rows are fixed and never chipset-dependent - unlike mode/c2p,
     * no value-array indirection is needed: 0=None, 1=2x, 2=Copper 2x (see
     * where scale_modes is built in main()). update_mode_controls() is what
     * keeps row 2 from ever landing here when it wouldn't actually engage. */
    options->scale_2x = selected_scale >= 1;
    options->copper_vdouble = selected_scale == 2;
    options->h264_performance = selected_h264 <= MR_H264_PERF_SMOOSH
                              ? (mr_h264_performance)selected_h264
                              : MR_H264_PERF_AUTO;
    options->audio_rate = selected_audio_rate == 1
                        ? MR_AUDIO_RATE_LOW : MR_AUDIO_RATE_NORMAL;
    options->fast_buffer = selected_fast_buffer <= MR_FAST_BUFFER_64MB
                         ? (mr_fast_buffer_mode)selected_fast_buffer
                         : MR_FAST_BUFFER_AUTO;
    options->no_audio = checked_no_audio != 0;
    options->mono_audio = checked_mono != 0;
    /* Video rows are fixed, like Scale: 0=All Frames (throughput on), 1=Skip
     * Frames (throughput off) - see where video_modes is built in main().
     * Audio only is the Display chooser's "No Video" row. */
    options->throughput = selected_video_mode != 1;
    options->no_video = options->display == MR_DISPLAY_NONE;
    if (g_skip_after) {
        ULONG row = 2;
        GetAttr(CHOOSER_Selected, g_skip_after, &row);
        options->skip_trigger_ms = row < SKIP_AFTER_ROWS
                                 ? skip_after_ms[row]
                                 : MR_SKIP_TRIGGER_DEFAULT_MS;
    }
    /* PiStorm hard-fullscreen is local-playback-only for this first
     * integration. Do not persist it into the shared settings consumed by
     * the untouched IPTV/YouTube/mrplay paths. */
    if (options->display != MR_DISPLAY_PISTORM_HARD_FULLSCREEN &&
        options->display != MR_DISPLAY_PISTORM_HARD_WINDOW &&
        options->display != MR_DISPLAY_PISTORM_RGB &&
        options->display != MR_DISPLAY_PISTORM_PAULA)
        mr_saved_options_save(options);
}

/* "Skip after" only matters under Video: Skip Frames, and Video/VQ/Skip
 * after all do nothing under Display: No Video - see mrgui_gadtools.c's
 * update_skip_after(), which this mirrors. */
static void update_skip_after(Object *mode, Object *h264, Object *video_mode,
                              struct Window *window)
{
    ULONG selected = 0, selected_h264 = 0, selected_video_mode = 0;
    ULONG no_video;
    if (g_pi_only) return;
    GetAttr(CHOOSER_Selected, mode, &selected);
    GetAttr(CHOOSER_Selected, h264, &selected_h264);
    GetAttr(CHOOSER_Selected, video_mode, &selected_video_mode);
    no_video = selected < mode_count &&
               mode_values[selected] == MR_DISPLAY_NONE ? TRUE : FALSE;
    SetGadgetAttrs((struct Gadget *)video_mode, window, NULL,
                   GA_Disabled, no_video, TAG_DONE);
    SetGadgetAttrs((struct Gadget *)h264, window, NULL,
                   GA_Disabled, no_video, TAG_DONE);
    if (!g_skip_after) return;
    SetGadgetAttrs((struct Gadget *)g_skip_after, window, NULL,
                   GA_Disabled, (no_video || selected_video_mode != 1 ||
                                 selected_h264 == MR_H264_PERF_SMOOSH)
                                ? TRUE : FALSE,
                   TAG_DONE);
}

/* The PiStorm backend is currently local-file playback only.  Browser
 * windows still launch the untouched mrplay engine, so when the controller
 * is sitting on the PiStorm selector give IPTV/YouTube the last saved
 * classic MintVID display instead.  This keeps the new selector from
 * changing any existing browser behaviour. */
static void use_classic_browser_display(mr_play_options *options)
{
    mr_play_options saved;
    if (!options ||
        (options->display != MR_DISPLAY_PISTORM_HARD_FULLSCREEN &&
         options->display != MR_DISPLAY_PISTORM_HARD_WINDOW &&
         options->display != MR_DISPLAY_PISTORM_PAULA))
        return;
    if (mr_saved_options_load(&saved) &&
        saved.display != MR_DISPLAY_PISTORM_HARD_FULLSCREEN &&
        saved.display != MR_DISPLAY_PISTORM_HARD_WINDOW &&
        saved.display != MR_DISPLAY_PISTORM_PAULA &&
        saved.display <= MR_DISPLAY_HAM8_DITHER)
        options->display = saved.display;
    else
        options->display = MR_DISPLAY_AGA;
    options->no_video = options->display == MR_DISPLAY_NONE;
}

static void publish_play_options(mr_master_options_port *master_options,
                                 Object *mode, Object *c2p, Object *h264,
                                 Object *lace, Object *scale,
                                 Object *audio_rate, Object *fast_buffer,
                                 Object *no_audio,
                                 Object *mono_audio, Object *video_mode)
{
    mr_play_options options;

    read_play_options(mode, c2p, h264, lace, scale, audio_rate, fast_buffer,
                      no_audio, mono_audio, video_mode, &options);
    use_classic_browser_display(&options);
    mr_master_options_publish(master_options, &options);
}

static void open_iptv_browser(Object *mode, Object *c2p, Object *h264,
                              Object *lace, Object *scale,
                              Object *audio_rate, Object *fast_buffer,
                              Object *no_audio,
                              Object *mono_audio, Object *video_mode,
                              Object *info,
                              struct Window *window)
{
    BPTR seglist;
    struct Process *process;
    char arguments[512];
    mr_play_options options;

    read_play_options(mode, c2p, h264, lace, scale, audio_rate, fast_buffer,
                      no_audio, mono_audio, video_mode, &options);
    use_classic_browser_display(&options);
    if (!mr_build_iptv_arguments(arguments, sizeof(arguments), &options)) {
        set_info(info, window, "Could not build IPTV playback options.");
        return;
    }

    seglist = LoadSeg((CONST_STRPTR)"PROGDIR:iptvgui");
    if (!seglist)
        seglist = LoadSeg((CONST_STRPTR)"iptvgui");
    if (!seglist) {
        set_info(info, window,
                 "Could not load iptvgui (keep it beside MintVID).");
        return;
    }

    process = CreateNewProcTags(
        NP_Seglist, seglist,
        NP_FreeSeglist, TRUE,
        NP_Arguments, (ULONG)arguments,
        NP_StackSize, MRPLAY_STACK_SIZE,
        NP_Cli, TRUE,
        NP_CommandName, (ULONG)"iptvgui",
        NP_Name, (ULONG)"MintVID IPTV",
        TAG_END);
    if (!process) {
        UnLoadSeg(seglist);
        set_info(info, window, "Could not create the iptvgui process.");
    }
}

static void open_youtube_browser(Object *mode, Object *c2p, Object *h264,
                                 Object *lace, Object *scale,
                                 Object *audio_rate, Object *fast_buffer,
                                 Object *no_audio,
                                 Object *mono_audio, Object *video_mode,
                                 Object *info,
                                 struct Window *window)
{
    BPTR seglist;
    struct Process *process;
    char arguments[512];
    mr_play_options options;

    read_play_options(mode, c2p, h264, lace, scale, audio_rate, fast_buffer,
                      no_audio, mono_audio, video_mode, &options);
    use_classic_browser_display(&options);
    if (!mr_build_iptv_arguments(arguments, sizeof(arguments), &options)) {
        set_info(info, window, "Could not build YouTube playback options.");
        return;
    }
    seglist = LoadSeg((CONST_STRPTR)"PROGDIR:ytgui");
    if (!seglist)
        seglist = LoadSeg((CONST_STRPTR)"ytgui");
    if (!seglist) {
        set_info(info, window,
                 "Could not load ytgui (keep it beside MintVID).");
        return;
    }
    process = CreateNewProcTags(
        NP_Seglist, seglist,
        NP_FreeSeglist, TRUE,
        NP_Arguments, (ULONG)arguments,
        NP_StackSize, MRPLAY_STACK_SIZE,
        NP_Cli, TRUE,
        NP_CommandName, (ULONG)"ytgui",
        NP_Name, (ULONG)"MintVID YouTube",
        TAG_END);
    if (!process) {
        UnLoadSeg(seglist);
        set_info(info, window, "Could not create the ytgui process.");
    }
}

/* P96W72: the info area has two lines.  Line 1 (g_info_film) = the film
 * (name, length, video); line 2 (the old "info" object) = sound/Paula offset,
 * saved position and every status message (set_info). */
static Object *g_info_film;
static Object *g_info_status;        /* = info (line 2) */
static void set_film_line(struct Window *window, const char *text)
{
    if (!g_info_film || !window) return;
    SetGadgetAttrs((struct Gadget *)g_info_film, window, NULL,
                   STRINGA_TextVal, (ULONG)(text ? text : ""),
                   STRINGA_BufferPos, 0, STRINGA_DispPos, 0, TAG_DONE);
}

static void set_info(Object *info, struct Window *window, const char *text)
{
    if (!info || !window)
        return;

    SetGadgetAttrs((struct Gadget *)info, window, NULL,
                   STRINGA_TextVal, (ULONG)(text ? text : ""),
                   STRINGA_BufferPos, 0,
                   STRINGA_DispPos, 0,
                   TAG_DONE);
}

/* ---- MintVID-pi B (P96W57): timeline and mrberry INFO --------------------
 * Choosing a film runs "mrberry <film> INFO" (no Pi, no window) and fills:
 *   - the info line: length, video, sound (or why it cannot play),
 *   - the timeline slider (start time, seconds) with its "T h:mm:ss" label,
 *     preset to the position saved in <film>.t.
 * The slider does not move during playback; Play passes T=h:mm:ss to
 * mrberry.  Files mrberry cannot jump in (TS) keep the slider disabled. */
static Object *g_time_slider;
static Object *g_time_label;
static char g_time_text[24];
static ULONG g_film_len_s;
static int g_film_seek;
static char g_film_path[512];
/* P96W58: one Play/Pause button, the freed button is Fullscreen. */
static Object *g_play_button;
static Object *g_full_button;
static Object *g_ff_button;
static int g_ff_enabled;
static int g_play_shows_pause;      /* 1 = button reads "||" */
static int g_full_enabled = -1;
static int g_player_paused;         /* mrberry status text "Paused" */
static int g_player_was_running;
/* P96W78: Paula pause = mrberry stopped at the spot (status ENDED "Paused",
 * .t without recap).  Play then continues there like ">>". */
static int g_paula_paused;
/* P96W79: during the Paula pause MintVID-pi shows a still picture: mrberry
 * W=50 T=<spot> STILL (window, no sound).  g_still_active = that process is
 * ours; g_still_resume = Space pressed in the still window (status ENDED
 * "Resume") -> start the Paula fullscreen again. */
static int g_still_active;
static int g_still_resume;
static int g_full_req;              /* v96.82: F in the video window */
static int g_tab_req;               /* v96.85: Tab = chapter / 10 min jump */
static int g_launch_display = -1;   /* display of the last launch */
static Object *g_mode;              /* P96W59: Display chooser */
/* MKA1: audio track chooser (MKV).  g_audio_n = tracks of the film,
 * g_audio_sel = chosen track (1-based, 0 = none).  g_info_want_track > 0
 * makes the next INFO run report that track ("INFO AUDIO=n"). */
static Object *g_audio_ch;
static struct List g_audio_labels;
static char g_audio_text[16][80];
static int g_audio_n, g_audio_sel;
static int g_audio_hdmi[16], g_audio_paula[16], g_audio_def[16];   /* MKA1c */
static int g_info_want_track;
/* S2: subtitle chooser.  Entry 0 = Off, n = SRT file n of the film (as
 * mrberry INFO numbers them).  g_sub_sel follows INFO's SUB_CHOSEN until
 * the user picks one for this film (g_sub_touched). */
static Object *g_sub_ch;
static struct List g_sub_labels;
static char g_sub_text[17][72];
static int g_sub_n, g_sub_sel, g_sub_touched;
static char g_sub_film[256];

typedef struct {
    int ok, playable, seek;
    ULONG len_ms, resume_s;
    int have_resume;
    char video[48], size[16], fps[12], audio[24], ch[4], sound[8], reason[80];
    char resume_a[8];                 /* P96W71: HDMI / PAULA / NONE */
    long resume_p;                    /* P96W72: Paula fine offset (ms) */
    int have_resume_p;
    /* MKA1: audio tracks (MKV) from TRACK<n>= lines */
    int tracks, track;                /* count, 1-based track INFO reported */
    struct {
        char lang[12], codec[12], name[48];
        int ch, hdmi, paula, def;
    } t[16];
    /* S2: SRT files from SUBS= / SUB<n>= / SUB_CHOSEN= */
    int subs, sub_chosen;
    char sub_label[16][64];
    int sub_forced[16];
} mrg_film_info;

static ULONG mrg_hms_to_s(const char *t)
{
    ULONG h = 0, m = 0, sec = 0;
    if (sscanf(t, "%lu:%lu:%lu", &h, &m, &sec) != 3) return 0;
    return h * 3600UL + m * 60UL + sec;
}

static void mrg_copy_field(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    const char *cut = strstr(src, " (fourcc");
    if (cut) n = (size_t)(cut - src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Run "mrberry <film> INFO" synchronously (own process via SystemTags) and
 * parse its KEY=VALUE lines from a T: file. */
static int mrg_film_info_read(const char *path, mrg_film_info *fi)
{
    static char cmd[900], prog[300], buf[4096];   /* MKA1: room for tracks */
    BPTR in, out, lock;
    LONG n;
    char *line;

    memset(fi, 0, sizeof *fi);
    if (!path || !*path || strchr(path, '"')) return 0;
    prog[0] = 0;
    lock = GetProgramDir();
    if (lock && NameFromLock(lock, (STRPTR)prog, sizeof prog - 16))
        AddPart((STRPTR)prog, (CONST_STRPTR)"mrberry", sizeof prog);
    else
        strcpy(prog, "mrberry");
    lock = Lock((CONST_STRPTR)prog, ACCESS_READ);
    if (lock) UnLock(lock);
    else strcpy(prog, "mrberry");
    if (g_info_want_track > 0)        /* MKA1: info for the chosen track */
        snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" INFO AUDIO=%d", prog, path,
                 g_info_want_track);
    else
        snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" INFO", prog, path);

    in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"T:MintVID-pi.info", MODE_NEWFILE);
    if (!in || !out) {
        if (in) Close(in);
        if (out) Close(out);
        return 0;
    }
    /* P96W78a: give mrberry the same stack as for playback.  Without
     * NP_StackSize a Workbench-started MintVID-pi runs the command with the
     * small default stack (often 4 KB); the MKV/MP4 demuxer overflowed it
     * and overwrote memory (rare 8000 0004 / freeze when choosing a film). */
    SystemTags((CONST_STRPTR)cmd, SYS_Input, (ULONG)in, SYS_Output, (ULONG)out,
               NP_StackSize, MRPLAY_STACK_SIZE, TAG_END);
    Close(in);
    Close(out);

    out = Open((CONST_STRPTR)"T:MintVID-pi.info", MODE_OLDFILE);
    if (!out) return 0;
    n = Read(out, buf, (LONG)sizeof buf - 1);
    Close(out);
    /* P96W78a: the file stays in T: (overwritten each time), so the last
     * INFO answer incl. STACK= can be checked with "Type T:MintVID-pi.info". */
    if (n <= 0) return 0;
    buf[n] = 0;

    for (line = buf; line && *line;) {
        char *next = strchr(line, '\n');
        char *eq;
        if (next) *next++ = 0;
        eq = strchr(line, '=');
        if (eq) {
            *eq++ = 0;
            if (!strcmp(line, "MRBERRY_INFO")) fi->ok = 1;
            else if (!strcmp(line, "PLAYABLE")) fi->playable = *eq == '1';
            else if (!strcmp(line, "SEEK")) fi->seek = *eq == '1';
            else if (!strcmp(line, "LENGTH_MS")) fi->len_ms = strtoul(eq, NULL, 10);
            else if (!strcmp(line, "RESUME_T")) {
                fi->resume_s = mrg_hms_to_s(eq);
                fi->have_resume = 1;
            }
            else if (!strcmp(line, "VIDEO")) mrg_copy_field(fi->video, sizeof fi->video, eq);
            else if (!strcmp(line, "SIZE")) mrg_copy_field(fi->size, sizeof fi->size, eq);
            else if (!strcmp(line, "FPS")) mrg_copy_field(fi->fps, sizeof fi->fps, eq);
            else if (!strcmp(line, "AUDIO")) mrg_copy_field(fi->audio, sizeof fi->audio, eq);
            else if (!strcmp(line, "AUDIO_CH")) mrg_copy_field(fi->ch, sizeof fi->ch, eq);
            else if (!strcmp(line, "SOUND")) mrg_copy_field(fi->sound, sizeof fi->sound, eq);
            else if (!strcmp(line, "REASON")) mrg_copy_field(fi->reason, sizeof fi->reason, eq);
            else if (!strcmp(line, "RESUME_A")) mrg_copy_field(fi->resume_a, sizeof fi->resume_a, eq);
            else if (!strcmp(line, "RESUME_P")) { fi->resume_p = strtol(eq, NULL, 10); fi->have_resume_p = 1; }
            else if (!strcmp(line, "AUDIO_TRACKS")) fi->tracks = atoi(eq);
            else if (!strcmp(line, "SUBS")) fi->subs = atoi(eq);              /* S2 */
            else if (!strcmp(line, "SUB_CHOSEN")) fi->sub_chosen = atoi(eq);
            else if (!strncmp(line, "SUB", 3) && line[3] >= '1' && line[3] <= '9') {
                int n = atoi(line + 3);
                char *bar = strrchr(eq, '|');
                if (n >= 1 && n <= 16) {
                    if (bar) { *bar++ = 0; fi->sub_forced[n - 1] = *bar == '1'; }
                    mrg_copy_field(fi->sub_label[n - 1], sizeof fi->sub_label[n - 1], eq);
                }
            }
            else if (!strcmp(line, "AUDIO_TRACK")) fi->track = atoi(eq);
            else if (!strncmp(line, "TRACK", 5)) {
                /* MKA1: TRACK<n>=lang|codec|ch|rate|hdmi|paula|default|name */
                int n = atoi(line + 5);
                char *f[8];
                int k = 0;
                if (n >= 1 && n <= 16) {
                    f[k++] = eq;
                    while (k < 8 && (eq = strchr(eq, '|')) != NULL) { *eq++ = 0; f[k++] = eq; }
                    if (k == 8) {
                        mrg_copy_field(fi->t[n - 1].lang, sizeof fi->t[n - 1].lang, f[0]);
                        mrg_copy_field(fi->t[n - 1].codec, sizeof fi->t[n - 1].codec, f[1]);
                        fi->t[n - 1].ch = atoi(f[2]);
                        fi->t[n - 1].hdmi = *f[4] == '1';
                        fi->t[n - 1].paula = *f[5] == '1';
                        fi->t[n - 1].def = *f[6] == '1';
                        mrg_copy_field(fi->t[n - 1].name, sizeof fi->t[n - 1].name, f[7]);
                    }
                }
            }
        }
        line = next;
    }
    return fi->ok;
}

static void mrg_fmt_hms(char *dst, size_t cap, ULONG s)
{
    snprintf(dst, cap, "%lu:%02lu:%02lu", s / 3600UL, (s / 60UL) % 60UL, s % 60UL);
}

static void mrg_time_label_update(struct Window *window)
{
    ULONG level = 0;
    char t[16];
    if (!g_time_label) return;
    if (g_time_slider && g_film_seek && g_film_len_s) {
        GetAttr(SLIDER_Level, g_time_slider, &level);
        mrg_fmt_hms(t, sizeof t, level);
        snprintf(g_time_text, sizeof g_time_text, "T %s", t);
    } else {
        strcpy(g_time_text, "T -:--:--");
    }
    if (window && SetGadgetAttrs((struct Gadget *)g_time_label, window, NULL,
                                 GA_Text, (ULONG)g_time_text, TAG_DONE))
        RefreshGList((struct Gadget *)g_time_label, window, NULL, 1);
}

/* P96W59: the timeline only works for mrberry (HDMI); h264rgb always
 * starts at the beginning. */
static int mrg_rgb_selected(void)
{
    ULONG row = 0;
    if (!g_mode) return 0;
    GetAttr(CHOOSER_Selected, g_mode, &row);
    return row < mode_count && mode_values[row] == MR_DISPLAY_PISTORM_RGB;
}

/* P96W71: Display chooser is on "PiStorm / VC Hard Fullscreen (Paula)" */
static int mrg_paula_selected(void)
{
    ULONG row = 0;
    if (!g_mode) return 0;
    GetAttr(CHOOSER_Selected, g_mode, &row);
    return row < mode_count && mode_values[row] == MR_DISPLAY_PISTORM_PAULA;
}

static void mrg_time_enable(struct Window *window)
{
    int on = g_film_seek && g_film_len_s && !mrg_rgb_selected();
    if (!g_time_slider || !window) return;
    SetGadgetAttrs((struct Gadget *)g_time_slider, window, NULL,
                   GA_Disabled, on ? FALSE : TRUE, TAG_DONE);
}

static void mrg_time_slider_set(struct Window *window, ULONG len_s,
                                ULONG level, int enabled)
{
    if (!g_time_slider || !window) return;
    if (!enabled || !len_s) { len_s = 1; level = 0; enabled = 0; }
    if (level >= len_s) level = 0;
    SetGadgetAttrs((struct Gadget *)g_time_slider, window, NULL,
                   SLIDER_Max, len_s, TAG_DONE);
    SetGadgetAttrs((struct Gadget *)g_time_slider, window, NULL,
                   SLIDER_Level, level, TAG_DONE);
    mrg_time_enable(window);
    mrg_time_label_update(window);
}

/* P96W72: line 2 = sound of the chosen film, with the Paula offset. */
static mrg_film_info g_fi;
static int g_fi_valid;
static long g_live_fine;
static int mrg_paula_selected(void);
static void mrg_sound_line(Object *info, struct Window *window)
{
    static char l2[200];
    char snd[64];
    if (!g_fi_valid || !g_fi.playable) return;
    if (mrg_paula_selected()) {
        if (g_fi.audio[0] && !strstr(g_fi.audio, "E-AC")) {
            long base = mr_paula_pref_load();     /* P96W73: menu value */
            snprintf(snd, sizeof snd, "Paula %ld ms (base %ld / fine %+ld)",
                     base + g_live_fine, base, g_live_fine);
        }
        else
            strcpy(snd, "none");
    } else {
        strcpy(snd, !strcmp(g_fi.sound, "HDMI") ? "HDMI" : "none");
    }
    snprintf(l2, sizeof l2, "%s %sch -> %s%s",
             g_fi.audio[0] ? g_fi.audio : "no sound", g_fi.ch, snd,
             g_fi.have_resume ? " | saved" : "");
    set_info(info, window, l2);
}

/* Fill timeline (always) and, if text is set, the info line for path. */
/* MKA1: ISO 639-2 / BCP 47 code -> English name for the chooser. */
static const char *mrg_lang_name(const char *code)
{
    static const char *const map[] = {
        "eng", "English", "en", "English", "ger", "German", "deu", "German",
        "de", "German", "fre", "French", "fra", "French", "fr", "French",
        "spa", "Spanish", "es", "Spanish", "ita", "Italian", "it", "Italian",
        "dut", "Dutch", "nld", "Dutch", "nl", "Dutch", "por", "Portuguese",
        "pt", "Portuguese", "rus", "Russian", "ru", "Russian", "pol", "Polish",
        "pl", "Polish", "swe", "Swedish", "sv", "Swedish", "dan", "Danish",
        "da", "Danish", "nor", "Norwegian", "no", "Norwegian", "nob", "Norwegian",
        "fin", "Finnish", "fi", "Finnish", "cze", "Czech", "ces", "Czech",
        "cs", "Czech", "hun", "Hungarian", "hu", "Hungarian", "tur", "Turkish",
        "tr", "Turkish", "gre", "Greek", "ell", "Greek", "el", "Greek",
        "jpn", "Japanese", "ja", "Japanese", "chi", "Chinese", "zho", "Chinese",
        "zh", "Chinese", "kor", "Korean", "ko", "Korean", "und", "Unknown",
        NULL
    };
    int i;
    for (i = 0; map[i]; i += 2)
        if (!strcmp(code, map[i])) return map[i + 1];
    return code[0] ? code : "Unknown";
}

/* MKA1: fill the Audio chooser from the film info.  One entry per track,
 * e.g. "1: German - AAC 2.0 (Director)"; "only Paula" / "only HDMI" /
 * "not playable" tell where the track can play.  Disabled with < 2 tracks. */
static void mrg_audio_chooser_update(struct Window *window, const mrg_film_info *fi)
{
    int i, n = fi ? fi->tracks : 0;
    if (!g_audio_ch) return;
    if (n > 16) n = 16;
    SetGadgetAttrs((struct Gadget *)g_audio_ch, window, NULL,
                   CHOOSER_Labels, (ULONG)~0, TAG_DONE);
    free_chooser_nodes(&g_audio_labels);
    for (i = 0; i < n; i++) {
        char chs[8];
        const char *where = "";
        int ch = fi->t[i].ch;
        if (ch == 1) strcpy(chs, "mono");
        else if (ch == 2) strcpy(chs, "2.0");
        else if (ch == 6) strcpy(chs, "5.1");
        else if (ch == 8) strcpy(chs, "7.1");
        else snprintf(chs, sizeof chs, "%dch", ch);
        if (!fi->t[i].hdmi && !fi->t[i].paula) where = " - not playable";
        else if (!fi->t[i].hdmi) where = " - only Paula";
        else if (!fi->t[i].paula) where = " - only HDMI";
        if (fi->t[i].name[0])
            snprintf(g_audio_text[i], sizeof g_audio_text[i], "%d: %s - %s %s (%s)%s",
                     i + 1, mrg_lang_name(fi->t[i].lang), fi->t[i].codec, chs,
                     fi->t[i].name, where);
        else
            snprintf(g_audio_text[i], sizeof g_audio_text[i], "%d: %s - %s %s%s",
                     i + 1, mrg_lang_name(fi->t[i].lang), fi->t[i].codec, chs, where);
        add_chooser_node(&g_audio_labels, g_audio_text[i]);
        g_audio_hdmi[i] = fi->t[i].hdmi;
        g_audio_paula[i] = fi->t[i].paula;
        g_audio_def[i] = fi->t[i].def;
    }
    if (!n) {
        strcpy(g_audio_text[0], fi && fi->audio[0] ? fi->audio : "-");
        add_chooser_node(&g_audio_labels, g_audio_text[0]);
    }
    g_audio_n = n;
    g_audio_sel = (n && fi->track >= 1 && fi->track <= n) ? fi->track : (n ? 1 : 0);
    SetGadgetAttrs((struct Gadget *)g_audio_ch, window, NULL,
                   CHOOSER_Labels, (ULONG)&g_audio_labels,
                   CHOOSER_Selected, (ULONG)(g_audio_sel ? g_audio_sel - 1 : 0),
                   GA_Disabled, n < 2 ? TRUE : FALSE, TAG_DONE);
}

/* S2b: subtitles only in fullscreen (cinema, HDMI or Paula sound); the
 * window mode has no OSD layer -> chooser grey there. */
static int mrg_sub_display_ok(void)
{
    ULONG row = 0;
    if (!g_mode) return 0;
    GetAttr(CHOOSER_Selected, g_mode, &row);
    return row < mode_count && (mode_values[row] == MR_DISPLAY_PISTORM_HARD_FULLSCREEN ||
                                mode_values[row] == MR_DISPLAY_PISTORM_PAULA);
}
static void mrg_sub_enable(struct Window *window)
{
    if (g_sub_ch)
        SetGadgetAttrs((struct Gadget *)g_sub_ch, window, NULL,
                       GA_Disabled, (g_sub_n < 1 || !mrg_sub_display_ok()) ? TRUE : FALSE,
                       TAG_DONE);
}

/* S2: fill the Subtitles chooser: "Off", then one entry per SRT file.
 * Keeps the user's choice while the same film stays selected. */
static void mrg_sub_chooser_update(struct Window *window, const mrg_film_info *fi,
                                   const char *path)
{
    int i, n = fi ? fi->subs : 0;
    if (!g_sub_ch) return;
    if (n > 16) n = 16;
    if (!path || strcmp(path, g_sub_film)) {
        g_sub_touched = 0;
        g_sub_film[0] = 0;
        if (path) { strncpy(g_sub_film, path, sizeof g_sub_film - 1); g_sub_film[sizeof g_sub_film - 1] = 0; }
    }
    SetGadgetAttrs((struct Gadget *)g_sub_ch, window, NULL,
                   CHOOSER_Labels, (ULONG)~0, TAG_DONE);
    free_chooser_nodes(&g_sub_labels);
    strcpy(g_sub_text[0], n ? "Off" : "-");
    add_chooser_node(&g_sub_labels, g_sub_text[0]);
    for (i = 0; i < n; i++) {
        snprintf(g_sub_text[i + 1], sizeof g_sub_text[i + 1], "%d: %s", i + 1,
                 fi->sub_label[i][0] ? fi->sub_label[i] : "?");
        add_chooser_node(&g_sub_labels, g_sub_text[i + 1]);
    }
    if (!g_sub_touched || g_sub_sel > n)
        g_sub_sel = (fi && fi->sub_chosen >= 0 && fi->sub_chosen <= n) ? fi->sub_chosen : 0;
    g_sub_n = n;
    SetGadgetAttrs((struct Gadget *)g_sub_ch, window, NULL,
                   CHOOSER_Labels, (ULONG)&g_sub_labels,
                   CHOOSER_Selected, (ULONG)g_sub_sel,
                   GA_Disabled, (n < 1 || !mrg_sub_display_ok()) ? TRUE : FALSE, TAG_DONE);
}

/* MKA1c: can track n (1-based) play with the display chosen now?
 * HDMI displays need an HDMI track (MP3 = only Paula, like Opus not). */
static int mrg_audio_track_ok(int n)
{
    if (n < 1 || n > g_audio_n) return 0;
    return mrg_paula_selected() ? g_audio_paula[n - 1] : g_audio_hdmi[n - 1];
}

/* MKA1c: after a film or display change: if the chosen track cannot play
 * with this display, take the default track that can, else the first. */
static int mrg_audio_fix_for_display(struct Window *window)
{
    int i, pick = 0;
    if (g_audio_n < 2 || mrg_audio_track_ok(g_audio_sel)) return 0;
    for (i = 1; !pick && i <= g_audio_n; i++)
        if (mrg_audio_track_ok(i) && g_audio_def[i - 1]) pick = i;
    for (i = 1; !pick && i <= g_audio_n; i++)
        if (mrg_audio_track_ok(i)) pick = i;
    if (!pick) return 0;
    g_audio_sel = pick;
    SetGadgetAttrs((struct Gadget *)g_audio_ch, window, NULL,
                   CHOOSER_Selected, (ULONG)(pick - 1), TAG_DONE);
    return 1;
}

/* MKA1: "AUDIO=n" for mrberry when the film has more than one track. */
static int mrg_args_append(char *args, size_t cap, const char *text);
static int mrg_args_append_audio(char *args, size_t cap, const char *path)
{
    char a[24];
    if (g_audio_n < 2 || !g_audio_sel || strcmp(path, g_film_path)) return 1;
    if (!mrg_audio_track_ok(g_audio_sel)) return 1;   /* MKA1c: mrberry picks */
    snprintf(a, sizeof a, "AUDIO=%d", g_audio_sel);
    return mrg_args_append(args, cap, a);
}

/* S2: "SUB=n" / "SUB=OFF" for mrberry when the film has SRT files. */
static int mrg_args_append_sub(char *args, size_t cap, const char *path)
{
    char a[24];
    if (g_sub_n < 1 || strcmp(path, g_film_path)) return 1;
    if (g_sub_sel >= 1 && g_sub_sel <= g_sub_n) snprintf(a, sizeof a, "SUB=%d", g_sub_sel);
    else strcpy(a, "SUB=OFF");
    return mrg_args_append(args, cap, a);
}

static int mrg_film_info_apply(const char *path, Object *info,
                               struct Window *window, int text)
{
    mrg_film_info fi;
    static char line[400];
    const char *name;

    if (!mrg_film_info_read(path, &fi)) {
        g_info_want_track = 0;
        g_film_len_s = 0; g_film_seek = 0; g_film_path[0] = 0;
        mrg_time_slider_set(window, 0, 0, 0);
        mrg_audio_chooser_update(window, NULL);          /* MKA1 */
        mrg_sub_chooser_update(window, NULL, NULL);      /* S2 */
        return 0;
    }
    g_info_want_track = 0;
    strncpy(g_film_path, path, sizeof g_film_path - 1);
    g_film_path[sizeof g_film_path - 1] = 0;
    mrg_audio_chooser_update(window, &fi);               /* MKA1 */
    mrg_sub_chooser_update(window, &fi, path);           /* S2 */
    g_film_len_s = fi.len_ms / 1000UL;
    g_film_seek = fi.seek && fi.playable;
    mrg_time_slider_set(window, g_film_len_s,
                        fi.have_resume ? fi.resume_s : 0, g_film_seek);
    if (!text) { mrg_audio_fix_for_display(window); return 1; }   /* MKA1c */
    /* P96W71: a .t that was saved with Paula sound (A:PAULA) selects the
     * Paula cinema entry, one saved with HDMI sound the normal cinema. */
    if (fi.have_resume && g_mode && window) {
        ULONG cur = 0;
        int want = -1;
        GetAttr(CHOOSER_Selected, g_mode, &cur);
        if (cur < mode_count) {
            if (!strcmp(fi.resume_a, "PAULA") &&
                mode_values[cur] == MR_DISPLAY_PISTORM_HARD_FULLSCREEN)
                want = mode_row(MR_DISPLAY_PISTORM_PAULA);
            else if (!strcmp(fi.resume_a, "HDMI") &&
                     mode_values[cur] == MR_DISPLAY_PISTORM_PAULA)
                want = mode_row(MR_DISPLAY_PISTORM_HARD_FULLSCREEN);
        }
        if (want >= 0)
            SetGadgetAttrs((struct Gadget *)g_mode, window, NULL,
                           CHOOSER_Selected, (ULONG)want, TAG_DONE);
    }
    if (mrg_audio_fix_for_display(window)) {             /* MKA1c */
        /* other track for this display: info line for that one */
        g_info_want_track = g_audio_sel;
        return mrg_film_info_apply(path, info, window, text);
    }
    name = (const char *)FilePart((CONST_STRPTR)path);
    g_fi = fi;
    g_fi_valid = 1;
    g_live_fine = fi.have_resume_p ? fi.resume_p : 0;
    if (!fi.playable) {
        snprintf(line, sizeof line, "%s | not playable: %s", name,
                 fi.reason[0] ? fi.reason : "unknown");
        set_film_line(window, line);
        set_info(info, window, "");
        return 1;
    } else {
        char len[16];
        if (g_film_len_s) mrg_fmt_hms(len, sizeof len, g_film_len_s);
        else strcpy(len, "?:??:??");
        snprintf(line, sizeof line, "%s | %s | %s %s %s fps",
                 name, len, fi.video, fi.size, fi.fps);
        set_film_line(window, line);
    }
    mrg_sound_line(info, window);
    return 1;
}

static void update_file_info(Object *file, Object *info,
                             struct Window *window)
{
    static char text[640];
    STRPTR path;
    BPTR lock;
    struct FileInfoBlock fib;
    const char *ext;

    path = NULL;
    GetAttr(GETFILE_FullFile, file, (ULONG *)&path);
    if (!path || !*path)
        return;

    {
        char drawer[256];
        mr_last_dir_from_path((const char *)path, drawer, sizeof(drawer));
        mr_last_dir_save(drawer);
    }

    ext = strrchr((const char *)path, '.');
    lock = Lock(path, ACCESS_READ);
    if (lock && Examine(lock, &fib)) {
        snprintf(text, sizeof(text), "%s | type: %s | %ld bytes", path,
                 ext && ext[1] ? ext + 1 : "unknown",
                 (long)fib.fib_Size);
    } else {
        snprintf(text, sizeof(text), "%s | type: %s", path,
                 ext && ext[1] ? ext + 1 : "unknown");
    }
    if (lock)
        UnLock(lock);

    set_film_line(window, text);
    set_info(info, window, "");
    /* P96W57: length, video/sound and saved position from mrberry INFO */
    SetWindowPointer(window, WA_BusyPointer, TRUE, TAG_DONE);
    mrg_film_info_apply((const char *)path, info, window, 1);
    SetWindowPointer(window, TAG_DONE);
}

static void update_mode_controls(Object *mode, Object *c2p, Object *lace,
                                 Object *scale, struct Window *window,
                                 int output_changed)
{
    ULONG selected;
    ULONG disable_chipset_options;
    ULONG selected_c2p;
    ULONG selected_scale;
    mr_c2p_mode selected_c2p_mode;
    int kalms_available;
    int direct_available;
    int copper_ok;

    if (g_pi_only) return;
    selected = 0;
    GetAttr(CHOOSER_Selected, mode, &selected);
    disable_chipset_options = selected < mode_count &&
                              !mr_display_has_screen_options(
                                  mode_values[selected])
                            ? TRUE : FALSE;

    selected_c2p = 0;
    GetAttr(CHOOSER_Selected, c2p, &selected_c2p);
    selected_c2p_mode = selected_c2p < c2p_count
                      ? c2p_values[selected_c2p] : MR_C2P_STANDARD;
    kalms_available = selected < mode_count &&
                      ((mode_values[selected] == MR_DISPLAY_AGA &&
                        chipset_has_aga()) ||
                       mr_display_ham_bits(mode_values[selected]) == 8
#ifdef MR_KALMS_040
                       || mr_display_ham_bits(mode_values[selected]) == 6
                       || mode_values[selected] == MR_DISPLAY_AGA_EHB
#endif
                      );
    direct_available = 0;
#ifdef MR_KALMS_040
    direct_available = selected < mode_count &&
                       mode_values[selected] == MR_DISPLAY_AGA &&
                       chipset_has_aga();
#endif

    if (output_changed && kalms_available &&
        selected_c2p_mode == MR_C2P_STANDARD) {
        SetGadgetAttrs((struct Gadget *)c2p, window, NULL,
                       CHOOSER_Selected, c2p_row(MR_C2P_KALMS), TAG_DONE);
        selected_c2p_mode = MR_C2P_KALMS;
    }
    if (selected_c2p_mode == MR_C2P_KALMS &&
        !disable_chipset_options && !kalms_available) {
        SetGadgetAttrs((struct Gadget *)c2p, window, NULL,
                       CHOOSER_Selected, c2p_row(MR_C2P_STANDARD), TAG_DONE);
        selected_c2p_mode = MR_C2P_STANDARD;
    }
    if (selected_c2p_mode == MR_C2P_DIRECT &&
        !disable_chipset_options && !direct_available) {
        SetGadgetAttrs((struct Gadget *)c2p, window, NULL,
                       CHOOSER_Selected, c2p_row(MR_C2P_STANDARD), TAG_DONE);
        selected_c2p_mode = MR_C2P_STANDARD;
    }

    SetGadgetAttrs((struct Gadget *)c2p, window, NULL,
                   GA_Disabled, disable_chipset_options,
                   TAG_DONE);
    if (disable_chipset_options) {
        SetGadgetAttrs((struct Gadget *)lace, window, NULL,
                       GA_Disabled, TRUE,
                       CHECKBOX_Checked, FALSE,
                       TAG_DONE);
        /* A real-hardware report found Scale not greying out (and not
         * snapping back to None) when switching to P96 - the two attribute
         * changes below were previously combined into one SetGadgetAttrs
         * call, same shape as lace's pair just above. The logic and call
         * site here are otherwise identical to c2p's/lace's own (already
         * working) disable path, so source review alone couldn't pin a root
         * cause; splitting CHOOSER_Selected and GA_Disabled into two
         * separate calls is the conservative fix - it forces two independent
         * attribute-update/redraw passes on chooser.class instead of relying
         * on both tags being applied and rendered correctly from one
         * combined taglist. Unconfirmed until retested on real hardware,
         * same standing limitation as every other Amiga-only GUI fix in this
         * tree (see CLAUDE.md). */
        SetGadgetAttrs((struct Gadget *)scale, window, NULL,
                       CHOOSER_Selected, 0,
                       TAG_DONE);
        SetGadgetAttrs((struct Gadget *)scale, window, NULL,
                       GA_Disabled, TRUE,
                       TAG_DONE);
        return;
    }
    SetGadgetAttrs((struct Gadget *)lace, window, NULL,
                   GA_Disabled, FALSE,
                   TAG_DONE);
    SetGadgetAttrs((struct Gadget *)scale, window, NULL,
                   GA_Disabled, FALSE,
                   TAG_DONE);

    /* Copper 2x (row 2 of the Scale chooser) only ever does anything for a
     * plain c2p/riva-c2p/Akiko geometry - see display_aga.c's own
     * eligibility check in aga_open(). Kalms/Standard(WPA)/Direct all
     * silently no-op it at runtime instead of erroring, which is exactly
     * what let a real-hardware test "succeed" against plain --2x without the
     * copper path ever actually running - so snap back to row 1 (2x) here
     * instead of leaving a selection that looks chosen but was never
     * honoured. Re-checked on every mode/c2p/scale change (all three call
     * this function) so no ordering of clicks can leave Copper selected
     * under an incompatible combination.
     *
     * HAM6/HAM8 no longer disqualify Copper here (EXPERIMENTAL - see
     * display_aga.c's file header comment for the correctness argument and
     * its unconfirmed-on-real-hardware status): aga_open() downgrades HAM8
     * to HAM6 on a non-AGA chipset before its own copper eligibility check
     * runs, so either HAM selection here always resolves to something the
     * backend can actually honour - there is no chipset case this GUI needs
     * to pre-filter that aga_open() doesn't already handle. */
    copper_ok = selected_c2p_mode == MR_C2P_WPA ||
                selected_c2p_mode == MR_C2P_RIVA ||
                selected_c2p_mode == MR_C2P_AKIKO;
    if (!copper_ok) {
        selected_scale = 0;
        GetAttr(CHOOSER_Selected, scale, &selected_scale);
        if (selected_scale == 2)
            SetGadgetAttrs((struct Gadget *)scale, window, NULL,
                           CHOOSER_Selected, 1, TAG_DONE);
    }
}

#define STATUS_POLL_MICROS 250000UL
/* iptvgui's own process exists (LoadSeg()/CreateNewProcTags() has already
 * returned) long before its window is actually open and interactive - it
 * still has to load/refresh its channel cache first, which is the real
 * source of the "takes a while to show the window" delay. Polled on the
 * same STATUS_POLL_MICROS tick as poll_player_status() below, so ~60s here
 * is that many ticks, not a separate timer. Bounded so a launch that never
 * completes (missing binary, crash) doesn't leave the button disabled and
 * the busy pointer up forever.
 *
 * Was 60 ticks (~15s), which real A1200/68060 hardware confirmed was too
 * short: a cold channel-directory load (parsing tens of thousands of
 * channels/streams - see iptv/mr_iptv.c) plus a slow/network-backed cache
 * refresh can legitimately take longer than that, and this watchdog fired
 * before iptvgui's window ever opened, showing "IPTV browser did not open
 * (missing binary or crash?)" for a launch that was still genuinely in
 * progress and would have succeeded given more time. Same real-hardware
 * lesson as the live-stream stall this session's CLAUDE.md notes document
 * elsewhere: this target's own network/storage latency is much larger than
 * a desktop-tuned timeout assumes. Quadrupled to 240 ticks (~60s) rather
 * than removed outright - a launch that is genuinely missing/crashed should
 * still be reported eventually, just not this early. */
#define IPTV_LAUNCH_TIMEOUT_TICKS 240UL

static int iptv_launch_pending;
static ULONG iptv_launch_ticks;

static struct MsgPort *status_timer_port;
static struct timerequest *status_timer_io;
static int status_timer_device_open;
static int status_timer_running;
static ULONG player_status_seq;

static int status_timer_open(void)
{
    status_timer_port = CreateMsgPort();
    if (!status_timer_port) return 0;
    status_timer_io = (struct timerequest *)
        CreateIORequest(status_timer_port, sizeof(*status_timer_io));
    if (!status_timer_io) return 0;
    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                   (struct IORequest *)status_timer_io, 0) != 0)
        return 0;
    status_timer_device_open = 1;
    return 1;
}

static void status_timer_start(void)
{
    if (!status_timer_io) return;
    status_timer_io->tr_node.io_Command = TR_ADDREQUEST;
    status_timer_io->tr_time.tv_secs = 0;
    status_timer_io->tr_time.tv_micro = STATUS_POLL_MICROS;
    SendIO((struct IORequest *)status_timer_io);
    status_timer_running = 1;
}

static void status_timer_close(void)
{
    if (status_timer_io) {
        if (status_timer_running && !CheckIO((struct IORequest *)status_timer_io))
            AbortIO((struct IORequest *)status_timer_io);
        if (status_timer_running)
            WaitIO((struct IORequest *)status_timer_io);
        status_timer_running = 0;
        if (status_timer_device_open) {
            CloseDevice((struct IORequest *)status_timer_io);
            status_timer_device_open = 0;
        }
        DeleteIORequest((struct IORequest *)status_timer_io);
        status_timer_io = NULL;
    }
    if (status_timer_port) {
        DeleteMsgPort(status_timer_port);
        status_timer_port = NULL;
    }
}

/* PiStorm06: terminal player state must survive the child task disappearing.
 * mrplay already writes this atomic T: snapshot; mrberry now does the same. */
static void clear_player_status_snapshot(void)
{
    DeleteFile((CONST_STRPTR)MR_PLAYER_STATUS_FILE);
    DeleteFile((CONST_STRPTR)MR_PLAYER_STATUS_TMP);
}

static int read_player_status_snapshot(mr_player_status *out)
{
    mr_player_status_snapshot snapshot;
    BPTR file = Open((CONST_STRPTR)MR_PLAYER_STATUS_FILE, MODE_OLDFILE);
    LONG got;

    if (!file)
        return 0;
    got = Read(file, &snapshot, (LONG)sizeof snapshot);
    Close(file);
    if (got != (LONG)sizeof snapshot ||
        !mr_player_status_snapshot_valid(&snapshot))
        return 0;
    *out = snapshot.status;
    out->codec[sizeof(out->codec) - 1] = 0;
    out->text[sizeof(out->text) - 1] = 0;
    return 1;
}

static void poll_player_status(Object *info, struct Window *window)
{
    mr_player_status ps;
    char line[300];
    int live = mr_player_status_read(&ps);

    if (!live && !read_player_status_snapshot(&ps)) {
        player_status_seq = 0;
        return;
    }
    if (ps.seq == player_status_seq) return;
    player_status_seq = ps.seq;
    g_player_paused = ps.state == MR_PLAYER_STATE_PLAYING &&
                      !strcmp(ps.text, "Paused");
    if (ps.state == MR_PLAYER_STATE_ENDED && !strcmp(ps.text, "Jump")) {
        g_tab_req = 1;                            /* v96.85: Tab */
        set_info(info, window, "Jumping...");
        return;
    }
    if (ps.state == MR_PLAYER_STATE_ENDED && !strcmp(ps.text, "Fullscreen")) {
        g_full_req = 1;                           /* v96.82: key F */
        set_info(info, window, "Switching to fullscreen...");
        return;
    }
    if (ps.state == MR_PLAYER_STATE_ENDED && !strcmp(ps.text, "Paused")) {
        g_paula_paused = 1;                       /* P96W78 */
        set_info(info, window, "Paused - press > to continue.");
        return;
    }
    if (g_still_active) {                         /* P96W79: the still */
        if (ps.state == MR_PLAYER_STATE_ENDED && !strcmp(ps.text, "Resume"))
            g_still_resume = 1;
        if (ps.state == MR_PLAYER_STATE_PLAYING && !strcmp(ps.text, "Paused"))
            set_info(info, window, "Paused - press > to continue.");
        else if (ps.state == MR_PLAYER_STATE_OPENING)
            set_info(info, window, "Paused - showing the picture...");
        return;
    }

    if (ps.state == MR_PLAYER_STATE_OPENING)
        snprintf(line, sizeof(line), "%s",
                 ps.text[0] ? ps.text : "Opening player...");
    else if (ps.state == MR_PLAYER_STATE_PLAYING)
        snprintf(line, sizeof(line), "Playing%s%s%s%s", ps.codec[0] ? " (" : "",
                 ps.codec, ps.codec[0] ? "): " : "", ps.text);
    else if (ps.state == MR_PLAYER_STATE_UNSUPPORTED)
        snprintf(line, sizeof(line), "Not supported: %s", ps.text);
    else if (ps.state == MR_PLAYER_STATE_ERROR)
        snprintf(line, sizeof(line), "Player error: %s", ps.text);
    else if (ps.state == MR_PLAYER_STATE_ENDED)
        snprintf(line, sizeof(line), "Playback ended%s%s",
                 ps.text[0] ? ": " : "", ps.text);
    else
        return;
    set_info(info, window, line);
}

/* Called on the same status_timer tick as poll_player_status() while an
 * iptvgui launch is pending. Clears the busy indicator once iptvgui's own
 * window is open (MR_IPTV_GUI_PORT appears) or, failing that, once the
 * bounded timeout elapses. */
static void poll_iptv_launch(Object *info, struct Window *window,
                             Object *iptv_button)
{
    int ready;

    if (!iptv_launch_pending)
        return;
    Forbid();
    ready = FindPort((CONST_STRPTR)MR_IPTV_GUI_PORT) != NULL;
    Permit();
    if (!ready && ++iptv_launch_ticks < IPTV_LAUNCH_TIMEOUT_TICKS)
        return;
    iptv_launch_pending = 0;
    SetWindowPointer(window, TAG_DONE);
    SetGadgetAttrs((struct Gadget *)iptv_button, window, NULL,
                   GA_Disabled, FALSE, TAG_DONE);
    set_info(info, window,
            ready ? NULL
                  : "IPTV browser did not open (missing binary or crash?).");
}

/* Immediate feedback for the button click - iptvgui's own process exists
 * as soon as open_iptv_browser() returns, but its window can take a real
 * moment to appear (channel cache load/refresh) with nothing else visible
 * changing in the meantime. */
static void start_iptv_launch(Object *info, struct Window *window,
                              Object *iptv_button)
{
    SetGadgetAttrs((struct Gadget *)iptv_button, window, NULL,
                   GA_Disabled, TRUE, TAG_DONE);
    SetWindowPointer(window, WA_BusyPointer, TRUE, TAG_DONE);
    set_info(info, window, "Opening IPTV browser...");
    iptv_launch_pending = 1;
    iptv_launch_ticks = 0;
}

/* Insert " text" in front of the trailing newline of an mrberry argument
 * line. */
static int mrg_args_append(char *args, size_t cap, const char *text)
{
    size_t len = strlen(args);
    if (len && args[len - 1] == '\n') args[--len] = 0;
    if (len + strlen(text) + 3 > cap) return 0;
    strcat(args, " ");
    strcat(args, text);
    strcat(args, "\n");
    return 1;
}

/* P96W57: T=h:mm:ss from the timeline, only for the film it was read for. */
static int mrg_args_append_time(char *args, size_t cap, const char *path)
{
    ULONG level = 0;
    char t[24], hms[16];
    if (!g_time_slider || !g_film_seek || !g_film_len_s ||
        strcmp(path, g_film_path))
        return 1;
    GetAttr(SLIDER_Level, g_time_slider, &level);
    if (!level) return 1;
    mrg_fmt_hms(hms, sizeof hms, level);
    snprintf(t, sizeof t, "T=%s", hms);
    return mrg_args_append(args, cap, t);
}

static void start_player_path(const char *full_file, Object *mode,
                              Object *c2p, Object *h264,
                              Object *lace, Object *scale,
                              Object *audio_rate, Object *fast_buffer,
                              Object *no_audio,
                              Object *mono_audio, Object *video_mode,
                              Object *info,
                              struct Window *window)
{
    char path[512];
    char args[1600];
    mr_play_options options;
    BPTR seglist;
    struct Process *process;

    if (!full_file || !*full_file) {
        set_info(info, window, "Choose a video first.");
        return;
    }
    if (mr_path_is_audio_only(full_file)) {
        set_info(info, window, "Audio-only file: use MintAMP instead.");
        return;
    }
    if (find_player()) {
        set_info(info, window,
                 "A MintVID player is already running; stop it first.");
        return;
    }
    strncpy(path, full_file, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    read_play_options(mode, c2p, h264, lace, scale, audio_rate, fast_buffer,
                      no_audio, mono_audio, video_mode, &options);
    g_launch_display = (int)options.display;
    g_player_paused = 0;
    g_paula_paused = 0;

    if (options.display == MR_DISPLAY_PISTORM_HARD_FULLSCREEN ||
        options.display == MR_DISPLAY_PISTORM_PAULA) {
        /* PiStorm06: discard the previous child's terminal snapshot before
         * launching a new one and reset the GUI sequence latch. */
        clear_player_status_snapshot();
        player_status_seq = 0;
        if (!mr_build_mrberry_arguments(args, sizeof(args), path) ||
            !mrg_args_append_time(args, sizeof(args), path) ||
            !mrg_args_append_audio(args, sizeof(args), path) ||   /* MKA1 */
            !mrg_args_append_sub(args, sizeof(args), path) ||     /* S2 */
            (options.display == MR_DISPLAY_PISTORM_PAULA &&
             !mrg_args_append(args, sizeof(args), "PAULA"))) {
            set_info(info, window, "Could not build PiStorm player arguments.");
            return;
        }
        seglist = LoadSeg((CONST_STRPTR)"PROGDIR:mrberry");
        if (!seglist)
            seglist = LoadSeg((CONST_STRPTR)"mrberry");
        if (!seglist) {
            set_info(info, window,
                     "Could not load mrberry (keep it beside MintVID-pi).");
            return;
        }
        process = CreateNewProcTags(
            NP_Seglist, seglist, NP_FreeSeglist, TRUE,
            NP_Arguments, (ULONG)args, NP_StackSize, MRPLAY_STACK_SIZE,
            NP_Cli, TRUE, NP_CommandName, (ULONG)"mrberry",
            NP_Name, (ULONG)"MintVID-pi player", TAG_END);
        if (!process) {
            UnLoadSeg(seglist);
            set_info(info, window, "Could not create the mrberry process.");
        } else {
            set_info(info, window, options.display == MR_DISPLAY_PISTORM_PAULA ?
                     "Starting PiStorm playback, sound through Paula (+/- = sound offset)..." :
                     "Starting PiStorm VideoCore playback...");
        }
        return;
    }

    /* MintVID-pi: the PiStorm window is mrberry with W:50 (berryvp.library
     * P96 window, picture held to the Pi audio clock).  It replaces VPberry
     * and uses the same process name, status file and stop signal as the
     * fullscreen mrberry. */
    if (options.display == MR_DISPLAY_PISTORM_HARD_WINDOW) {
        clear_player_status_snapshot();
        player_status_seq = 0;
        if (!mr_build_mrberry_arguments(args, sizeof(args), path)) {
            set_info(info, window, "Could not build PiStorm player arguments.");
            return;
        }
        if (!mrg_args_append(args, sizeof(args), "W:50") ||
            !mrg_args_append_time(args, sizeof(args), path) ||
            !mrg_args_append_audio(args, sizeof(args), path)) {   /* MKA1 */
            set_info(info, window, "PiStorm player arguments too long.");
            return;
        }
        seglist = LoadSeg((CONST_STRPTR)"PROGDIR:mrberry");
        if (!seglist)
            seglist = LoadSeg((CONST_STRPTR)"mrberry");
        if (!seglist) {
            set_info(info, window,
                     "Could not load mrberry (keep it beside MintVID-pi).");
            return;
        }
        process = CreateNewProcTags(
            NP_Seglist, seglist, NP_FreeSeglist, TRUE,
            NP_Arguments, (ULONG)args, NP_StackSize, MRPLAY_STACK_SIZE,
            NP_Cli, TRUE, NP_CommandName, (ULONG)"mrberry",
            NP_Name, (ULONG)"MintVID-pi player", TAG_END);
        if (!process) {
            UnLoadSeg(seglist);
            set_info(info, window, "Could not create the mrberry process.");
        } else {
            set_info(info, window, "Starting PiStorm window playback (W:50)...");
        }
        return;
    }

    /* MintVID-pi P96W59: Amiga RGB output through Michael's h264rgb
     * (HAM6 192x144, no sound, no pause or start time).  It loads ham.bin,
     * edge.bin and state.bin from its current directory, so it runs with
     * the MintVID-pi drawer as current directory.  Ctrl-C stops it. */
    if (options.display == MR_DISPLAY_PISTORM_RGB) {
        BPTR home;
        if (!mr_build_mrberry_arguments(args, sizeof(args), path)) {
            set_info(info, window, "Could not build h264rgb arguments.");
            return;
        }
        seglist = LoadSeg((CONST_STRPTR)"PROGDIR:h264rgb");
        if (!seglist)
            seglist = LoadSeg((CONST_STRPTR)"h264rgb");
        if (!seglist) {
            set_info(info, window,
                     "Could not load h264rgb (keep it beside MintVID-pi).");
            return;
        }
        home = GetProgramDir() ? DupLock(GetProgramDir()) : 0;
        process = CreateNewProcTags(
            NP_Seglist, seglist, NP_FreeSeglist, TRUE,
            NP_Arguments, (ULONG)args, NP_StackSize, MRPLAY_STACK_SIZE,
            NP_Cli, TRUE, NP_CommandName, (ULONG)"h264rgb",
            NP_CurrentDir, (ULONG)home,
            NP_Name, (ULONG)"MintVID-pi player", TAG_END);
        if (!process) {
            UnLoadSeg(seglist);
            if (home) UnLock(home);
            set_info(info, window, "Could not create the h264rgb process.");
        } else {
            set_info(info, window, "Playing on the Amiga RGB output (HAM6, no sound)...");
        }
        return;
    }

    if (!mr_build_player_arguments(args, sizeof(args), &options, path,
                                   NULL, NULL)) {
        set_info(info, window, "Could not build player arguments.");
        return;
    }
    seglist = LoadSeg((CONST_STRPTR)"PROGDIR:mrplay");
    if (!seglist)
        seglist = LoadSeg((CONST_STRPTR)"mrplay");
    if (!seglist) {
        set_info(info, window,
                 "Could not load mrplay (keep it beside MintVID or in PATH).");
        return;
    }
    process = CreateNewProcTags(
        NP_Seglist, seglist, NP_FreeSeglist, TRUE,
        NP_Arguments, (ULONG)args, NP_StackSize, MRPLAY_STACK_SIZE,
        NP_Cli, TRUE, NP_CommandName, (ULONG)"mrplay",
        NP_Name, (ULONG)"MintVID player", TAG_END);
    if (!process) {
        UnLoadSeg(seglist);
        set_info(info, window, "Could not create the mrplay process.");
    }
}

static void start_player(Object *file, Object *mode, Object *c2p,
                         Object *h264, Object *lace, Object *scale,
                         Object *audio_rate, Object *fast_buffer,
                         Object *no_audio, Object *mono_audio,
                         Object *video_mode, Object *info,
                         struct Window *window)
{
    STRPTR full_file = NULL;
    GetAttr(GETFILE_FullFile, file, (ULONG *)&full_file);
    start_player_path((const char *)full_file, mode, c2p, h264, lace, scale,
                      audio_rate, fast_buffer, no_audio, mono_audio,
                      video_mode, info, window);
}



/* P96W58: button states, called on every status tick and after actions. */
static void mrg_buttons_update(struct Window *window)
{
    int running = find_player() != NULL;
    /* P96W78: Paula has a pause now (stop at the spot + Play) */
    int show_pause = running && !g_player_paused &&
                     g_launch_display != MR_DISPLAY_PISTORM_RGB;
    int full = running && g_launch_display == MR_DISPLAY_PISTORM_HARD_WINDOW;
    if (!running) g_player_paused = 0;
    if (g_play_button && window && show_pause != g_play_shows_pause) {
        g_play_shows_pause = show_pause;
        if (SetGadgetAttrs((struct Gadget *)g_play_button, window, NULL,
                           GA_Text, (ULONG)(show_pause ? "||" : ">"), TAG_DONE))
            RefreshGList((struct Gadget *)g_play_button, window, NULL, 1);
    }
    if (g_ff_button && window && running != g_ff_enabled) {
        g_ff_enabled = running;
        SetGadgetAttrs((struct Gadget *)g_ff_button, window, NULL,
                       GA_Disabled, running ? FALSE : TRUE, TAG_DONE);
    }
    if (g_full_button && window && full != g_full_enabled) {
        g_full_enabled = full;
        SetGadgetAttrs((struct Gadget *)g_full_button, window, NULL,
                       GA_Disabled, full ? FALSE : TRUE, TAG_DONE);
    }
}

/* P96W79: still picture for the Paula pause.  The slider already stands on
 * the pause spot (.t read again).  The display chooser and g_launch_display
 * stay on "Fullscreen (Paula)", so ">" and ">>" continue with Paula. */
static void mrg_start_still(Object *info, struct Window *window)
{
    char args[1600];
    BPTR seglist;
    struct Process *process;

    if (!g_film_path[0] || find_player()) return;
    if (!mr_build_mrberry_arguments(args, sizeof(args), g_film_path) ||
        !mrg_args_append(args, sizeof(args), "W=50") ||
        !mrg_args_append_time(args, sizeof(args), g_film_path) ||
        !mrg_args_append(args, sizeof(args), "STILL"))
        return;
    seglist = LoadSeg((CONST_STRPTR)"PROGDIR:mrberry");
    if (!seglist)
        seglist = LoadSeg((CONST_STRPTR)"mrberry");
    if (!seglist) return;
    clear_player_status_snapshot();
    player_status_seq = 0;
    g_still_resume = 0;
    process = CreateNewProcTags(
        NP_Seglist, seglist, NP_FreeSeglist, TRUE,
        NP_Arguments, (ULONG)args, NP_StackSize, MRPLAY_STACK_SIZE,
        NP_Cli, TRUE, NP_CommandName, (ULONG)"mrberry",
        NP_Name, (ULONG)"MintVID-pi player", TAG_END);
    if (!process) {
        UnLoadSeg(seglist);
        return;
    }
    g_still_active = 1;
    g_player_was_running = 1;
    set_info(info, window, "Paused - showing the picture...");
}

/* P96W79: end the still (Ctrl-F; it never writes the .t).  1 = gone. */
static int mrg_stop_still(void)
{
    struct Task *task;
    int guard;

    Forbid();
    task = FindTask((STRPTR)"MintVID-pi player");
    if (task) Signal(task, SIGBREAKF_CTRL_F);
    Permit();
    for (guard = 0; guard < 500 && find_player(); guard++)
        Delay(1);
    if (find_player()) return 0;
    g_still_active = 0;
    g_still_resume = 0;
    g_player_was_running = 0;
    g_player_paused = 0;
    return 1;
}

/* Fullscreen: stop the PiStorm window film with Ctrl-F (mrberry writes the
 * .t position without recap), read it, switch the Display chooser to
 * cinema and start again at that position. */
/* The window film has stopped and saved its spot in the .t: start it in
 * full screen from there (Fullscreen button, and v96.82 key F). */
static void mrg_fullscreen_restart(Object *file, Object *mode, Object *c2p,
                                   Object *h264, Object *lace, Object *scale,
                                   Object *audio_rate, Object *fast_buffer,
                                   Object *no_audio, Object *mono_audio,
                                   Object *video_mode, Object *info,
                                   struct Window *window, int row)
{
    g_info_want_track = g_audio_n > 1 ? g_audio_sel : 0;  /* MKA1: keep choice */
    mrg_film_info_apply(g_film_path, info, window, 0);
    SetGadgetAttrs((struct Gadget *)mode, window, NULL,
                   CHOOSER_Selected, (ULONG)row, TAG_DONE);
    update_mode_controls(mode, c2p, lace, scale, window, TRUE);
    update_skip_after(mode, h264, video_mode, window);
    mrg_sub_enable(window);                               /* S2b */
    SetWindowPointer(window, TAG_DONE);
    start_player(file, mode, c2p, h264, lace, scale, audio_rate, fast_buffer,
                 no_audio, mono_audio, video_mode, info, window);
    mrg_buttons_update(window);
}

static void mrg_switch_fullscreen(Object *file, Object *mode, Object *c2p,
                                  Object *h264, Object *lace, Object *scale,
                                  Object *audio_rate, Object *fast_buffer,
                                  Object *no_audio, Object *mono_audio,
                                  Object *video_mode, Object *info,
                                  struct Window *window)
{
    struct Task *task;
    int guard, row;

    if (!find_player() || g_launch_display != MR_DISPLAY_PISTORM_HARD_WINDOW ||
        !g_film_path[0]) {
        set_info(info, window, "Fullscreen works while a film plays in the PiStorm window.");
        return;
    }
    row = mode_row(MR_DISPLAY_PISTORM_HARD_FULLSCREEN);
    if (row < 0) return;
    SetWindowPointer(window, WA_BusyPointer, TRUE, TAG_DONE);
    set_info(info, window, "Switching to fullscreen...");
    Forbid();
    task = FindTask((STRPTR)"MintVID-pi player");
    if (task) Signal(task, SIGBREAKF_CTRL_F);
    Permit();
    for (guard = 0; guard < 500 && find_player(); guard++)
        Delay(1);
    if (find_player()) {
        SetWindowPointer(window, TAG_DONE);
        set_info(info, window, "The window film did not stop - fullscreen cancelled.");
        return;
    }
    g_player_was_running = 0;
    mrg_fullscreen_restart(file, mode, c2p, h264, lace, scale, audio_rate,
                           fast_buffer, no_audio, mono_audio, video_mode,
                           info, window, row);
}

/* P96W62: follow the running film on the timeline.  mrberry writes the
 * film position (whole seconds) to T:MintVID-pi.pos every 10 s of film
 * time.  After the user moves the slider it is left alone for
 * MRG_POS_HOLD_TICKS status ticks (or until ">>"/stop). */
#define MRG_POS_FILE "T:MintVID-pi.pos"
#define MRG_POS_HOLD_TICKS 40          /* 40 x 250 ms = 10 s */
static int g_pos_hold;
/* MKA1d: the user moved the timeline while a film runs (or is paused);
 * ">" then jumps there like ">>" instead of only ending the pause.
 * Cleared when the timeline follows the film again or after a jump. */
static int g_slider_touched;
static ULONG g_pos_last = (ULONG)-1;

static void mrg_pos_poll(struct Window *window)
{
    BPTR f;
    char buf[48];
    LONG n;
    ULONG sec = 0;
    int i;
    char *pp;
    if (!window || !g_film_path[0]) return;
    f = Open((CONST_STRPTR)MRG_POS_FILE, MODE_OLDFILE);
    if (!f) return;
    n = Read(f, buf, (LONG)sizeof buf - 1);
    Close(f);
    if (n <= 0) return;
    buf[n] = 0;
    /* P96W72: second line "P:<ms>" = live Paula fine offset */
    pp = strstr(buf, "P:");
    if (pp && g_launch_display == MR_DISPLAY_PISTORM_PAULA) {
        long fine = strtol(pp + 2, NULL, 10);
        if (fine != g_live_fine) {
            g_live_fine = fine;
            mrg_sound_line(g_info_status, window);
        }
    }
    if (g_pos_hold > 0) { g_pos_hold--; return; }
    if (!g_player_paused) g_slider_touched = 0;   /* MKA1d: follows the film */
    if (!g_time_slider || !g_film_seek || !g_film_len_s) return;
    for (i = 0; buf[i] >= '0' && buf[i] <= '9'; i++)
        sec = sec * 10 + (ULONG)(buf[i] - '0');
    if (!i || sec == g_pos_last) return;
    g_pos_last = sec;
    if (sec >= g_film_len_s) sec = g_film_len_s - 1;
    SetGadgetAttrs((struct Gadget *)g_time_slider, window, NULL,
                   SLIDER_Level, sec, TAG_DONE);
    mrg_time_label_update(window);
}

/* P96W61: ">>" jump.  The user sets a new time on the timeline (forward
 * or back) while a film plays and presses ">>": stop mrberry with Ctrl-F
 * (writes .t without the 30 s recap), then start the same film again in
 * the same display at the slider time. */
static void mrg_jump(Object *file, Object *mode, Object *c2p,
                     Object *h264, Object *lace, Object *scale,
                     Object *audio_rate, Object *fast_buffer,
                     Object *no_audio, Object *mono_audio,
                     Object *video_mode, Object *info,
                     struct Window *window)
{
    struct Task *task;
    ULONG level = 0;
    int guard, row;
    char hms[16], text[64];

    if (!find_player() || !g_film_path[0] ||
        (g_launch_display != MR_DISPLAY_PISTORM_HARD_WINDOW &&
         g_launch_display != MR_DISPLAY_PISTORM_HARD_FULLSCREEN &&
         g_launch_display != MR_DISPLAY_PISTORM_PAULA)) {
        set_info(info, window, "Set the time on the timeline while a film plays, then >>.");
        return;
    }
    if (!g_film_seek || !g_film_len_s || !g_time_slider) {
        set_info(info, window, "This film cannot jump (no seek support).");
        return;
    }
    row = mode_row(g_launch_display);
    if (row < 0) return;
    GetAttr(SLIDER_Level, g_time_slider, &level);
    mrg_fmt_hms(hms, sizeof hms, level);
    snprintf(text, sizeof text, "Jumping to %s...", hms);
    SetWindowPointer(window, WA_BusyPointer, TRUE, TAG_DONE);
    set_info(info, window, text);
    Forbid();
    task = FindTask((STRPTR)"MintVID-pi player");
    if (task) Signal(task, SIGBREAKF_CTRL_F);
    Permit();
    for (guard = 0; guard < 500 && find_player(); guard++)
        Delay(1);
    if (find_player()) {
        SetWindowPointer(window, TAG_DONE);
        set_info(info, window, "The film did not stop - jump cancelled.");
        return;
    }
    g_player_was_running = 0;
    g_player_paused = 0;
    g_still_active = 0;                 /* P96W79: ">>" also from the still */
    g_still_resume = 0;
    g_pos_hold = 0;
    g_slider_touched = 0;               /* MKA1d */
    g_pos_last = (ULONG)-1;
    /* re-read the film (.t now holds the stop position), then put the
     * slider back to the time the user chose */
    g_info_want_track = g_audio_n > 1 ? g_audio_sel : 0;  /* MKA1: keep choice */
    mrg_film_info_apply(g_film_path, info, window, 0);
    SetGadgetAttrs((struct Gadget *)g_time_slider, window, NULL,
                   SLIDER_Level, level, TAG_DONE);
    mrg_time_label_update(window);
    SetGadgetAttrs((struct Gadget *)mode, window, NULL,
                   CHOOSER_Selected, (ULONG)row, TAG_DONE);
    update_mode_controls(mode, c2p, lace, scale, window, TRUE);
    update_skip_after(mode, h264, video_mode, window);
    SetWindowPointer(window, TAG_DONE);
    start_player(file, mode, c2p, h264, lace, scale, audio_rate, fast_buffer,
                 no_audio, mono_audio, video_mode, info, window);
    mrg_buttons_update(window);
}

#define MRG_PL_LIST 200
#define MRG_PL_ADD 201
#define MRG_PL_REMOVE 202
#define MRG_PL_CLEAR 203
#define MRG_PL_PLAY 204
#define MRG_PL_LOAD 205
#define MRG_PL_SAVE 206
#define MRG_PL_CLOSE 207

typedef struct mrg_playlist_window {
    Object *window_object;
    struct Window *window;
    Object *list;
    struct List nodes;
    mr_playlist *playlist;
    ULONG sigmask;
    Object *mode, *c2p, *h264, *lace, *scale;
    Object *audio_rate, *fast_buffer, *no_audio, *mono_audio, *video_mode;
    Object *info;
    struct Window *parent_window;
} mrg_playlist_window;

static void mrg_playlist_free_nodes(mrg_playlist_window *p)
{
    struct Node *node;
    while ((node = RemHead(&p->nodes)) != NULL)
        FreeListBrowserNode(node);
}

static struct Node *mrg_playlist_node_at(mrg_playlist_window *p, int index)
{
    struct Node *node;
    int i = 0;
    for (node = p->nodes.lh_Head; node && node->ln_Succ;
         node = node->ln_Succ) {
        if (i++ == index)
            return node;
    }
    return NULL;
}

static void mrg_playlist_rebuild(mrg_playlist_window *p)
{
    int i;
    mrg_playlist_free_nodes(p);
    for (i = 0; i < p->playlist->count; i++) {
        struct Node *node = AllocListBrowserNode(
            1, LBNA_Column, 0, LBNCA_Text,
            (ULONG)p->playlist->names[i], TAG_END);
        if (!node)
            break;
        AddTail(&p->nodes, node);
    }
}

static void mrg_playlist_refresh(mrg_playlist_window *p)
{
    struct Node *selected;
    mrg_playlist_rebuild(p);
    if (!p->window || !p->list)
        return;
    selected = mrg_playlist_node_at(p, p->playlist->selected);
    SetGadgetAttrs((struct Gadget *)p->list, p->window, NULL,
                   LISTBROWSER_Labels, (ULONG)&p->nodes,
                   LISTBROWSER_SelectedNode, (ULONG)selected, TAG_DONE);
}

static void mrg_playlist_set_selected(mrg_playlist_window *p)
{
    struct Node *selected = NULL;
    struct Node *node;
    int i = 0;
    GetAttr(LISTBROWSER_SelectedNode, p->list, (ULONG *)&selected);
    for (node = p->nodes.lh_Head; node && node->ln_Succ;
         node = node->ln_Succ, i++)
        if (node == selected) {
            p->playlist->selected = i;
            return;
        }
}

static void mrg_playlist_add_files(mrg_playlist_window *p)
{
    struct FileRequester *req;
    char initial_drawer[256];
    int accepted;

    if (mr_last_dir_load(initial_drawer, sizeof(initial_drawer)))
        req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText, (ULONG)"Add videos to playlist",
            ASLFR_DoMultiSelect, TRUE, ASLFR_DoPatterns, TRUE,
            ASLFR_InitialPattern, (ULONG)MR_VIDEO_FILE_PATTERN,
            ASLFR_InitialDrawer, (ULONG)initial_drawer, TAG_DONE);
    else
        req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText, (ULONG)"Add videos to playlist",
            ASLFR_DoMultiSelect, TRUE, ASLFR_DoPatterns, TRUE,
            ASLFR_InitialPattern, (ULONG)MR_VIDEO_FILE_PATTERN, TAG_DONE);
    if (!req)
        return;

    accepted = AslRequestTags(req, ASLFR_Window, (ULONG)p->window,
                              ASLFR_SleepWindow, TRUE, TAG_DONE);
    /* fr_Drawer reflects the drawer visible when the requester closes, even
     * after Cancel. Save it before freeing this one-shot requester so the
     * next Add starts in the same place. */
    if (req->fr_Drawer && req->fr_Drawer[0])
        mr_last_dir_save((const char *)req->fr_Drawer);
    if (accepted) {
        char path[MR_PLAYLIST_PATH_MAX];
        int i;
        if (req->fr_NumArgs > 0 && req->fr_ArgList) {
            for (i = 0; i < (int)req->fr_NumArgs; i++) {
                strncpy(path, req->fr_Drawer ? (const char *)req->fr_Drawer : "",
                        sizeof(path) - 1);
                path[sizeof(path) - 1] = 0;
                if (req->fr_ArgList[i].wa_Name &&
                    AddPart((STRPTR)path, req->fr_ArgList[i].wa_Name,
                            sizeof(path)))
                    mr_playlist_add(p->playlist, path);
            }
        } else if (req->fr_File && req->fr_File[0]) {
            strncpy(path, req->fr_Drawer ? (const char *)req->fr_Drawer : "",
                    sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
            if (AddPart((STRPTR)path, req->fr_File, sizeof(path)))
                mr_playlist_add(p->playlist, path);
        }
    }
    FreeAslRequest(req);
    mrg_playlist_refresh(p);
}

static void mrg_playlist_load_m3u(mrg_playlist_window *p)
{
    struct FileRequester *req;
    char m3u[MR_PLAYLIST_PATH_MAX], drawer[MR_PLAYLIST_PATH_MAX];
    char line[MR_PLAYLIST_PATH_MAX];
    FILE *file;
    req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, (ULONG)"Load M3U Playlist",
        ASLFR_DoPatterns, TRUE, ASLFR_InitialPattern, (ULONG)"#?.m3u",
        TAG_DONE);
    if (!req)
        return;
    if (!AslRequestTags(req, ASLFR_Window, (ULONG)p->window,
                       ASLFR_SleepWindow, TRUE, TAG_DONE)) {
        FreeAslRequest(req);
        return;
    }
    strncpy(m3u, req->fr_Drawer ? (const char *)req->fr_Drawer : "", sizeof(m3u) - 1);
    m3u[sizeof(m3u) - 1] = 0;
    if (req->fr_File && req->fr_File[0])
        AddPart((STRPTR)m3u, req->fr_File, sizeof(m3u));
    FreeAslRequest(req);
    if (!m3u[0] || !(file = fopen(m3u, "r")))
        return;
    mr_last_dir_from_path(m3u, drawer, sizeof(drawer));
    while (fgets(line, sizeof(line), file)) {
        char path[MR_PLAYLIST_PATH_MAX];
        char *eol = strpbrk(line, "\r\n");
        if (eol) *eol = 0;
        if (!line[0] || line[0] == '#')
            continue;
        if (strchr(line, ':') || line[0] == '/') {
            strncpy(path, line, sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
        } else {
            strncpy(path, drawer, sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
            if (!AddPart((STRPTR)path, (STRPTR)line, sizeof(path)))
                continue;
        }
        mr_playlist_add(p->playlist, path);
    }
    fclose(file);
    mrg_playlist_refresh(p);
}

static void mrg_playlist_save_m3u(mrg_playlist_window *p)
{
    struct FileRequester *req;
    char m3u[MR_PLAYLIST_PATH_MAX];
    FILE *file;
    int i;
    req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, (ULONG)"Save M3U Playlist",
        ASLFR_DoSaveMode, TRUE, ASLFR_InitialFile, (ULONG)"playlist.m3u",
        TAG_DONE);
    if (!req)
        return;
    if (!AslRequestTags(req, ASLFR_Window, (ULONG)p->window,
                       ASLFR_SleepWindow, TRUE, TAG_DONE)) {
        FreeAslRequest(req);
        return;
    }
    strncpy(m3u, req->fr_Drawer ? (const char *)req->fr_Drawer : "", sizeof(m3u) - 1);
    m3u[sizeof(m3u) - 1] = 0;
    if (req->fr_File && req->fr_File[0])
        AddPart((STRPTR)m3u, req->fr_File, sizeof(m3u));
    FreeAslRequest(req);
    if (!m3u[0] || !(file = fopen(m3u, "w")))
        return;
    fprintf(file, "#EXTM3U\n");
    for (i = 0; i < p->playlist->count; i++)
        fprintf(file, "%s\n", p->playlist->paths[i]);
    fclose(file);
}

static void mrg_playlist_close(mrg_playlist_window *p)
{
    if (p->window_object) {
        if (p->window)
            RA_CloseWindow(p->window_object);
        DisposeObject(p->window_object);
    }
    mrg_playlist_free_nodes(p);
    p->window_object = NULL;
    p->window = NULL;
    p->list = NULL;
    p->sigmask = 0;
}

static int mrg_playlist_open(mrg_playlist_window *p, mr_playlist *playlist,
                             Object *mode, Object *c2p, Object *h264,
                             Object *lace, Object *scale,
                             Object *audio_rate, Object *fast_buffer,
                             Object *no_audio, Object *mono_audio,
                             Object *video_mode, Object *info,
                             struct Window *parent)
{
    Object *add, *remove, *clear, *play, *load, *save, *close;
    Object *row1, *row2, *layout;
    memset(p, 0, sizeof(*p));
    mr_playlist_list_init(&p->nodes);
    p->playlist = playlist;
    p->mode = mode; p->c2p = c2p; p->h264 = h264;
    p->lace = lace; p->scale = scale;
    p->audio_rate = audio_rate; p->fast_buffer = fast_buffer;
    p->no_audio = no_audio; p->mono_audio = mono_audio;
    p->video_mode = video_mode; p->info = info;
    p->parent_window = parent;
    mrg_playlist_rebuild(p);
    p->list = (Object *)NewObject(LISTBROWSER_GetClass(), NULL,
        GA_ID, MRG_PL_LIST, GA_RelVerify, TRUE,
        LISTBROWSER_Labels, (ULONG)&p->nodes,
        LISTBROWSER_AutoFit, TRUE, LISTBROWSER_ShowSelected, TRUE,
        LISTBROWSER_MinVisible, 12, TAG_DONE);
    add = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_ADD, GA_Text, (ULONG)"Add", GA_RelVerify, TRUE, TAG_DONE);
    remove = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_REMOVE, GA_Text, (ULONG)"Remove", GA_RelVerify, TRUE, TAG_DONE);
    clear = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_CLEAR, GA_Text, (ULONG)"Clear", GA_RelVerify, TRUE, TAG_DONE);
    play = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_PLAY, GA_Text, (ULONG)"Play", GA_RelVerify, TRUE, TAG_DONE);
    load = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_LOAD, GA_Text, (ULONG)"Load M3U", GA_RelVerify, TRUE, TAG_DONE);
    save = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_SAVE, GA_Text, (ULONG)"Save M3U", GA_RelVerify, TRUE, TAG_DONE);
    close = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_CLOSE, GA_Text, (ULONG)"Close", GA_RelVerify, TRUE, TAG_DONE);
    row1 = (Object *)NewObject(LAYOUT_GetClass(), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, LAYOUT_SpaceInner, TRUE,
        LAYOUT_AddChild, (ULONG)add, CHILD_WeightedWidth, 0,
        LAYOUT_AddChild, (ULONG)remove, CHILD_WeightedWidth, 0,
        LAYOUT_AddChild, (ULONG)clear, CHILD_WeightedWidth, 0,
        LAYOUT_AddChild, (ULONG)play, CHILD_WeightedWidth, 0, TAG_DONE);
    row2 = (Object *)NewObject(LAYOUT_GetClass(), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, LAYOUT_SpaceInner, TRUE,
        LAYOUT_AddChild, (ULONG)load, CHILD_WeightedWidth, 0,
        LAYOUT_AddChild, (ULONG)save, CHILD_WeightedWidth, 0,
        LAYOUT_AddChild, (ULONG)close, CHILD_WeightedWidth, 0, TAG_DONE);
    layout = (Object *)NewObject(LAYOUT_GetClass(), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT, LAYOUT_SpaceOuter, TRUE,
        LAYOUT_SpaceInner, TRUE,
        LAYOUT_AddChild, (ULONG)p->list, CHILD_WeightedHeight, 1,
        LAYOUT_AddChild, (ULONG)row1, CHILD_WeightedHeight, 0,
        LAYOUT_AddChild, (ULONG)row2, CHILD_WeightedHeight, 0, TAG_DONE);
    p->window_object = (Object *)NewObject(WINDOW_GetClass(), NULL,
        WA_Title, (ULONG)"MintVID-pi Playlist", WA_Activate, TRUE,
        WA_DepthGadget, TRUE, WA_DragBar, TRUE, WA_CloseGadget, TRUE,
        WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW |
                  IDCMP_REFRESHWINDOW,
        WINDOW_Position, WPOS_CENTERSCREEN,
        WINDOW_ParentGroup, (ULONG)layout, TAG_DONE);
    if (!p->list || !add || !remove || !clear || !play || !load || !save ||
        !close || !row1 || !row2 || !layout || !p->window_object)
        goto fail;
    p->window = (struct Window *)RA_OpenWindow(p->window_object);
    if (!p->window)
        goto fail;
    GetAttr(WINDOW_SigMask, p->window_object, &p->sigmask);
    return 1;
fail:
    mrg_playlist_close(p);
    return 0;
}

static void mrg_playlist_handle(mrg_playlist_window *p)
{
    ULONG result;
    UWORD code;
    if (!p->window_object)
        return;
    while ((result = RA_HandleInput(p->window_object, &code)) != WMHI_LASTMSG) {
        switch (result & WMHI_CLASSMASK) {
        case WMHI_CLOSEWINDOW:
            mrg_playlist_close(p);
            return;
        case WMHI_GADGETUP:
            switch (result & WMHI_GADGETMASK) {
            case MRG_PL_LIST:
                mrg_playlist_set_selected(p);
                break;
            case MRG_PL_ADD: mrg_playlist_add_files(p); break;
            case MRG_PL_REMOVE:
                mr_playlist_remove(p->playlist, p->playlist->selected);
                mrg_playlist_refresh(p);
                break;
            case MRG_PL_CLEAR:
                mr_playlist_clear(p->playlist);
                mrg_playlist_refresh(p);
                break;
            case MRG_PL_PLAY:
                if (p->playlist->selected >= 0 &&
                    p->playlist->selected < p->playlist->count) {
                    p->playlist->current = p->playlist->selected;
                    start_player_path(
                        p->playlist->paths[p->playlist->selected], p->mode,
                        p->c2p, p->h264, p->lace, p->scale, p->audio_rate,
                        p->fast_buffer, p->no_audio, p->mono_audio,
                        p->video_mode, p->info, p->parent_window);
                }
                break;
            case MRG_PL_LOAD: mrg_playlist_load_m3u(p); break;
            case MRG_PL_SAVE: mrg_playlist_save_m3u(p); break;
            case MRG_PL_CLOSE: mrg_playlist_close(p); return;
            }
            break;
        }
    }
}

static void apply_gui_volume(Object *slider)
{
    ULONG level = 64;
    if (slider) {
        GetAttr(SLIDER_Level, slider, &level);
        mr_player_control_set_volume(level);
    }
}

int main(void)
{
    Object *window_object;
    Object *file;
    Object *mode;
    Object *c2p;
    Object *h264;
    Object *lace;
    Object *scale;
    Object *audio_rate;
    Object *fast_buffer;
    Object *no_audio;
    Object *mono_audio;
    Object *video_mode;
    Object *info;
    Object *layout;
    Object *controls;
    Object *controls_top;
    Object *buttons;
    Object *transport;
    Object *services;
    Object *file_label;
    Object *display_label;
    Object *audio_track_label;          /* MKA1 */
    Object *sub_track_label;            /* S2 */
    Object *c2p_label;
    Object *h264_label;
    Object *scale_label;
    Object *audio_rate_label;
    Object *fast_buffer_label;
    Object *video_mode_label;
    Object *play_button;
    Object *pause_button;
    Object *stop_button;
    Object *ff_button;
    Object *iptv_button;
    Object *youtube_button;
    Object *volume_slider;
    Object *playlist_button;
    struct Window *window;
    mr_playlist playlist;
    mrg_playlist_window playlist_gui;
    mr_master_options_port *master_options;
    mr_gui_menu app_menu;
    struct List modes;
    struct List c2p_modes;
    struct List h264_modes;
    struct List scale_modes;
    struct List skip_after_modes;
    Object *skip_after_label;
    struct List audio_rate_modes;
    struct List fast_buffer_modes;
    struct List video_modes;
    ULONG sigmask;
    ULONG result;
    ULONG signals;
    ULONG timermask;
    UWORD code;
    int status;
    int have_rtg;
    int default_mode;
    char initial_drawer[256];
    int have_initial_drawer;
    mr_play_options saved_options;
    int have_saved_options;
    ULONG initial_c2p, initial_h264, initial_laced, initial_scale;
    ULONG initial_audio_rate, initial_fast_buffer, initial_no_audio;
    ULONG initial_mono_audio, initial_video_mode, initial_skip_after;

    window_object = NULL;
    timermask = 0;
    file = NULL;
    mode = NULL;
    c2p = NULL;
    h264 = NULL;
    lace = NULL;
    scale = NULL;
    scale_label = NULL;
    audio_rate = NULL;
    fast_buffer = NULL;
    no_audio = NULL;
    mono_audio = NULL;
    video_mode = NULL;
    info = NULL;
    layout = NULL;
    controls = NULL;
    controls_top = NULL;
    buttons = NULL;
    transport = NULL;
    services = NULL;
    file_label = NULL;
    display_label = NULL;
    audio_track_label = NULL;
    sub_track_label = NULL;
    c2p_label = NULL;
    h264_label = NULL;
    audio_rate_label = NULL;
    fast_buffer_label = NULL;
    video_mode_label = NULL;
    play_button = NULL;
    pause_button = NULL;
    stop_button = NULL;
    ff_button = NULL;
    iptv_button = NULL;
    youtube_button = NULL;
    volume_slider = NULL;
    playlist_button = NULL;
    memset(&playlist_gui, 0, sizeof(playlist_gui));
    mr_playlist_init(&playlist);
    window = NULL;
    master_options = NULL;
    memset(&app_menu, 0, sizeof(app_menu));
    status = RETURN_FAIL;
    have_rtg = 0;
    default_mode = 0;
    mode_count = 0;

    modes.lh_Head = (struct Node *)&modes.lh_Tail;
    modes.lh_Tail = NULL;
    modes.lh_TailPred = (struct Node *)&modes.lh_Head;
    g_audio_labels.lh_Head = (struct Node *)&g_audio_labels.lh_Tail;   /* MKA1 */
    g_audio_labels.lh_Tail = NULL;
    g_audio_labels.lh_TailPred = (struct Node *)&g_audio_labels.lh_Head;
    g_sub_labels.lh_Head = (struct Node *)&g_sub_labels.lh_Tail;       /* S2 */
    g_sub_labels.lh_Tail = NULL;
    g_sub_labels.lh_TailPred = (struct Node *)&g_sub_labels.lh_Head;
    c2p_modes.lh_Head = (struct Node *)&c2p_modes.lh_Tail;
    c2p_modes.lh_Tail = NULL;
    c2p_modes.lh_TailPred = (struct Node *)&c2p_modes.lh_Head;
    h264_modes.lh_Head = (struct Node *)&h264_modes.lh_Tail;
    h264_modes.lh_Tail = NULL;
    h264_modes.lh_TailPred = (struct Node *)&h264_modes.lh_Head;
    scale_modes.lh_Head = (struct Node *)&scale_modes.lh_Tail;
    scale_modes.lh_Tail = NULL;
    scale_modes.lh_TailPred = (struct Node *)&scale_modes.lh_Head;
    skip_after_modes.lh_Head = (struct Node *)&skip_after_modes.lh_Tail;
    skip_after_modes.lh_Tail = NULL;
    skip_after_modes.lh_TailPred = (struct Node *)&skip_after_modes.lh_Head;
    skip_after_label = NULL;
    g_skip_after = NULL;
    audio_rate_modes.lh_Head = (struct Node *)&audio_rate_modes.lh_Tail;
    audio_rate_modes.lh_Tail = NULL;
    audio_rate_modes.lh_TailPred = (struct Node *)&audio_rate_modes.lh_Head;
    fast_buffer_modes.lh_Head = (struct Node *)&fast_buffer_modes.lh_Tail;
    fast_buffer_modes.lh_Tail = NULL;
    fast_buffer_modes.lh_TailPred = (struct Node *)&fast_buffer_modes.lh_Head;
    video_modes.lh_Head = (struct Node *)&video_modes.lh_Tail;
    video_modes.lh_Tail = NULL;
    video_modes.lh_TailPred = (struct Node *)&video_modes.lh_Head;

    if (!open_reaction_classes()) {
        fprintf(stderr, "MintVID: ReAction V%ld classes are not available.\n",
                (long)MRGUI_CLASS_VERSION);
        goto cleanup;
    }

    have_rtg = default_screen_is_rtg();
    (void)have_rtg;
    /* MintVID-pi P96W60: H.264 through the Pi only - HDMI cinema and HDMI
     * window (W:50).  The Amiga RGB entry (h264rgb) was taken out again:
     * whether the native output has a signal depends on the OS setup
     * (CaffeineOS skips the native monitor drivers), so it is not offered. */
    if (!add_mode_node(&modes, "PiStorm / VC Hard Fullscreen",
                       MR_DISPLAY_PISTORM_HARD_FULLSCREEN) ||
        !add_mode_node(&modes, "PiStorm / VC Hard Fenster",
                       MR_DISPLAY_PISTORM_HARD_WINDOW) ||
        !add_mode_node(&modes, "PiStorm / VC Hard Fullscreen (Paula)",
                       MR_DISPLAY_PISTORM_PAULA))
        goto cleanup;
    default_mode = 0;
    if (!add_c2p_node(&c2p_modes, "Standard", MR_C2P_STANDARD) ||
        (mr_akiko_available() &&
         !add_c2p_node(&c2p_modes, "CD32", MR_C2P_AKIKO)) ||
        !add_c2p_node(&c2p_modes, "Kalms", MR_C2P_KALMS) ||
        /* The only other --2x-and-scale_2x-independent backend besides
         * Kalms - i.e. the one non-CD32 choice that actually qualifies for
         * Copper 2x (display_aga.c's copper_vdouble excludes Kalms/WPA/
         * Standard entirely, since only this portable mr_c2p8 kernel,
         * --riva-c2p and Akiko take an arbitrary output stride). Was never
         * exposed here before, silently leaving Copper 2x with no reachable
         * qualifying c2p choice on a non-CD32 machine. */
        !add_c2p_node(&c2p_modes, "Portable", MR_C2P_WPA)
#ifdef MR_KALMS_040
        || !add_c2p_node(&c2p_modes, "Direct", MR_C2P_DIRECT)
#endif
       )
        goto cleanup;
    if (!add_chooser_node(&h264_modes, "Auto") ||
        !add_chooser_node(&h264_modes, "Quality") ||
        !add_chooser_node(&h264_modes, "Balanced") ||
        !add_chooser_node(&h264_modes, "Fast") ||
        !add_chooser_node(&h264_modes, "Turbo") ||
        !add_chooser_node(&h264_modes, "Turbo+") ||
        !add_chooser_node(&h264_modes, "Smoosh"))
        goto cleanup;
    if (!add_chooser_node(&skip_after_modes, "0.2s") ||
        !add_chooser_node(&skip_after_modes, "0.5s") ||
        !add_chooser_node(&skip_after_modes, "0.7s") ||
        !add_chooser_node(&skip_after_modes, "1.0s") ||
        !add_chooser_node(&skip_after_modes, "1.5s") ||
        !add_chooser_node(&skip_after_modes, "2.0s"))
        goto cleanup;
    /* Rows are fixed, unlike mode/c2p - no value-array indirection needed;
     * read_play_options() and update_mode_controls() both hard-code
     * 0=None, 1=2x, 2=Copper 2x to match this exact order. */
    if (!add_chooser_node(&scale_modes, "None") ||
        !add_chooser_node(&scale_modes, "2x") ||
        !add_chooser_node(&scale_modes, "Copper 2x"))
        goto cleanup;
    if (!add_chooser_node(&audio_rate_modes, "Normal") ||
        !add_chooser_node(&audio_rate_modes, "Low"))
        goto cleanup;
    if (!add_chooser_node(&fast_buffer_modes, "Auto") ||
        !add_chooser_node(&fast_buffer_modes, "Off") ||
        !add_chooser_node(&fast_buffer_modes, "4 MB") ||
        !add_chooser_node(&fast_buffer_modes, "8 MB") ||
        !add_chooser_node(&fast_buffer_modes, "16 MB") ||
        !add_chooser_node(&fast_buffer_modes, "32 MB") ||
        !add_chooser_node(&fast_buffer_modes, "64 MB"))
        goto cleanup;
    /* Rows are fixed, like Scale: 0=All Frames, 1=Skip Frames -
     * read_play_options() hard-codes this exact order. All Frames (never
     * skip a decoded frame purely for falling behind the playback clock -
     * see mr_play_options.h's own throughput field) is the default and the
     * real-hardware-confirmed fix for a live/network stream that can't
     * decode in real time; see CLAUDE.md's "Live HLS playback stall
     * notes". */
    if (!add_chooser_node(&video_modes, "All Frames") ||
        !add_chooser_node(&video_modes, "Skip Frames"))
        goto cleanup;

    /* Restore the last-saved controller settings (mr_saved_options.h),
     * falling back to today's hardware-detected defaults field by field
     * when there's nothing saved yet, or a saved value no longer fits this
     * session (e.g. a saved display mode that isn't in modes[] this run -
     * mode_row() reports that with -1 rather than a fallback row, unlike
     * c2p_row()). update_mode_controls(), called once window_open()
     * finishes below, already clamps any combination that isn't actually
     * eligible (Copper 2x needing a qualifying c2p/mode, etc.) - the same
     * safety net a manual click through these controls relies on. */
    have_saved_options = mr_saved_options_load(&saved_options);
    if (have_saved_options) {
        /* Settings saved by the old Video: Off row have no_video set with
         * an ordinary display; they now mean the No Video display. */
        int saved_mode_row = mode_row(saved_options.no_video
                                      ? MR_DISPLAY_NONE
                                      : saved_options.display);
        if (saved_mode_row >= 0)
            default_mode = saved_mode_row;
    }
    initial_c2p = have_saved_options ? c2p_row(saved_options.c2p)
                                     : c2p_row(MR_C2P_KALMS);
    initial_h264 = have_saved_options &&
                   saved_options.h264_performance <= MR_H264_PERF_SMOOSH
                 ? (ULONG)saved_options.h264_performance
                 : (ULONG)MR_H264_PERF_TURBO;
    initial_laced = have_saved_options && saved_options.laced ? TRUE : FALSE;
    initial_scale = have_saved_options
                  ? (saved_options.copper_vdouble ? 2 :
                     saved_options.scale_2x ? 1 : 0)
                  : 0;
    initial_audio_rate = have_saved_options &&
                         saved_options.audio_rate == MR_AUDIO_RATE_LOW ? 1 : 0;
    initial_fast_buffer = have_saved_options &&
                          saved_options.fast_buffer <= MR_FAST_BUFFER_64MB
                        ? (ULONG)saved_options.fast_buffer
                        : (ULONG)MR_FAST_BUFFER_AUTO;
    initial_no_audio = have_saved_options && saved_options.no_audio
                      ? TRUE : FALSE;
    initial_mono_audio = have_saved_options && saved_options.mono_audio
                        ? TRUE : FALSE;
    initial_video_mode = have_saved_options && !saved_options.throughput
                       ? 1 : 0;
    initial_skip_after = skip_after_row(have_saved_options
                                        ? saved_options.skip_trigger_ms
                                        : MR_SKIP_TRIGGER_DEFAULT_MS);

    /* Seed the embedded file requester's starting drawer from the last one
     * used, saved via mr_last_dir_save() in update_file_info() below.
     * GETFILE_Drawer is a real ReAction getfile.gadget tag, but - like every
     * NDK-only detail in this file - there is no AmigaOS toolchain on this
     * dev host to check its exact behaviour against (see CLAUDE.md's
     * "Validate against ffmpeg" section); the real m68k-amigaos-gcc CI build
     * and a real-hardware run are what actually confirm it. */
    have_initial_drawer = mr_last_dir_load(initial_drawer,
                                           sizeof(initial_drawer));
    file = have_initial_drawer
        ? (Object *)NewObject(GETFILE_GetClass(), NULL,
                              GA_ID, G_FILE,
                              GA_RelVerify, TRUE,
                              GETFILE_TitleText, (ULONG)"Choose a video",
                              GETFILE_Pattern, (ULONG)MR_VIDEO_FILE_PATTERN,
                              GETFILE_DoPatterns, TRUE,
                              GETFILE_ReadOnly, TRUE,
                              GETFILE_DrawersOnly, FALSE,
                              GETFILE_Drawer, (ULONG)initial_drawer,
                              TAG_DONE)
        : (Object *)NewObject(GETFILE_GetClass(), NULL,
                              GA_ID, G_FILE,
                              GA_RelVerify, TRUE,
                              GETFILE_TitleText, (ULONG)"Choose a video",
                              GETFILE_Pattern, (ULONG)MR_VIDEO_FILE_PATTERN,
                              GETFILE_DoPatterns, TRUE,
                              GETFILE_ReadOnly, TRUE,
                              GETFILE_DrawersOnly, FALSE,
                              TAG_DONE);
    mode = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                               GA_ID, G_MODE,
                               GA_RelVerify, TRUE,
                               CHOOSER_Labels, (ULONG)&modes,
                               CHOOSER_Selected, default_mode,
                               TAG_DONE);
    g_mode = mode;
    /* MKA1: audio track chooser, filled per film (mrg_audio_chooser_update) */
    strcpy(g_audio_text[0], "-");
    add_chooser_node(&g_audio_labels, g_audio_text[0]);
    g_audio_ch = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                                     GA_ID, G_AUDIO_TRACK,
                                     GA_RelVerify, TRUE,
                                     GA_Disabled, TRUE,
                                     CHOOSER_Labels, (ULONG)&g_audio_labels,
                                     CHOOSER_Selected, 0,
                                     TAG_DONE);
    audio_track_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                            LABEL_Text, (ULONG)"Audio",
                                            TAG_DONE);
    /* S2: subtitle chooser, filled per film (mrg_sub_chooser_update) */
    strcpy(g_sub_text[0], "-");
    add_chooser_node(&g_sub_labels, g_sub_text[0]);
    g_sub_ch = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                                   GA_ID, G_SUB_TRACK,
                                   GA_RelVerify, TRUE,
                                   GA_Disabled, TRUE,
                                   CHOOSER_Labels, (ULONG)&g_sub_labels,
                                   CHOOSER_Selected, 0,
                                   TAG_DONE);
    sub_track_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                          LABEL_Text, (ULONG)"Subtitles",
                                          TAG_DONE);
    c2p = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                              GA_ID, G_C2P,
                              GA_RelVerify, TRUE,
                              CHOOSER_Labels, (ULONG)&c2p_modes,
                              CHOOSER_Selected, initial_c2p,
                              TAG_DONE);
    h264 = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                               GA_ID, G_H264,
                               GA_RelVerify, TRUE,
                               CHOOSER_Labels, (ULONG)&h264_modes,
                               CHOOSER_Selected, initial_h264,
                               TAG_DONE);
    lace = (Object *)NewObject(CHECKBOX_GetClass(), NULL,
                               GA_ID, G_LACE,
                               GA_Text, (ULONG)"Laced",
                               GA_RelVerify, TRUE,
                               CHECKBOX_Checked, initial_laced,
                               TAG_DONE);
    scale = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                                GA_ID, G_SCALE,
                                GA_RelVerify, TRUE,
                                CHOOSER_Labels, (ULONG)&scale_modes,
                                CHOOSER_Selected, initial_scale,
                                TAG_DONE);
    scale_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                      LABEL_Text, (ULONG)"Scale",
                                      TAG_DONE);
    audio_rate = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                                     GA_ID, G_AUDIO_RATE,
                                     GA_RelVerify, TRUE,
                                     CHOOSER_Labels, (ULONG)&audio_rate_modes,
                                     CHOOSER_Selected, initial_audio_rate,
                                     TAG_DONE);
    fast_buffer = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                                      GA_ID, G_FAST_BUFFER,
                                      GA_RelVerify, TRUE,
                                      CHOOSER_Labels, (ULONG)&fast_buffer_modes,
                                      CHOOSER_Selected, initial_fast_buffer,
                                      TAG_DONE);
    no_audio = (Object *)NewObject(CHECKBOX_GetClass(), NULL,
                                   GA_ID, G_NO_AUDIO,
                                   GA_Text, (ULONG)"No audio",
                                   GA_RelVerify, TRUE,
                                   CHECKBOX_Checked, initial_no_audio,
                                   TAG_DONE);
    mono_audio = (Object *)NewObject(CHECKBOX_GetClass(), NULL,
                                     GA_ID, G_MONO_AUDIO,
                                     GA_Text, (ULONG)"Mono audio",
                                     GA_RelVerify, TRUE,
                                     CHECKBOX_Checked, initial_mono_audio,
                                     TAG_DONE);
    video_mode = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                                     GA_ID, G_VIDEO_MODE,
                                     GA_RelVerify, TRUE,
                                     CHOOSER_Labels, (ULONG)&video_modes,
                                     CHOOSER_Selected, initial_video_mode,
                                     TAG_DONE);
    g_skip_after = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                                       GA_ID, G_SKIP_AFTER,
                                       GA_RelVerify, TRUE,
                                       CHOOSER_Labels, (ULONG)&skip_after_modes,
                                       CHOOSER_Selected, initial_skip_after,
                                       TAG_DONE);
    skip_after_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                           LABEL_Text, (ULONG)"Skip after",
                                           TAG_DONE);
    g_info_film = (Object *)NewObject(STRING_GetClass(), NULL,
                               GA_ReadOnly, TRUE,
                               STRINGA_TextVal, (ULONG)"No file selected",
                               STRINGA_MaxChars, 640,
                               TAG_DONE);
    info = (Object *)NewObject(STRING_GetClass(), NULL,
                               GA_ReadOnly, TRUE,
                               STRINGA_TextVal, (ULONG)"",
                               STRINGA_MaxChars, 640,
                               TAG_DONE);
    g_info_status = info;
    if (!g_info_film)
        goto cleanup;
    file_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                     LABEL_Text, (ULONG)"File",
                                     TAG_DONE);
    display_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                        LABEL_Text, (ULONG)"Display",
                                        TAG_DONE);
    c2p_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                    LABEL_Text, (ULONG)"C2P",
                                    TAG_DONE);
    /* "VQ" (Video Quality), not "H.264" - still H.264's own speed dial
     * internally (h264_performance/G_H264/MR_H264_PERF_*, unrenamed - see
     * CLAUDE.md's "DV decode speed" notes), but now also drives DV's
     * mr_dv_set_speed_mode() generically via mr_video_quality_prefers_fast()
     * (core/mr_play_options.c), so the on-screen label no longer names one
     * specific codec. */
    h264_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                     LABEL_Text, (ULONG)"VQ",
                                     TAG_DONE);
    audio_rate_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                           LABEL_Text, (ULONG)"Audio rate",
                                           TAG_DONE);
    fast_buffer_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                            LABEL_Text, (ULONG)"Fast buffer",
                                            TAG_DONE);
    video_mode_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                           LABEL_Text, (ULONG)"Video",
                                           TAG_DONE);

    if (!file || !mode || !c2p || !h264 || !lace || !scale ||
        !audio_rate || !fast_buffer || !no_audio || !mono_audio ||
        !video_mode || !g_skip_after || !skip_after_label || !info ||
        !file_label || !display_label || !c2p_label || !h264_label ||
        !scale_label || !audio_rate_label || !fast_buffer_label ||
        !video_mode_label || !g_audio_ch || !audio_track_label ||
        !g_sub_ch || !sub_track_label)
        goto cleanup;

    /* Keep the option area comfortably inside a 640-pixel A1200 Workbench.
     * The tightly-related display/C2P/H.264 controls stay together on the
     * first row; audio and buffering live on the second row. Scale (None/
     * 2x/Copper 2x) replaces the old separate 2x and Copper 2x checkboxes -
     * Copper only ever does anything on top of 2x, and only for a c2p/
     * display combination that supports it (update_mode_controls() snaps
     * the selection back to plain 2x otherwise), so one three-way chooser
     * reads better than two checkboxes whose relationship wasn't visible. */
    controls_top = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                       LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
                                       LAYOUT_SpaceInner, TRUE,
                                       LAYOUT_AddChild, (ULONG)mode,
                                       CHILD_Label, (ULONG)display_label,
                                       LAYOUT_AddChild, (ULONG)g_audio_ch,   /* MKA1 */
                                       CHILD_Label, (ULONG)audio_track_label,
                                       LAYOUT_AddChild, (ULONG)g_sub_ch,     /* S2 */
                                       CHILD_Label, (ULONG)sub_track_label,
                                       TAG_DONE);
    if (!controls_top)
        goto cleanup;

    controls = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                    LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
                                    LAYOUT_SpaceInner, TRUE,
                                    LAYOUT_AddChild, (ULONG)controls_top,
                                    CHILD_WeightedHeight, 0,
                                    TAG_DONE);
    if (!controls)
        goto cleanup;

    play_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                      GA_ID, G_PLAY,
                                      GA_Text, (ULONG)">",
                                      GA_RelVerify, TRUE,
                                      TAG_DONE);
    /* P96W58: G_PAUSE is the Fullscreen button now; Play/Pause is one. */
    pause_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                       GA_ID, G_PAUSE,
                                       GA_Text, (ULONG)"Full",
                                       GA_RelVerify, TRUE,
                                       GA_Disabled, TRUE,
                                       TAG_DONE);
    stop_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                      GA_ID, G_STOP,
                                      GA_Text, (ULONG)"[]",
                                      GA_RelVerify, TRUE,
                                      TAG_DONE);
    ff_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                    GA_ID, G_FF,
                                    GA_Text, (ULONG)">>",
                                    GA_RelVerify, TRUE,
                                    GA_Disabled, TRUE,
                                    TAG_DONE);
    /* MintVID-pi P96W59: IPTV/YouTube (classic mrplay path) are left out
     * for now - mrberry cannot open network streams yet. */
    iptv_button = NULL;
    youtube_button = NULL;
    /* P96W57: the wide slider is the start-time timeline now; volume
     * (only used by the mrplay path - mrberry plays over HDMI, the TV
     * remote sets the volume) is a narrow vertical slider. */
    g_time_slider = (Object *)NewObject(SLIDER_GetClass(), NULL,
                                        GA_ID, G_TIME,
                                        GA_RelVerify, TRUE,
                                        GA_Disabled, TRUE,
                                        SLIDER_Min, 0,
                                        SLIDER_Max, 1,
                                        SLIDER_Level, 0,
                                        SLIDER_Orientation, SLIDER_HORIZONTAL,
                                        TAG_DONE);
    strcpy(g_time_text, "T -:--:--");
    g_time_label = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                       GA_ID, G_TIME_LABEL,
                                       GA_Text, (ULONG)g_time_text,
                                       GA_ReadOnly, TRUE,
                                       TAG_DONE);
    /* P96W59: no volume slider - mrberry plays over HDMI (TV remote),
     * h264rgb has no sound. */
    volume_slider = NULL;
    playlist_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                          GA_ID, G_PLAYLIST,
                                          GA_Text, (ULONG)"Playlist",
                                          GA_RelVerify, TRUE, TAG_DONE);
    g_play_button = play_button;
    g_full_button = pause_button;
    g_ff_button = ff_button;
    g_ff_enabled = 0;
    if (!play_button || !pause_button || !stop_button || !ff_button ||
        !g_time_slider || !g_time_label || !playlist_button)
        goto cleanup;

    /* VLC-style lower strip: fixed media buttons on the left, a real
     * horizontal volume slider in the middle, and browser/playlist actions
     * grouped tightly on the right. Explicit min/max widths stop ReAction
     * distributing the small buttons across the full window. */
    transport = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                     LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
                                     LAYOUT_SpaceInner, TRUE,
                                     LAYOUT_AddChild, (ULONG)play_button,
                                     CHILD_WeightedWidth, 0,
                                     CHILD_MinWidth, 30, CHILD_MaxWidth, 30,
                                     LAYOUT_AddChild, (ULONG)pause_button,
                                     CHILD_WeightedWidth, 0,
                                     CHILD_MinWidth, 44, CHILD_MaxWidth, 44,
                                     LAYOUT_AddChild, (ULONG)stop_button,
                                     CHILD_WeightedWidth, 0,
                                     CHILD_MinWidth, 30, CHILD_MaxWidth, 30,
                                     LAYOUT_AddChild, (ULONG)ff_button,
                                     CHILD_WeightedWidth, 0,
                                     CHILD_MinWidth, 34, CHILD_MaxWidth, 34,
                                     LAYOUT_AddChild, (ULONG)g_time_slider,
                                     CHILD_WeightedWidth, 1,
                                     CHILD_MinWidth, 110,
                                     LAYOUT_AddChild, (ULONG)g_time_label,
                                     CHILD_WeightedWidth, 0,
                                     CHILD_MinWidth, 92, CHILD_MaxWidth, 92,
                                     TAG_DONE);
    if (!transport)
        goto cleanup;

    services = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                    LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
                                    LAYOUT_SpaceInner, TRUE,
                                    LAYOUT_AddChild, (ULONG)playlist_button,
                                    CHILD_WeightedWidth, 0,
                                    CHILD_MinWidth, 76, CHILD_MaxWidth, 76,
                                    TAG_DONE);
    if (!services)
        goto cleanup;

    buttons = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                   LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
                                   LAYOUT_SpaceInner, TRUE,
                                   LAYOUT_AddChild, (ULONG)transport,
                                   CHILD_WeightedWidth, 1,
                                   LAYOUT_AddChild, (ULONG)services,
                                   CHILD_WeightedWidth, 0,
                                   TAG_DONE);
    if (!buttons)
        goto cleanup;

    layout = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                  LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
                                  LAYOUT_SpaceOuter, TRUE,
                                  LAYOUT_SpaceInner, TRUE,
                                  LAYOUT_AddChild, (ULONG)file,
                                  CHILD_Label, (ULONG)file_label,
                                  CHILD_WeightedHeight, 0,
                                  LAYOUT_AddChild, (ULONG)controls,
                                  CHILD_WeightedHeight, 0,
                                  LAYOUT_AddChild, (ULONG)buttons,
                                  CHILD_WeightedHeight, 0,
                                  LAYOUT_AddChild, (ULONG)g_info_film,
                                  CHILD_WeightedHeight, 0,
                                  LAYOUT_AddChild, (ULONG)info,
                                  CHILD_WeightedHeight, 0,
                                  TAG_DONE);
    if (!layout)
        goto cleanup;

    window_object = (Object *)NewObject(WINDOW_GetClass(), NULL,
                                         WA_Title,
                                         (ULONG)"MintVID-pi Control",
                                         WA_Activate, TRUE,
                                         WA_DepthGadget, TRUE,
                                         WA_DragBar, TRUE,
                                         WA_CloseGadget, TRUE,
                                         WA_SizeGadget, TRUE,
                                         WA_IDCMP,
                                         IDCMP_GADGETUP |
                                         IDCMP_CLOSEWINDOW |
                                         IDCMP_MENUPICK |
                                         IDCMP_IDCMPUPDATE |
                                         IDCMP_REFRESHWINDOW,
                                         WINDOW_Position, WPOS_CENTERSCREEN,
                                         WINDOW_LockHeight, TRUE,   /* P96W72 */
                                         WINDOW_ParentGroup, (ULONG)layout,
                                         TAG_DONE);
    if (!window_object)
        goto cleanup;

    window = (struct Window *)RA_OpenWindow(window_object);
    if (!window)
        goto cleanup;
    mr_gui_menu_with_paula = 1;        /* P96W73 */
    mr_gui_menu_open(&app_menu, window);

    update_mode_controls(mode, c2p, lace, scale, window, TRUE);
    update_skip_after(mode, h264, video_mode, window);
    master_options = mr_master_options_open();
    publish_play_options(master_options, mode, c2p, h264, lace, scale,
                         audio_rate, fast_buffer, no_audio, mono_audio,
                         video_mode);
    GetAttr(WINDOW_SigMask, window_object, &sigmask);
    if (status_timer_open()) {
        timermask = 1UL << status_timer_port->mp_SigBit;
        status_timer_start();
    }

    for (;;) {
        signals = Wait(sigmask | timermask | SIGBREAKF_CTRL_C);
        if (signals & SIGBREAKF_CTRL_C)
            break;
        if (timermask && (signals & timermask)) {
            while (GetMsg(status_timer_port))
                ;
            status_timer_running = 0;
            poll_player_status(info, window);
            poll_iptv_launch(info, window, iptv_button);
            /* P96W58: when the player process is gone, a stop has written
             * a new <film>.t - read it again so the timeline (and Play)
             * continue from there.  (W57 waited for the ENDED status,
             * which did not move the slider on hardware.) */
            {
                int running = find_player() != NULL;
                if (g_player_was_running && !running && g_film_path[0]) {
                    /* P96W79: read the last status once more - the player may
                     * have written "Paused"/"Resume" and ended in this tick */
                    poll_player_status(info, window);
                    if (g_tab_req) {
                        /* v96.85: Tab - mrberry saved the target in the .t;
                         * start again there with the same display */
                        int row = mode_row((mr_display_mode)g_launch_display);
                        g_tab_req = 0;
                        g_pos_hold = 0;
                        g_slider_touched = 0;
                        if (row >= 0)
                            mrg_fullscreen_restart(file, mode, c2p, h264, lace, scale,
                                                   audio_rate, fast_buffer, no_audio,
                                                   mono_audio, video_mode, info,
                                                   window, row);
                        else
                            mrg_film_info_apply(g_film_path, info, window, 0);
                    } else if (g_full_req) {
                        /* v96.82: F in the video window -> full screen */
                        int row = mode_row(MR_DISPLAY_PISTORM_HARD_FULLSCREEN);
                        g_full_req = 0;
                        if (row >= 0 && g_launch_display == MR_DISPLAY_PISTORM_HARD_WINDOW)
                            mrg_fullscreen_restart(file, mode, c2p, h264, lace, scale,
                                                   audio_rate, fast_buffer, no_audio,
                                                   mono_audio, video_mode, info,
                                                   window, row);
                        else
                            mrg_film_info_apply(g_film_path, info, window, 0);
                    } else if (g_still_active) {
                        /* P96W79: the still ended (Space = continue, or
                         * Esc / window closed = stay paused, no picture) */
                        int resume = g_still_resume;
                        g_still_active = 0;
                        g_still_resume = 0;
                        g_player_paused = 0;
                        if (resume)        /* .t unchanged: slider as it is */
                            start_player(file, mode, c2p, h264, lace, scale,
                                         audio_rate, fast_buffer, no_audio,
                                         mono_audio, video_mode, info, window);
                        else if (g_paula_paused)
                            set_info(info, window, "Paused - press > to continue.");
                    } else {
                        mrg_film_info_apply(g_film_path, info, window, 0);
                        /* P96W78: slider now on the pause spot (.t) */
                        if (g_paula_paused) {
                            set_info(info, window, "Paused - press > to continue.");
                            if (g_film_seek)          /* P96W79: still picture */
                                mrg_start_still(info, window);
                        }
                    }
                    running = find_player() != NULL;
                }
                g_player_was_running = running;
                if (running) mrg_pos_poll(window);
                else { g_pos_hold = 0; g_pos_last = (ULONG)-1; g_slider_touched = 0; }
                mrg_buttons_update(window);
            }
            status_timer_start();
        }

        while ((result = RA_HandleInput(window_object, &code)) !=
               WMHI_LASTMSG) {
            switch (result & WMHI_CLASSMASK) {
            case WMHI_CLOSEWINDOW:
                goto done;

            case WMHI_MENUPICK: {
                int action = mr_gui_menu_action(&app_menu, code);
                if (action == MR_GUI_MENU_ABOUT)
                    mr_gui_show_about(window, "ReAction edition");
                else if (action == MR_GUI_MENU_GUIDE)
                    mr_gui_open_guide(&app_menu, window);
                else if (action == MR_GUI_MENU_QUIT)
                    goto done;
                else if (action == MR_GUI_MENU_PAULA)
                    mrg_sound_line(info, window);   /* P96W73: new base */
                break;
            }

            case WMHI_GADGETUP:
                switch (result & WMHI_GADGETMASK) {
                case G_FILE:
                    if (gfRequestFile(file, window))
                        update_file_info(file, info, window);
                    break;

                case G_AUDIO_TRACK: {
                    /* MKA1: new audio track.  Before the start: read INFO for
                     * it (sound line, playable).  While a film plays: it is
                     * used at the next start or ">>" (live switch = A2). */
                    ULONG sel = 0;
                    GetAttr(CHOOSER_Selected, g_audio_ch, &sel);
                    if (g_audio_n < 2 || (int)sel + 1 == g_audio_sel) break;
                    if (!mrg_audio_track_ok((int)sel + 1)) {
                        /* MKA1c: not with this display -> back to the old one */
                        static char msg[96];
                        int n = (int)sel + 1;
                        if (g_audio_paula[n - 1] && !mrg_paula_selected())
                            snprintf(msg, sizeof msg,
                                     "Audio track %d plays only with Fullscreen (Paula).", n);
                        else if (g_audio_hdmi[n - 1] && mrg_paula_selected())
                            snprintf(msg, sizeof msg,
                                     "Audio track %d plays only over HDMI, not Paula.", n);
                        else
                            snprintf(msg, sizeof msg, "Audio track %d cannot be played.", n);
                        SetGadgetAttrs((struct Gadget *)g_audio_ch, window, NULL,
                                       CHOOSER_Selected,
                                       (ULONG)(g_audio_sel ? g_audio_sel - 1 : 0), TAG_DONE);
                        set_info(info, window, msg);
                        break;
                    }
                    g_audio_sel = (int)sel + 1;
                    if (find_player()) {
                        set_info(info, window,
                                 "Audio track is used after >> or the next start.");
                    } else if (g_film_path[0]) {
                        ULONG level = 0;
                        if (g_time_slider) GetAttr(SLIDER_Level, g_time_slider, &level);
                        g_info_want_track = g_audio_sel;
                        SetWindowPointer(window, WA_BusyPointer, TRUE, TAG_DONE);
                        mrg_film_info_apply(g_film_path, info, window, 1);
                        SetWindowPointer(window, TAG_DONE);
                        if (g_time_slider && g_film_seek) {   /* keep the start time */
                            SetGadgetAttrs((struct Gadget *)g_time_slider, window, NULL,
                                           SLIDER_Level, level, TAG_DONE);
                            mrg_time_label_update(window);
                        }
                    }
                    break;
                }

                case G_SUB_TRACK: {
                    /* S2: subtitle file for this film (used at the next
                     * start or ">>"; mrberry keeps it in the .t as S:) */
                    ULONG sel = 0;
                    GetAttr(CHOOSER_Selected, g_sub_ch, &sel);
                    if (g_sub_n < 1 || (int)sel == g_sub_sel) break;
                    g_sub_sel = (int)sel;
                    g_sub_touched = 1;
                    if (find_player())
                        set_info(info, window, "Subtitles are used after >> or the next start.");
                    break;
                }

                case G_MODE:
                    mrg_time_enable(window);
                    mrg_sub_enable(window);           /* S2b */
                    if (mrg_audio_fix_for_display(window) && /* MKA1c */
                        !find_player() && g_film_path[0]) {
                        ULONG level = 0;
                        if (g_time_slider) GetAttr(SLIDER_Level, g_time_slider, &level);
                        g_info_want_track = g_audio_sel;
                        mrg_film_info_apply(g_film_path, info, window, 1);
                        if (g_time_slider && g_film_seek) {
                            SetGadgetAttrs((struct Gadget *)g_time_slider, window, NULL,
                                           SLIDER_Level, level, TAG_DONE);
                            mrg_time_label_update(window);
                        }
                    }
                    mrg_sound_line(info, window);      /* P96W72: HDMI/Paula */
                    update_mode_controls(mode, c2p, lace, scale, window, TRUE);
                    update_skip_after(mode, h264, video_mode, window);
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    break;

                case G_C2P:
                    update_mode_controls(mode, c2p, lace, scale, window, FALSE);
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    break;

                case G_SCALE:
                    /* Re-validate here too (not just on G_MODE/G_C2P): picking
                     * an incompatible c2p/mode first and only then Copper 2x
                     * must snap back just as reliably as the reverse order. */
                    update_mode_controls(mode, c2p, lace, scale, window, FALSE);
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    break;

                case G_H264:
                case G_VIDEO_MODE:
                    update_skip_after(mode, h264, video_mode, window);
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    break;

                case G_LACE:
                case G_AUDIO_RATE:
                case G_FAST_BUFFER:
                case G_NO_AUDIO:
                case G_MONO_AUDIO:
                case G_SKIP_AFTER:
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    break;

                case G_PLAY:
                    /* P96W58: one button - start, or pause/continue */
                    if (find_player() && g_still_active) {
                        /* P96W79: end the still, continue with Paula */
                        if (mrg_stop_still()) {
                            /* .t is unchanged; the slider keeps the pause
                             * spot or the time the user chose meanwhile */
                            start_player(file, mode, c2p, h264, lace, scale,
                                         audio_rate, fast_buffer, no_audio,
                                         mono_audio, video_mode, info, window);
                        } else {
                            set_info(info, window, "The still picture did not end - press [] first.");
                        }
                    } else if (find_player() &&
                        g_launch_display == MR_DISPLAY_PISTORM_RGB) {
                        set_info(info, window, "Amiga RGB output has no pause.");
                    } else if (find_player() &&
                               g_launch_display == MR_DISPLAY_PISTORM_PAULA) {
                        /* P96W78: mrberry stops at the spot (Ctrl-D) */
                        signal_player(SIGBREAKF_CTRL_D);
                        set_info(info, window, "Pausing...");
                    } else if (find_player() && g_player_paused && g_slider_touched &&
                               g_film_seek &&
                               (g_launch_display == MR_DISPLAY_PISTORM_HARD_WINDOW ||
                                g_launch_display == MR_DISPLAY_PISTORM_HARD_FULLSCREEN)) {
                        /* MKA1d: paused and the timeline was moved -> ">"
                         * continues at the new time (same as ">>") */
                        g_slider_touched = 0;
                        mrg_jump(file, mode, c2p, h264, lace, scale,
                                 audio_rate, fast_buffer, no_audio,
                                 mono_audio, video_mode, info, window);
                    } else if (find_player()) {
                        signal_player(SIGBREAKF_CTRL_D);
                        g_player_paused = !g_player_paused;
                    } else {
                        start_player(file, mode, c2p, h264, lace, scale,
                                     audio_rate, fast_buffer, no_audio,
                                     mono_audio, video_mode, info, window);
                    }
                    mrg_buttons_update(window);
                    break;

                case G_PAUSE:      /* Fullscreen */
                    mrg_switch_fullscreen(file, mode, c2p, h264, lace, scale,
                                          audio_rate, fast_buffer, no_audio,
                                          mono_audio, video_mode, info, window);
                    break;

                case G_STOP:
                    stop_player_and_wait();
                    g_still_active = 0;       /* P96W79: [] ends the pause too */
                    g_still_resume = 0;
                    g_paula_paused = 0;
                    /* P96W58: mrberry has written <film>.t - read it now */
                    if (!find_player() && g_player_was_running && g_film_path[0]) {
                        g_player_was_running = 0;
                        mrg_film_info_apply(g_film_path, info, window, 0);
                    }
                    mrg_buttons_update(window);
                    break;

                case G_FF:         /* P96W61: jump to timeline time */
                    mrg_jump(file, mode, c2p, h264, lace, scale,
                             audio_rate, fast_buffer, no_audio,
                             mono_audio, video_mode, info, window);
                    break;

                case G_VOLUME:
                    apply_gui_volume(volume_slider);
                    break;

                case G_TIME:
                    mrg_time_label_update(window);
                    if (find_player()) {
                        g_pos_hold = MRG_POS_HOLD_TICKS;
                        g_slider_touched = 1;          /* MKA1d */
                    }
                    break;

                case G_PLAYLIST:
                    if (!playlist_gui.window_object &&
                        mrg_playlist_open(&playlist_gui, &playlist, mode, c2p,
                                          h264, lace, scale, audio_rate,
                                          fast_buffer, no_audio, mono_audio,
                                          video_mode, info, window))
                        sigmask |= playlist_gui.sigmask;
                    break;

                case G_IPTV:
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    start_iptv_launch(info, window, iptv_button);
                    open_iptv_browser(mode, c2p, h264, lace, scale,
                                      audio_rate, fast_buffer, no_audio,
                                      mono_audio, video_mode, info, window);
                    break;

                case G_YOUTUBE:
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    open_youtube_browser(mode, c2p, h264, lace, scale,
                                         audio_rate, fast_buffer, no_audio,
                                         mono_audio, video_mode, info, window);
                    break;

                default:
                    break;
                }
                break;

            default:
                break;
            }
        }
        if (playlist_gui.window_object &&
            (signals & playlist_gui.sigmask))
            mrg_playlist_handle(&playlist_gui);
    }

done:
    mrg_playlist_close(&playlist_gui);
    status = RETURN_OK;
    stop_player_and_wait();

cleanup:
    status_timer_close();
    mr_master_options_close(&master_options);
    mr_gui_menu_close(&app_menu, window);
    if (window_object) {
        if (window)
            RA_CloseWindow(window_object);
        DisposeObject(window_object);
    } else if (layout) {
        DisposeObject(layout);
    } else {
        if (controls) {
            DisposeObject(controls);
        } else {
            if (controls_top) {
                DisposeObject(controls_top);
            } else {
                if (mode) DisposeObject(mode);
                if (display_label) DisposeObject(display_label);
                if (g_audio_ch) DisposeObject(g_audio_ch);          /* MKA1 */
                if (audio_track_label) DisposeObject(audio_track_label);
                if (g_sub_ch) DisposeObject(g_sub_ch);              /* S2 */
                if (sub_track_label) DisposeObject(sub_track_label);
            }
        }

        if (buttons) {
            DisposeObject(buttons);
        } else {
            if (transport) {
                DisposeObject(transport);
            } else {
                if (play_button) DisposeObject(play_button);
                if (pause_button) DisposeObject(pause_button);
                if (stop_button) DisposeObject(stop_button);
                if (ff_button) DisposeObject(ff_button);
                if (volume_slider) DisposeObject(volume_slider);
                if (g_time_slider) DisposeObject(g_time_slider);
                if (g_time_label) DisposeObject(g_time_label);
            }
            if (services) {
                DisposeObject(services);
            } else {
                if (iptv_button) DisposeObject(iptv_button);
                if (youtube_button) DisposeObject(youtube_button);
                if (playlist_button) DisposeObject(playlist_button);
            }
        }

        if (file)
            DisposeObject(file);
        if (info)
            DisposeObject(info);
        if (g_info_film)
            DisposeObject(g_info_film);
        if (file_label)
            DisposeObject(file_label);
    }

    /* P96W59: the classic options were never put into the window layout,
     * so they are always disposed here on their own. */
    if (c2p) DisposeObject(c2p);
    if (h264) DisposeObject(h264);
    if (lace) DisposeObject(lace);
    if (scale) DisposeObject(scale);
    if (c2p_label) DisposeObject(c2p_label);
    if (h264_label) DisposeObject(h264_label);
    if (scale_label) DisposeObject(scale_label);
    if (audio_rate) DisposeObject(audio_rate);
    if (fast_buffer) DisposeObject(fast_buffer);
    if (no_audio) DisposeObject(no_audio);
    if (mono_audio) DisposeObject(mono_audio);
    if (video_mode) DisposeObject(video_mode);
    if (audio_rate_label) DisposeObject(audio_rate_label);
    if (fast_buffer_label) DisposeObject(fast_buffer_label);
    if (video_mode_label) DisposeObject(video_mode_label);
    if (g_skip_after) DisposeObject(g_skip_after);
    if (skip_after_label) DisposeObject(skip_after_label);

    free_chooser_nodes(&modes);
    free_chooser_nodes(&c2p_modes);
    free_chooser_nodes(&h264_modes);
    free_chooser_nodes(&scale_modes);
    free_chooser_nodes(&audio_rate_modes);
    free_chooser_nodes(&fast_buffer_modes);
    free_chooser_nodes(&video_modes);
    free_chooser_nodes(&skip_after_modes);
    free_chooser_nodes(&g_audio_labels);                 /* MKA1 */
    g_audio_ch = NULL;
    free_chooser_nodes(&g_sub_labels);                   /* S2 */
    g_sub_ch = NULL;
    g_skip_after = NULL;
    g_time_slider = NULL;
    g_time_label = NULL;
    g_play_button = NULL;
    g_info_film = NULL;
    g_info_status = NULL;
    g_full_button = NULL;
    g_ff_button = NULL;
    g_mode = NULL;
    close_reaction_classes();
    return status;
}
