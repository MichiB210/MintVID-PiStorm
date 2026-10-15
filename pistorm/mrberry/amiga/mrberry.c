/* MintVID demux -> berryvp.library hardware A/V bridge.
 * Stage 4F HUNKSAFE: the hardware-confirmed Stage-3D H.264 path is unchanged. AAC-LC packets are
 * demuxed by MintVID, wrapped as ADTS where required, decoded by BerryFAAD03
 * on ARM CPU1 and sent as PCM16 to MMAL audio_render/HDMI.
 *
 * MintVID external player: same task name, signals and status port as
 * mrplay, so MintVID's Play/Pause/Stop work; pause holds the feed, and a
 * jump (or --start=) restarts the stream at a keyframe. */
#include "../core/mr_demux.h"
#include "mr_berryvp.h"
#include "mrb_paula.h"
#include "mr_player_status.h"
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <clib/alib_protos.h>   /* NewList() (amiga.lib helper) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MR_DEBUG_LOGS
#define MR_DEBUG_LOGS 0
#endif

#if MR_DEBUG_LOGS
#define DBG_PRINTF(...) printf(__VA_ARGS__)
#else
#define DBG_PRINTF(...) ((void)0)
#endif

extern ULONG bvp_exec_setsignal(ULONG newSignals, ULONG signalSet);
extern ULONG ac309i_read_sys_us(void);

/* D4: keep the ~4 KiB BerryVP bridge context out of the Amiga CLI stack.
 * A normal Shell stack can be only 4096 bytes; the previous automatic
 * mr_berryvp made main reserve 4168 bytes before any useful work. */
static mr_berryvp g_bvp;


/* MintVID PiStorm04: keep compressed startup audio host-owned until the first
 * real H.264 access unit has successfully crossed BerryVP. Earlier standalone
 * VPr/VPv second-start paths always had video/VCHIQ traffic before the first
 * PCM BULK; N79 library mode can stall on a persistent second start when PCM
 * is the first real BULK. This queue changes only startup ordering. */
typedef struct p04_audio_node {
    struct p04_audio_node *next;
    uint8_t *data;
    uint32_t len;
    int has_pts;
    uint64_t pts_us;
} p04_audio_node;

static p04_audio_node *p04_ahead, *p04_atail;
static unsigned long p04_aqueued;
static unsigned long p04_abytes;

static int p04_queue_audio(const mr_packet *pkt)
{
    p04_audio_node *n;
    if (!pkt || !pkt->data || !pkt->len) return 0;
    n = (p04_audio_node *)malloc(sizeof *n);
    if (!n) return 0;
    memset(n, 0, sizeof *n);
    n->data = (uint8_t *)malloc(pkt->len);
    if (!n->data) { free(n); return 0; }
    memcpy(n->data, pkt->data, pkt->len);
    n->len = pkt->len;
    n->has_pts = pkt->has_pts;
    n->pts_us = pkt->pts_us;
    if (p04_atail) p04_atail->next = n; else p04_ahead = n;
    p04_atail = n;
    p04_aqueued++;
    p04_abytes += pkt->len;
    return 1;
}

static void p04_free_audio_queue(void)
{
    while (p04_ahead) {
        p04_audio_node *n = p04_ahead;
        p04_ahead = n->next;
        free(n->data);
        free(n);
    }
    p04_atail = NULL;
    p04_aqueued = 0;
    p04_abytes = 0;
}

static mr_status p04_flush_audio_queue(mr_berryvp *bvp,
                                       unsigned long *audio_sent)
{
    while (p04_ahead) {
        p04_audio_node *n = p04_ahead;
        mr_packet p;
        mr_status st;
        memset(&p, 0, sizeof p);
        p.is_video = 0;
        p.has_pts = n->has_pts;
        p.pts_us = n->pts_us;
        p.data = n->data;
        p.len = n->len;
        st = mr_berryvp_send_audio_packet(bvp, &p);
        if (st != MR_OK) return st;
        if (audio_sent) (*audio_sent)++;
        p04_ahead = n->next;
        if (!p04_ahead) p04_atail = NULL;
        p04_abytes -= n->len;
        free(n->data);
        free(n);
        p04_aqueued--;
    }
    return MR_OK;
}


/* ---- Player control: keys, signals and the MintVID status port ----------
 *
 * mrberry follows MintVID's external-player contract (docs/EXTERNAL_PLAYER.md
 * in MintVID): the GUI starts it as task "MintVID player", exactly like
 * mrplay, so the GUI's Play/Pause/Stop buttons and status line work without
 * any special case:
 *   Ctrl-C / Ctrl-F  stop
 *   Ctrl-D           pause / continue
 *   Ctrl-E           read the port's command bits (pause, jump forward)
 * The status lives in the same public port mrplay publishes
 * ("MintVID.player"), and the final state also goes to the T: file so the
 * GUI can still read it after mrberry has exited.
 *
 * Keys (the 1x1 input window, hidden under the VideoCore picture):
 *   ESC, 0      stop
 *   Space, P    pause / continue
 *   Right/Left  jump 10 s forward/back     (MP4/MOV and MKV only)
 *   Up/Down     jump 60 s forward/back */
enum {
    EV_NONE = 0,
    EV_STOP,
    EV_PAUSE,
    EV_SEEK
};

#define SEEK_SHORT_US 10000000LL
#define SEEK_LONG_US  60000000LL
#define MRBERRY_CODEC "H.264 / VideoCore"

static struct Window *p05_input_window;
static mr_player_control_port *control_block;
static ULONG status_file_seq;

static int p05_input_open(void)
{
    p05_input_window = OpenWindowTags(NULL,
        WA_Left, 0,
        WA_Top, 0,
        WA_Width, 1,
        WA_Height, 1,
        WA_Borderless, TRUE,
        WA_Activate, TRUE,
        WA_RMBTrap, TRUE,
        WA_IDCMP, IDCMP_RAWKEY | IDCMP_VANILLAKEY,
        TAG_DONE);
    return p05_input_window != NULL;
}

static void p05_input_close(void)
{
    if (p05_input_window) {
        struct IntuiMessage *msg;
        while ((msg = (struct IntuiMessage *)GetMsg(p05_input_window->UserPort)) != NULL)
            ReplyMsg((struct Message *)msg);
        CloseWindow(p05_input_window);
        p05_input_window = NULL;
    }
}

/* One pending event from the keyboard, the GUI's signals or the port's
 * command bits. A jump sets *seek_delta_us. Signals are cleared as they are
 * read (SetSignal(0, mask)), so one Ctrl-D is one pause toggle. */
static int poll_event(int64_t *seek_delta_us)
{
    struct IntuiMessage *msg;
    ULONG sig;
    int ev = EV_NONE;

    if (p05_input_window && p05_input_window->UserPort) {
        while ((msg = (struct IntuiMessage *)GetMsg(p05_input_window->UserPort)) != NULL) {
            ULONG cls = msg->Class;
            UWORD code = msg->Code;
            ReplyMsg((struct Message *)msg);
            if (ev == EV_STOP) continue;
            if (cls == IDCMP_RAWKEY) {
                if (code & 0x80u) continue;              /* key up */
                switch (code & 0x7fu) {
                case 0x45: ev = EV_STOP; break;          /* ESC          */
                case 0x4E: ev = EV_SEEK; *seek_delta_us = SEEK_SHORT_US; break;
                case 0x4F: ev = EV_SEEK; *seek_delta_us = -SEEK_SHORT_US; break;
                case 0x4C: ev = EV_SEEK; *seek_delta_us = SEEK_LONG_US; break;
                case 0x4D: ev = EV_SEEK; *seek_delta_us = -SEEK_LONG_US; break;
                default: break;
                }
            } else if (cls == IDCMP_VANILLAKEY) {
                if (code == 0x1bu || code == '0') ev = EV_STOP;
                else if (code == ' ' || code == 'p' || code == 'P')
                    ev = EV_PAUSE;
            }
        }
    }
    if (ev == EV_STOP) return ev;

    sig = bvp_exec_setsignal(0, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D |
                                SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F);
    if (sig & (SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F)) return EV_STOP;
    if (sig & SIGBREAKF_CTRL_D) return EV_PAUSE;
    if (sig & SIGBREAKF_CTRL_E) {
        ULONG commands = 0;
        if (control_block) {
            Forbid();
            commands = control_block->commands;
            control_block->commands = 0;
            Permit();
        }
        if (commands & MR_PLAYER_COMMAND_PAUSE) return EV_PAUSE;
        if ((commands & MR_PLAYER_COMMAND_FAST) || !commands) {
            *seek_delta_us = SEEK_SHORT_US;
            return EV_SEEK;
        }
        /* Volume and fullscreen bits do not apply to HDMI output. */
    }
    return ev;
}

static void control_port_close(void)
{
    if (control_block) {
        Forbid();
        RemPort(&control_block->port);
        Permit();
        FreeMem(control_block, sizeof *control_block);
        control_block = NULL;
    }
}

/* Publish MintVID's player port, unless another player already owns it. */
static void control_port_open(void)
{
    mr_player_control_port *block;
    Forbid();
    if (FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT)) { Permit(); return; }
    Permit();
    block = (mr_player_control_port *)AllocMem(sizeof *block,
                                               MEMF_PUBLIC | MEMF_CLEAR);
    if (!block) return;
    block->port.mp_Node.ln_Type = NT_MSGPORT;
    block->port.mp_Node.ln_Name = (char *)MR_IPTV_PLAYER_PORT;
    block->port.mp_Flags = PA_IGNORE;
    block->port.mp_SigTask = FindTask(NULL);
    NewList(&block->port.mp_MsgList);
    block->volume = 64;
    block->status.state = MR_PLAYER_STATE_STARTING;
    block->status.magic = MR_PLAYER_STATUS_MAGIC;
    AddPort(&block->port);
    control_block = block;
}

/* Live update of the port only: cheap enough to call once a second. */
static void status_live(LONG state, const char *text)
{
    mr_player_status_set(control_block, state, MRBERRY_CODEC, text);
}

/* State change: port plus the atomic T: snapshot that outlives mrberry. */
static void status_set(LONG state, const char *codec, const char *text)
{
    mr_player_status_snapshot snapshot;
    BPTR file;
    LONG written;

    mr_player_status_set(control_block, state, codec, text);
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.status.magic = MR_PLAYER_STATUS_MAGIC;
    snapshot.status.seq = ++status_file_seq;
    snapshot.status.state = state;
    mr_player_status_copy(snapshot.status.codec, sizeof snapshot.status.codec,
                          codec ? codec : "");
    mr_player_status_copy(snapshot.status.text, sizeof snapshot.status.text,
                          text ? text : "");
    snapshot.trailer_magic = MR_PLAYER_STATUS_MAGIC;

    file = Open((CONST_STRPTR)MR_PLAYER_STATUS_TMP, MODE_NEWFILE);
    if (!file) return;
    written = Write(file, &snapshot, (LONG)sizeof snapshot);
    Close(file);
    if (written == (LONG)sizeof snapshot) {
        DeleteFile((CONST_STRPTR)MR_PLAYER_STATUS_FILE);
        if (!Rename((CONST_STRPTR)MR_PLAYER_STATUS_TMP,
                    (CONST_STRPTR)MR_PLAYER_STATUS_FILE))
            DeleteFile((CONST_STRPTR)MR_PLAYER_STATUS_TMP);
    } else {
        DeleteFile((CONST_STRPTR)MR_PLAYER_STATUS_TMP);
    }
}

static void format_time(char *out, size_t cap, uint64_t us)
{
    unsigned long s = (unsigned long)(us / 1000000u);
    if (s >= 3600)
        snprintf(out, cap, "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(out, cap, "%lu:%02lu", s / 60, s % 60);
}

/* "H:MM:SS" / "HH:MM:SS" / "MM:SS" / plain seconds -> microseconds. */
static int parse_time(const char *s, uint64_t *out_us)
{
    uint64_t total = 0, part = 0;
    int digits = 0, fields = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            part = part * 10u + (uint64_t)(*s - '0');
            if (++digits > 9) return 0;
        } else if (*s == ':' || !*s) {
            if (!digits || ++fields > 3) return 0;
            total = total * 60u + part;
            part = 0; digits = 0;
            if (!*s) break;
        } else {
            return 0;
        }
    }
    *out_us = total * 1000000u;
    return 1;
}

static FILE *n76_hostlog;
static void n76_hlog(const char *s)
{
    if (n76_hostlog) { fputs(s, n76_hostlog); fputc('\n', n76_hostlog); fflush(n76_hostlog); }
}

typedef struct n73_video_node {
    struct n73_video_node *next;
    uint8_t *data;
    uint32_t len;
    int has_pts;
    uint64_t pts_us;
    int is_annexb;
} n73_video_node;

static n73_video_node *n73_vhead, *n73_vtail;
static unsigned long n73_vqueued;

static int n73_queue_video(const mr_packet *pkt)
{
    n73_video_node *n;
    if (!pkt || !pkt->data || !pkt->len) return 0;
    n = (n73_video_node *)malloc(sizeof *n);
    if (!n) return 0;
    memset(n, 0, sizeof *n);
    n->data = (uint8_t *)malloc(pkt->len);
    if (!n->data) { free(n); return 0; }
    memcpy(n->data, pkt->data, pkt->len);
    n->len = pkt->len;
    n->has_pts = pkt->has_pts;
    n->pts_us = pkt->pts_us;
    n->is_annexb = pkt->is_annexb;
    if (n73_vtail) n73_vtail->next = n; else n73_vhead = n;
    n73_vtail = n;
    n73_vqueued++;
    return 1;
}

static void n73_free_video_queue(void)
{
    while (n73_vhead) {
        n73_video_node *n = n73_vhead;
        n73_vhead = n->next;
        free(n->data);
        free(n);
    }
    n73_vtail = NULL;
    n73_vqueued = 0;
}

static mr_status n73_flush_video_queue(mr_berryvp *bvp,
                                       unsigned long *sent,
                                       unsigned long *skipped)
{
    while (n73_vhead) {
        n73_video_node *n = n73_vhead;
        mr_packet p;
        mr_status st;
        memset(&p, 0, sizeof p);
        p.is_video = 1;
        p.has_pts = n->has_pts;
        p.pts_us = n->pts_us;
        p.data = n->data;
        p.len = n->len;
        p.is_annexb = n->is_annexb;
        st = mr_berryvp_send_packet(bvp, &p);
        if (st == MR_EAGAIN) { if (skipped) (*skipped)++; }
        else if (st != MR_OK) return st;
        else if (sent) (*sent)++;
        n73_vhead = n->next;
        if (!n73_vhead) n73_vtail = NULL;
        free(n->data);
        free(n);
        n73_vqueued--;
    }
    return MR_OK;
}


/* AC309I: port of the hardware-confirmed VPv149 MP4 host-time feeder pacer.
 *
 * Presentation still belongs to BerryVP/video_scheduler and every original
 * MP4 stts/ctts PTS is passed through unchanged.  This clock has a different
 * job: INPUT BACKPRESSURE.  A fresh MOV/MP4 H.264 access unit may enter the
 * decoder inside a bounded 250-ms window before its absolute decode-order
 * frame deadline on the Pi 1-MHz system timer.  First AU is immediate; later deadlines stay on
 * an absolute rational grid derived from container rate/scale, so late work
 * never moves the clock.  This is the VPv102/VPv149 principle which removed
 * the historical animel.mp4 freeze.
 *
 * Unlike a blocking per-frame wait, not-due video stays in the host-owned AU
 * queue while the demux loop continues to deliver intervening audio packets.
 * Thus Audio/VCHIQ service remains alive while Video waits for its deadline.
 */
#define AC309I_FEED_LEAD_US 250000UL

typedef struct ac309i_pacer {
    int enabled;
    int started;
    uint32_t base_us;
    uint32_t target_us;
    uint32_t step_whole_us;
    uint32_t step_rem;
    uint32_t step_den;
    uint32_t accum;
    uint32_t lead_us;
    unsigned long sent_aus;
    unsigned long late_count;
    uint32_t max_late_us;
    unsigned long queue_peak;
} ac309i_pacer;

static uint32_t ac309i_next_interval(ac309i_pacer *p)
{
    uint32_t d = p->step_whole_us;
    if (p->step_rem && p->step_den) {
        uint32_t a = p->accum + p->step_rem;
        if (a >= p->step_den) { a -= p->step_den; d++; }
        p->accum = a;
    }
    return d ? d : 1u;
}

static void ac309i_pacer_init(ac309i_pacer *p, const mr_video_info *vi, int enable)
{
    uint64_t num;
    memset(p, 0, sizeof *p);
    if (!enable || !vi || !vi->rate || !vi->scale) return;
    num = 1000000ULL * (uint64_t)vi->scale;
    p->step_whole_us = (uint32_t)(num / (uint64_t)vi->rate);
    p->step_rem      = (uint32_t)(num % (uint64_t)vi->rate);
    p->step_den      = vi->rate;
    p->lead_us       = AC309I_FEED_LEAD_US;
    if (!p->step_whole_us && !p->step_rem) return;
    p->enabled = 1;
}

static int ac309i_video_due(const ac309i_pacer *p, uint32_t *elapsed_out)
{
    uint32_t elapsed, feed_deadline;
    if (!p->enabled || !p->started) {
        if (elapsed_out) *elapsed_out = 0;
        return 1;
    }
    elapsed = (uint32_t)(ac309i_read_sys_us() - p->base_us);
    if (elapsed_out) *elapsed_out = elapsed;
    /* AC309I: bounded decoder headroom. Presentation target/PTS is NOT moved.
     * Only decoder admission opens LEAD_US before the absolute frame target. */
    feed_deadline = (p->target_us > p->lead_us) ?
        (p->target_us - p->lead_us) : 0u;
    return elapsed >= feed_deadline;
}

static void ac309i_commit_video_deadline(ac309i_pacer *p, FILE *log)
{
    uint32_t now, elapsed, late = 0;
    if (!p->enabled) return;
    now = (uint32_t)ac309i_read_sys_us();
    if (!p->started) {
        p->base_us = now;
        p->started = 1;
        p->target_us = ac309i_next_interval(p);
        p->sent_aus = 1;
        if (log) {
            fprintf(log,
                    "AC309I_PACE_ANCHOR base=$%08lx next=%lu lead=%lu step=%lu rem=%lu/%lu\n",
                    (unsigned long)p->base_us, (unsigned long)p->target_us,
                    (unsigned long)p->lead_us, (unsigned long)p->step_whole_us,
                    (unsigned long)p->step_rem, (unsigned long)p->step_den);
            fflush(log);
        }
        return;
    }
    elapsed = (uint32_t)(now - p->base_us);
    {
        uint32_t feed_deadline = (p->target_us > p->lead_us) ?
            (p->target_us - p->lead_us) : 0u;
        if (elapsed > feed_deadline) {
            late = elapsed - feed_deadline;
            p->late_count++;
            if (late > p->max_late_us) p->max_late_us = late;
        }
    }
    p->target_us += ac309i_next_interval(p);
    p->sent_aus++;
    if (log && ((p->sent_aus & 255u) == 0u || (elapsed >= 45000000u && (p->sent_aus & 31u) == 0u))) {
        fprintf(log,
                "AC309I_PACE sent=%lu elapsed=%lu next=%lu lead=%lu queue=%lu feedlate=%lu maxfeedlate=%lu latecnt=%lu\n",
                p->sent_aus, (unsigned long)elapsed, (unsigned long)p->target_us,
                (unsigned long)p->lead_us, n73_vqueued, (unsigned long)late,
                (unsigned long)p->max_late_us, p->late_count);
        fflush(log);
    }
}

static mr_status ac309i_service_mov_video(mr_berryvp *bvp, ac309i_pacer *pace,
                                          unsigned long *sent,
                                          unsigned long *skipped)
{
    while (n73_vhead) {
        n73_video_node *n;
        mr_packet p;
        mr_status st;
        uint32_t elapsed;
        if (!ac309i_video_due(pace, &elapsed)) break;
        n = n73_vhead;
        memset(&p, 0, sizeof p);
        p.is_video = 1;
        p.has_pts = n->has_pts;
        p.pts_us = n->pts_us;
        p.data = n->data;
        p.len = n->len;
        p.is_annexb = n->is_annexb;
        /* VPv149 semantics: claim/advance the absolute input deadline before
         * entering SendVideo.  The first-frame anchor therefore excludes
         * decoder/VCHIQ submission latency, just like v04_pace_new_frame. */
        ac309i_commit_video_deadline(pace, n76_hostlog);
        st = mr_berryvp_send_packet(bvp, &p);
        if (st == MR_EAGAIN) { if (skipped) (*skipped)++; }
        else if (st != MR_OK) return st;
        else if (sent) (*sent)++;
        n73_vhead = n->next;
        if (!n73_vhead) n73_vtail = NULL;
        free(n->data);
        free(n);
        n73_vqueued--;
    }
    return MR_OK;
}

/* ---- One playback session -------------------------------------------------
 *
 * A session opens the BerryVP stream, feeds it from the current demux
 * position and closes it again. Pause holds the feed inside the session. A
 * jump (seek) ends the session: berryvp.library has no flush, so mrberry
 * closes the stream, repositions the demuxer on a keyframe and opens a new
 * one, which is the same as starting the next film in a row.
 *
 * After a seek, timestamps are rebased so the new session starts at 0, like
 * a fresh file: every packet's PTS has the keyframe's time (base_us)
 * subtracted, and packets from before the keyframe are dropped. With no
 * seek, base_us is 0 and packets pass through unchanged. */
enum {
    SESSION_END = 0,     /* natural end of file */
    SESSION_STOP,        /* user stop           */
    SESSION_SEEK,        /* jump requested      */
    SESSION_ERROR
};

typedef struct {
    mr_demux *d;
    const mr_video_info *vi;
    const mr_audio_info *ai;
    int use_audio;            /* in: wanted; cleared if the track is refused */
    int paula;                /* sound through the Amiga, not the Pi's HDMI */
    int low_rate, mono;       /* Paula only, as in mrplay                  */
    int32_t av_offset_us;     /* Paula only: delay the sound by this much  */
    int is_mov;
    int can_seek;
    uint64_t base_us;         /* media time this session starts at          */
    const char *audio_note;   /* added to the status line, may be NULL      */
    /* results */
    int64_t seek_delta_us;
    uint64_t pos_us;
    char error[128];
} session;

/* Session clock for the status line and for jumps: wall time since the
 * first video access unit went out, minus time spent paused. */
typedef struct {
    int started;
    uint32_t start_us;
    uint32_t paused_us;
} play_clock;

static uint64_t clock_pos(const session *s, const play_clock *c)
{
    if (!c->started) return s->base_us;
    return s->base_us +
           (uint64_t)(uint32_t)(ac309i_read_sys_us() - c->start_us - c->paused_us);
}

static void status_playing(const session *s, const play_clock *c, int paused)
{
    char text[160], at[24];
    format_time(at, sizeof at, clock_pos(s, c));
    snprintf(text, sizeof text, "%s%dx%d, %s, %s%s%s",
             paused ? "Paused - " : "", s->vi->width, s->vi->height,
             mr_demux_container_name(s->d), at,
             s->audio_note ? ", " : "", s->audio_note ? s->audio_note : "");
    status_live(MR_PLAYER_STATE_PLAYING, text);
}

/* Returns 0 to drop the packet (it lies before the seek point). */
static int rebase_packet(const session *s, mr_packet *pkt)
{
    if (!s->base_us || !pkt->has_pts) return 1;
    if (pkt->pts_us < s->base_us) return 0;
    pkt->pts_us -= s->base_us;
    return 1;
}

static int play_session(session *s)
{
    mr_berryvp *bvp = &g_bvp;
    mr_packet pkt;
    mr_status st;
    unsigned long seen = 0, sent = 0, skipped = 0;
    unsigned long audio_seen = 0, audio_sent = 0;
    /* P32: FAST06 E-AC3 natural-EOF drain clock, merged into the
     * hardware-confirmed P06 ESC/status host. */
    uint64_t eac3_first_pts_us = 0, eac3_last_pts_us = 0;
    uint32_t eac3_first_wall_us = 0;
    int eac3_pts_clock_valid = 0;
    ac309i_pacer ac309i_pace;
    int audio_enabled = 0;
    int n73_prime_ready = 0;
    int p04_first_video_sent = 0;
    int result = SESSION_END;
    play_clock clk;
    uint32_t last_status_us = 0;
    const mr_audio_info *ai = s->ai;
    mrb_paula *paula = NULL;
    /* Where the 68k time goes, for the log: demuxing, and handing video to
     * the library (which includes its own wait until a frame is due). */
    uint64_t t_demux_us = 0, t_vsend_us = 0;
    uint32_t t_session0 = 0, t0;

    memset(&clk, 0, sizeof clk);
    s->seek_delta_us = 0;

    /* HDMI audio only when ConfigAudio will be called and will accept the
     * track; otherwise the library must build its graph without the
     * audio-clocked scheduler. */
    st = mr_berryvp_open(bvp, s->vi,
                         s->use_audio && !s->paula &&
                         mr_berryvp_hdmi_audio_ok(ai));
    if (st != MR_OK) {
        snprintf(s->error, sizeof s->error, "Could not open BerryVP video graph");
        return SESSION_ERROR;
    }
    if (s->is_mov) {
        /* VPv149 hardware state: MP4 H.264 feed fragments are 128 KiB.
         * This is independent of presentation PTS and of the host-time gate. */
        bvp->video_chunk_limit = 0x00020000UL;
        n76_hlog("AC309I_MP4_128K_FRAGMENT_CAP");
    }

    if (s->use_audio && s->paula) {
        /* MintVID's own decoders and Paula. The VideoCore plays the picture
         * alone, exactly as with --no-audio, so the BerryCore kernel is not
         * needed. */
        static char why[96];
        char reason[64];
        paula = mrb_paula_open(ai, s->low_rate, s->mono, s->av_offset_us,
                               reason, sizeof reason);
        if (paula) {
            snprintf(why, sizeof why, "%s", mrb_paula_describe(paula));
            printf("Audio: %s\n", why);
        } else {
            printf("Warning: Amiga audio: %s; playing video without sound.\n",
                   reason);
            snprintf(why, sizeof why, "no sound (%s)", reason);
            s->use_audio = 0;   /* do not try again after a jump */
        }
        s->audio_note = why;
        n73_prime_ready = 1;
    } else if (s->use_audio) {
        st = mr_berryvp_config_audio(bvp, ai);
        if (st == MR_OK) {
            audio_enabled = 1;
            n73_prime_ready = 1; /* N75: clock/scheduler already armed in ConfigAudio; no host-side video hold. */
            n76_hlog("N76_AFTER_CONFIG_AUDIO_OK");
        } else if (st == MR_EUNSUPPORTED) {
            /* Refused by mrberry's own format check before the library saw
             * it: the stream is untouched, so play the picture alone. */
            printf("Warning: this audio format is not supported; playing video without sound.\n");
            s->use_audio = 0;
            s->audio_note = "no sound (audio format not supported)";
            n73_prime_ready = 1;
        } else {
            snprintf(s->error, sizeof s->error,
                     "Audio setup failed (is the BerryCore Emu68 kernel installed?)");
            mr_berryvp_close(bvp);
            return SESSION_ERROR;
        }
    } else {
        n73_prime_ready = 1;
    }

    ac309i_pacer_init(&ac309i_pace, s->vi, s->is_mov && audio_enabled);
    if (ac309i_pace.enabled) n76_hlog("AC309I_HOST_TIME_PACER_ACTIVE_LEAD_250MS");
    status_playing(s, &clk, 0);

    n76_hlog("N76_BEFORE_DEMUX_LOOP");
    t_session0 = (uint32_t)ac309i_read_sys_us();
    for (;;) {
        int64_t delta = 0;
        int ev = poll_event(&delta);
        if (ev == EV_PAUSE) {
            /* Hold the feed. Nothing new reaches the VideoCore, so the
             * picture stops once what is already queued has played. */
            uint32_t paused_at = (uint32_t)ac309i_read_sys_us(), dt;
            status_playing(s, &clk, 1);
            mrb_paula_pause(paula, 1);
            n76_hlog("PAUSE_ON");
            for (;;) {
                Delay(2);
                mrb_paula_service(paula);
                delta = 0;
                ev = poll_event(&delta);
                if (ev == EV_PAUSE || ev == EV_STOP ||
                    (ev == EV_SEEK && s->can_seek)) break;
            }
            dt = (uint32_t)ac309i_read_sys_us() - paused_at;
            /* Move every wall-clock anchor past the pause. */
            ac309i_pace.base_us += dt;
            eac3_first_wall_us += dt;
            clk.paused_us += dt;
            mrb_paula_pause(paula, 0);
            n76_hlog("PAUSE_OFF");
            if (ev == EV_PAUSE) { status_playing(s, &clk, 0); ev = EV_NONE; }
        }
        if (ev == EV_STOP) {
            n76_hlog("P05_ESC_STOP");
            result = SESSION_STOP;
            break;
        }
        if (ev == EV_SEEK) {
            if (s->can_seek) {
                s->seek_delta_us = delta;
                result = SESSION_SEEK;
                break;
            }
            /* TS and other unindexed sources cannot jump. */
        }
        {
            uint32_t now = (uint32_t)ac309i_read_sys_us();
            if ((uint32_t)(now - last_status_us) >= 1000000u) {
                last_status_us = now;
                status_playing(s, &clk, 0);
            }
        }

        mrb_paula_service(paula);
        t0 = (uint32_t)ac309i_read_sys_us();
        st = mr_demux_next_packet(s->d, &pkt);
        t_demux_us += (uint32_t)((uint32_t)ac309i_read_sys_us() - t0);
        if (st != MR_OK) break;
        if (!pkt.len) continue;
        if (!rebase_packet(s, &pkt)) continue;
        if (!pkt.is_video) {
            audio_seen++;
            if (paula) {
                if (!mrb_paula_feed(paula, &pkt)) {
                    snprintf(s->error, sizeof s->error,
                             "Cannot buffer audio (out of memory)");
                    result = SESSION_ERROR;
                    break;
                }
                audio_sent++;
                continue;
            }
            if (!audio_enabled) continue;

            /* PiStorm04: retain compressed startup audio until one real
             * H.264 AU has completed SendVideo. */
            if (!p04_first_video_sent) {
                if (!p04_queue_audio(&pkt)) {
                    snprintf(s->error, sizeof s->error,
                             "Cannot buffer startup audio (out of memory)");
                    result = SESSION_ERROR;
                    break;
                }
                continue;
            }

            st = mr_berryvp_send_audio_packet(bvp, &pkt);
            if (st != MR_OK) {
                snprintf(s->error, sizeof s->error,
                         "Audio playback failed at packet %lu", audio_seen);
                result = SESSION_ERROR;
                break;
            }
            if (ai && ai->format_tag == MR_AUDIO_FORMAT_EAC3 && pkt.has_pts) {
                if (!eac3_pts_clock_valid) {
                    eac3_first_pts_us = pkt.pts_us;
                    eac3_first_wall_us = (uint32_t)ac309i_read_sys_us();
                    eac3_pts_clock_valid = 1;
                }
                eac3_last_pts_us = pkt.pts_us;
            }
            audio_sent++;
            if (ac309i_pace.enabled && n73_prime_ready && n73_vhead) {
                st = ac309i_service_mov_video(bvp, &ac309i_pace, &sent, &skipped);
                if (st != MR_OK) {
                    snprintf(s->error, sizeof s->error,
                             "Timed MP4 video feed failed");
                    result = SESSION_ERROR;
                    break;
                }
            }
            if (!n73_prime_ready && audio_sent >= 4) {
                n73_prime_ready = 1;
                st = n73_flush_video_queue(bvp, &sent, &skipped);
                if (st != MR_OK) {
                    snprintf(s->error, sizeof s->error,
                             "Queued video flush failed");
                    result = SESSION_ERROR;
                    break;
                }
            }
            continue;
        }
        seen++;
        if (!n73_prime_ready) {
            if (!n73_queue_video(&pkt)) {
                snprintf(s->error, sizeof s->error,
                         "Cannot buffer video (out of memory)");
                result = SESSION_ERROR;
                break;
            }
            continue;
        }
        if (ac309i_pace.enabled) {
            if (!n73_queue_video(&pkt)) {
                snprintf(s->error, sizeof s->error,
                         "Cannot queue video (out of memory)");
                result = SESSION_ERROR;
                break;
            }
            if (n73_vqueued > ac309i_pace.queue_peak)
                ac309i_pace.queue_peak = n73_vqueued;
            t0 = (uint32_t)ac309i_read_sys_us();
            st = ac309i_service_mov_video(bvp, &ac309i_pace, &sent, &skipped);
            t_vsend_us += (uint32_t)((uint32_t)ac309i_read_sys_us() - t0);
            if (st != MR_OK) {
                snprintf(s->error, sizeof s->error,
                         "Timed MP4 video feed failed at frame %lu", seen);
                result = SESSION_ERROR;
                break;
            }
        } else {
            t0 = (uint32_t)ac309i_read_sys_us();
            st = mr_berryvp_send_packet(bvp, &pkt);
            t_vsend_us += (uint32_t)((uint32_t)ac309i_read_sys_us() - t0);
            if (st == MR_EAGAIN) {
                skipped++;
                continue;
            }
            if (st != MR_OK) {
                snprintf(s->error, sizeof s->error,
                         "Video playback failed at frame %lu", seen);
                result = SESSION_ERROR;
                break;
            }
            sent++;
        }

        if (!p04_first_video_sent && sent) {
            p04_first_video_sent = 1;
            clk.started = 1;
            clk.start_us = (uint32_t)ac309i_read_sys_us();
            n76_hlog("P04_FIRST_VIDEO_SENT_BEFORE_PCM");
            /* The first frame Paula sound lines up with. Paula mode never
             * uses the MP4 pacer, so this is the packet just sent. */
            mrb_paula_anchor(paula, clk.start_us,
                             pkt.has_pts ? pkt.pts_us : 0);
            if (ai && ai->format_tag == MR_AUDIO_FORMAT_EAC3 && !eac3_pts_clock_valid &&
                p04_ahead && p04_ahead->has_pts) {
                eac3_first_pts_us = p04_ahead->pts_us;
                eac3_last_pts_us = (p04_atail && p04_atail->has_pts) ?
                                   p04_atail->pts_us : p04_ahead->pts_us;
                eac3_first_wall_us = (uint32_t)ac309i_read_sys_us();
                eac3_pts_clock_valid = 1;
            }
            st = p04_flush_audio_queue(bvp, &audio_sent);
            if (st != MR_OK) {
                snprintf(s->error, sizeof s->error,
                         "Startup audio flush failed");
                result = SESSION_ERROR;
                break;
            }
        }
    }

    {
        /* Wall time of the feed loop, pauses excluded. "other" is
         * everything not listed: event polling, Paula service, queueing. */
        uint32_t wall_ms = (uint32_t)((uint32_t)ac309i_read_sys_us() -
                                      t_session0 - clk.paused_us) / 1000u;
        unsigned long dec_ms = 0, pkts = 0, starv = 0, minbuf = 0;
        unsigned long demux_ms = (unsigned long)(t_demux_us / 1000u);
        unsigned long vsend_ms = (unsigned long)(t_vsend_us / 1000u);
        unsigned long used;
        mrb_paula_timing(paula, &dec_ms, &pkts, &starv, &minbuf);
        used = demux_ms + vsend_ms + dec_ms;
        DBG_PRINTF("mrberry timing: wall=%lu ms frames=%lu demux=%lu ms "
                   "video-send=%lu ms audio-decode=%lu ms (%lu packets) "
                   "other=%lu ms\n",
                   (unsigned long)wall_ms, sent, demux_ms, vsend_ms, dec_ms,
                   pkts, used < wall_ms ? (unsigned long)wall_ms - used : 0UL);
        if (paula)
            DBG_PRINTF("Amiga audio: %lu underruns, least queued %lu ms\n",
                       starv, minbuf);
    }
    if (result == SESSION_END && st != MR_EAGAIN) {
        /* Current MintVID demuxers use MR_EAGAIN for normal EOF. */
        printf("Error: demux ended with status %ld after %lu video packets.\n",
               (long)st, seen);
    }

    if (result == SESSION_END && ac309i_pace.enabled && n73_vhead) {
        /* Natural EOF normally leaves only a tiny tail.  Let the absolute
         * timeline reach it without inventing new PTS or changing the grid.
         * Bound this final CPU-only wait to two seconds of the same 1-MHz
         * timer so malformed timing cannot create an endless close hang. */
        uint32_t drain_start = (uint32_t)ac309i_read_sys_us();
        while (n73_vhead) {
            int64_t delta;
            st = ac309i_service_mov_video(bvp, &ac309i_pace, &sent, &skipped);
            if (st != MR_OK || !n73_vhead) break;
            if (poll_event(&delta) == EV_STOP) { result = SESSION_STOP; break; }
            if ((uint32_t)(ac309i_read_sys_us() - drain_start) >= 2000000u) break;
        }
    }
    n73_free_video_queue();
    p04_free_audio_queue();

    /* Paula still holds a few seconds of sound at the end of the file. */
    if (result == SESSION_END && paula) {
        uint32_t drain_start = (uint32_t)ac309i_read_sys_us();
        while (!mrb_paula_idle(paula)) {
            int64_t delta;
            int ev;
            Delay(1);
            ev = poll_event(&delta);
            if (ev == EV_STOP) { result = SESSION_STOP; break; }
            if ((uint32_t)(ac309i_read_sys_us() - drain_start) >= 10000000u) break;
        }
    }
    if (paula) {
        unsigned long sil, drop, fixes;
        mrb_paula_stats(paula, &sil, &drop, &fixes);
        DBG_PRINTF("Amiga audio sync: %lu corrections, %lu ms silence added, "
                   "%lu ms dropped\n", fixes, sil, drop);
        mrb_paula_close(paula);
        paula = NULL;
    }

    /* P32/FAST06: at natural E-AC3 EOF, keep the graph alive until the final
     * scheduled PCM tail has had time to present. ESC remains active here. */
    if (result == SESSION_END && eac3_pts_clock_valid && ai && ai->sample_rate) {
        uint64_t frame_us = (1536ULL * 1000000ULL + (uint64_t)ai->sample_rate - 1ULL) /
                            (uint64_t)ai->sample_rate;
        uint64_t expected_us = frame_us + 150000ULL;
        uint32_t elapsed_us = (uint32_t)(ac309i_read_sys_us() - eac3_first_wall_us);
        uint32_t wait_us = 0;
        if (eac3_last_pts_us >= eac3_first_pts_us)
            expected_us += eac3_last_pts_us - eac3_first_pts_us;
        if (expected_us > (uint64_t)elapsed_us) {
            uint64_t rem = expected_us - (uint64_t)elapsed_us;
            if (rem > 15000000ULL) rem = 15000000ULL;
            wait_us = (uint32_t)rem;
        }
        if (wait_us) {
            uint32_t wait_start = (uint32_t)ac309i_read_sys_us();
            while ((uint32_t)(ac309i_read_sys_us() - wait_start) < wait_us) {
                int64_t delta;
                if (poll_event(&delta) == EV_STOP) { result = SESSION_STOP; break; }
            }
        }
    }

    s->pos_us = clock_pos(s, &clk);
    DBG_PRINTF("mrberry session: video=%lu sent=%lu skipped=%lu audio=%lu sent=%lu\n",
               seen, sent, skipped, audio_seen, audio_sent);
    if (mr_berryvp_close(bvp) != MR_OK && result != SESSION_ERROR) {
        snprintf(s->error, sizeof s->error, "VideoCore cleanup failed");
        result = SESSION_ERROR;
    }
    return result;
}

static void usage(void)
{
    printf("mrberry - H.264 hardware A/V player for PiStorm (Emu68 BerryCore)\n"
           "Usage: mrberry <movie.mkv|movie.mp4|movie.ts> [--start=H:MM:SS] [--no-audio]\n"
           "               [--paula [--audio-rate=low] [--audio-mono] [--av-offset=MS]]\n"
           "  --paula  sound through the Amiga (MintVID's decoders), not the Pi's HDMI\n"
           "Keys: ESC stop, Space pause, Left/Right -/+10 s, Down/Up -/+60 s\n");
}

int main(int argc, char **argv)
{
    session s;
    const char *path = NULL;
    uint64_t start_us = 0;
    int no_audio = 0, paula = 0, low_rate = 0, mono = 0, i;
    int result = SESSION_ERROR, rc = 20;
    long av_offset_ms = 0;
    const char *final_text = NULL;
    LONG final_state = MR_PLAYER_STATE_ERROR;

    setvbuf(stdout, NULL, _IONBF, 0);
#if MR_DEBUG_LOGS
    n76_hostlog = fopen("N76_HOST.TXT", "ab");
#endif
    n76_hlog("N76_START");

    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--start=", 8)) {
            if (!parse_time(argv[i] + 8, &start_us)) { usage(); return 5; }
        } else if (!strcmp(argv[i], "--no-audio")) {
            no_audio = 1;
        } else if (!strcmp(argv[i], "--paula")) {
            paula = 1;
        } else if (!strcmp(argv[i], "--audio-rate=low")) {
            low_rate = 1;
        } else if (!strcmp(argv[i], "--audio-rate=normal")) {
            low_rate = 0;
        } else if (!strcmp(argv[i], "--audio-mono")) {
            mono = 1;
        } else if (!strncmp(argv[i], "--av-offset=", 12)) {
            char *end;
            av_offset_ms = strtol(argv[i] + 12, &end, 10);
            if (*end || av_offset_ms < -5000 || av_offset_ms > 5000) {
                usage(); return 5;
            }
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            usage(); return 5;
        } else if (!path) {
            path = argv[i];
        } else {
            usage(); return 5;
        }
    }
    if (!path) { usage(); return 5; }

    memset(&s, 0, sizeof s);
    control_port_open();
    status_set(MR_PLAYER_STATE_OPENING, MRBERRY_CODEC,
               "Opening PiStorm VideoCore playback...");

    s.d = mr_demux_open_file(path);
    if (!s.d) {
        printf("Error: cannot open media: %s\n", mr_demux_last_open_error());
        status_set(MR_PLAYER_STATE_ERROR, MRBERRY_CODEC, "Could not open media file");
        rc = 10;
        goto out;
    }
    s.vi = mr_demux_video(s.d);
    if (!s.vi || !s.vi->valid || s.vi->fourcc != MR_FOURCC('a','v','c','1')) {
        char codec[80], text[128];
        mr_demux_describe_video_codec(s.d, codec, sizeof codec);
        printf("Error: H.264/AVC video is required (found: %s).\n", codec);
        snprintf(text, sizeof text, "PiStorm mode plays H.264 only (this is %s)", codec);
        status_set(MR_PLAYER_STATE_UNSUPPORTED, codec, text);
        rc = 10;
        goto out;
    }
    s.is_mov = !strcmp(mr_demux_container_name(s.d), "MOV");
    s.can_seek = mr_demux_can_seek(s.d);
    s.ai = mr_demux_audio(s.d);
    s.paula = paula;
    s.low_rate = low_rate;
    s.mono = mono;
    s.av_offset_us = (int32_t)av_offset_ms * 1000;
    if (no_audio) {
        s.audio_note = "sound off";
    } else if (paula) {
        /* Whatever MintVID can decode; mrb_paula_open() says if not. */
        if (s.ai && s.ai->valid) s.use_audio = 1;
        else s.audio_note = "no sound (no audio track)";
    } else if (!s.ai || !s.ai->valid ||
               (s.ai->format_tag != MR_AUDIO_FORMAT_AAC &&
                s.ai->format_tag != MR_AUDIO_FORMAT_AC3 &&
                s.ai->format_tag != MR_AUDIO_FORMAT_EAC3)) {
        printf("Warning: no supported AAC/AC3/E-AC3 audio track; video will continue without audio.\n");
        s.audio_note = "no sound (no AAC/AC3/E-AC3 track)";
    } else if (s.ai->format_tag == MR_AUDIO_FORMAT_EAC3 && s.ai->channels != 2) {
        /* AC308N3: multichannel E-AC3 decodes far below real time on CPU1;
         * only stereo E-AC3 is played. The picture plays alone. */
        printf("Warning: multichannel E-AC3 is not supported in real time; playing without sound.\n");
        s.audio_note = "no sound (E-AC3 5.1 is too slow)";
    } else {
        s.use_audio = 1;
    }

    if (start_us) {
        uint64_t got = 0;
        if (s.can_seek && mr_demux_seek(s.d, start_us, &got) == MR_OK)
            s.base_us = got;
        else
            printf("Warning: cannot start at a time in this file; starting at the beginning.\n");
    }

    if (p05_input_open()) n76_hlog("P05_ESC_INPUT_WINDOW_OK");
    else n76_hlog("P05_ESC_INPUT_WINDOW_FAIL");

    for (;;) {
        char text[128], at[24];
        format_time(at, sizeof at, s.base_us);
        snprintf(text, sizeof text, "%dx%d, %s, %s", s.vi->width, s.vi->height,
                 mr_demux_container_name(s.d), at);
        status_set(MR_PLAYER_STATE_PLAYING, MRBERRY_CODEC, text);
        result = play_session(&s);
        if (result != SESSION_SEEK) break;
        {
            int64_t target = (int64_t)s.pos_us + s.seek_delta_us;
            uint64_t got = 0;
            if (target < 0) target = 0;
            if (mr_demux_seek(s.d, (uint64_t)target, &got) != MR_OK) {
                snprintf(s.error, sizeof s.error, "Seek failed");
                result = SESSION_ERROR;
                break;
            }
            s.base_us = got;
        }
    }

    if (result == SESSION_ERROR) {
        printf("Error: %s.\n", s.error);
        final_state = MR_PLAYER_STATE_ERROR;
        final_text = s.error;
        rc = 20;
    } else {
        final_state = MR_PLAYER_STATE_ENDED;
        final_text = result == SESSION_STOP ? "Stopped" : "Finished";
        rc = 0;
    }
    status_set(final_state, MRBERRY_CODEC, final_text);

out:
    p05_input_close();
    if (s.d) mr_demux_close(s.d);
    control_port_close();
    n76_hlog(rc ? "N76_END_ERROR" : "N76_END_OK");
    if (n76_hostlog) fclose(n76_hostlog);
    return rc;
}
