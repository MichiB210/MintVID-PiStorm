/* MintVID demux -> berryvp.library hardware A/V bridge.
 * Stage 4F HUNKSAFE: the hardware-confirmed Stage-3D H.264 path is unchanged. AAC-LC packets are
 * demuxed by MintVID, wrapped as ADTS where required, decoded by BerryFAAD03
 * on ARM CPU1 and sent as PCM16 to MMAL audio_render/HDMI. */
#include "../core/mr_demux.h"
#include "mr_berryvp.h"
#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
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

/* MintVID PiStorm05: N79Q's historical ESC handler belongs to the standalone
 * Intuition window. Library mode deliberately skips that window, so mrberry
 * owns a tiny activated Intuition input window while HVS fullscreen is active.
 * The window itself is hidden under the VideoCore layer; only RAWKEY/vanilla
 * input is used. */
static struct Window *p05_input_window;

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

static int p05_input_poll_stop(void)
{
    struct IntuiMessage *msg;
    int stop = 0;

    if (!p05_input_window || !p05_input_window->UserPort)
        return 0;

    while ((msg = (struct IntuiMessage *)GetMsg(p05_input_window->UserPort)) != NULL) {
        ULONG cls = msg->Class;
        UWORD code = msg->Code;
        ReplyMsg((struct Message *)msg);

        if (cls == IDCMP_RAWKEY) {
            /* Amiga raw key 0x45 = ESC. Ignore key-up (bit 7 set). */
            if ((code & 0x80u) == 0 && ((code & 0x7fu) == 0x45u))
                stop = 1;
        } else if (cls == IDCMP_VANILLAKEY) {
            if (code == 0x1bu || code == '0')
                stop = 1;
        }
    }
    return stop;
}

static void p05_input_close(void)
{
    if (p05_input_window) {
        CloseWindow(p05_input_window);
        p05_input_window = NULL;
    }
}

/* MintVID PiStorm06: publish the same atomic terminal status snapshot used by
 * mrplay. MintVID launches mrberry asynchronously, so this T: record is the
 * reliable hand-off for Opening/Playing/Stopped/Finished after mrberry exits. */
#define P06_STATUS_MAGIC 0x4D525653UL
#define P06_STATUS_FILE  "T:MintVID.player-status"
#define P06_STATUS_TMP   "T:MintVID.player-status.tmp"
#define P06_CODEC_MAX 48
#define P06_TEXT_MAX 208
enum {
    P06_STATE_STARTING = 0,
    P06_STATE_OPENING,
    P06_STATE_PLAYING,
    P06_STATE_UNSUPPORTED,
    P06_STATE_ERROR,
    P06_STATE_ENDED
};
typedef struct {
    volatile ULONG magic;
    volatile ULONG seq;
    volatile LONG state;
    char codec[P06_CODEC_MAX];
    char text[P06_TEXT_MAX];
} p06_player_status;
typedef struct {
    p06_player_status status;
    ULONG trailer_magic;
} p06_player_status_snapshot;
static ULONG p06_status_seq;

static void p06_status_copy(char *dst, size_t size, const char *src)
{
    size_t i = 0;
    if (!size) return;
    if (src)
        for (; i + 1 < size && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

static void p06_status_write(LONG state, const char *codec, const char *text)
{
    p06_player_status_snapshot snapshot;
    BPTR file;
    LONG written;

    memset(&snapshot, 0, sizeof snapshot);
    snapshot.status.magic = P06_STATUS_MAGIC;
    snapshot.status.seq = ++p06_status_seq;
    snapshot.status.state = state;
    p06_status_copy(snapshot.status.codec, sizeof snapshot.status.codec,
                    codec ? codec : "");
    p06_status_copy(snapshot.status.text, sizeof snapshot.status.text,
                    text ? text : "");
    snapshot.trailer_magic = P06_STATUS_MAGIC;

    file = Open((CONST_STRPTR)P06_STATUS_TMP, MODE_NEWFILE);
    if (!file) return;
    written = Write(file, &snapshot, (LONG)sizeof snapshot);
    Close(file);
    if (written == (LONG)sizeof snapshot) {
        DeleteFile((CONST_STRPTR)P06_STATUS_FILE);
        if (!Rename((CONST_STRPTR)P06_STATUS_TMP,
                    (CONST_STRPTR)P06_STATUS_FILE))
            DeleteFile((CONST_STRPTR)P06_STATUS_TMP);
    } else {
        DeleteFile((CONST_STRPTR)P06_STATUS_TMP);
    }
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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
#if MR_DEBUG_LOGS
    n76_hostlog = fopen("N76_HOST.TXT", "ab");
#endif
    n76_hlog("N76_START");
    DBG_PRINTF("[N76] mrberry HOST-DEMUX trace on N75 scheduler base\n");
    DBG_PRINTF("[H4F] BerryVP context in BSS: %lu bytes @ %p\n", (unsigned long)sizeof g_bvp, (void *)&g_bvp);
    mr_demux *d;
    const mr_video_info *vi;
    const mr_audio_info *ai;
    mr_packet pkt;
    mr_berryvp *bvp = &g_bvp;
    unsigned long seen = 0, sent = 0, skipped = 0;
    unsigned long audio_seen = 0, audio_sent = 0;
    /* P32: FAST06 E-AC3 natural-EOF drain clock, merged into the
     * hardware-confirmed P06 ESC/status host. */
    uint64_t eac3_first_pts_us = 0, eac3_last_pts_us = 0;
    uint32_t eac3_first_wall_us = 0;
    int eac3_pts_clock_valid = 0;
    int ac309i_is_mov = 0;
    ac309i_pacer ac309i_pace;
    int audio_enabled = 0;
    int n73_prime_ready = 0;
    int p04_first_video_sent = 0;
    int aborted = 0;
    mr_status st;

    if (argc != 2) {
        printf("mrberry - H.264 hardware A/V player\n"
               "Usage: mrberry <movie.mkv|movie.mp4|movie.ts>\n");
        return 5;
    }
    p06_status_write(P06_STATE_OPENING, "H.264 / VideoCore",
                     "Opening PiStorm VideoCore playback...");
    DBG_PRINTF("[D01] before mr_demux_open_file\n");
    d = mr_demux_open_file(argv[1]);
    DBG_PRINTF("[D02] after mr_demux_open_file ptr=%p\n", (void *)d);
    if (!d) {
        printf("Error: cannot open media: %s\n", mr_demux_last_open_error());
        p06_status_write(P06_STATE_ERROR, "H.264 / VideoCore",
                         "Could not open media file");
        return 10;
    }
    DBG_PRINTF("[D03] before mr_demux_video\n");
    vi = mr_demux_video(d);
    DBG_PRINTF("[D04] after mr_demux_video ptr=%p\n", (void *)vi);
    if (!vi || !vi->valid || vi->fourcc != MR_FOURCC('a','v','c','1')) {
        char codec[80];
        mr_demux_describe_video_codec(d, codec, sizeof codec);
        printf("Error: H.264/AVC video is required (found: %s).\n", codec);
        p06_status_write(P06_STATE_UNSUPPORTED, codec,
                         "H.264/AVC video is required");
        mr_demux_close(d);
        return 10;
    }
    DBG_PRINTF("MintVID demux: %s, H.264 %dx%d, config=%lu bytes\n",
           mr_demux_container_name(d), vi->width, vi->height,
           (unsigned long)vi->config_len);
    ac309i_is_mov = !strcmp(mr_demux_container_name(d), "MOV");
    ai = mr_demux_audio(d);
    if (ai && ai->valid) {
        char acodec[80];
        mr_demux_describe_audio_codec(d, acodec, sizeof acodec);
        DBG_PRINTF("MintVID audio: %s, %lu Hz, %u ch, config=%u bytes\n",
               acodec, (unsigned long)ai->sample_rate, (unsigned)ai->channels,
               (unsigned)ai->config_len);
    } else {
        DBG_PRINTF("MintVID audio: no usable audio track\n");
    }
    DBG_PRINTF("[N75] Video PTS enabled; natural A/V interleave, scheduler owns presentation time.\n");
    fflush(stdout);

    /* AC308N3 realtime policy: do not even start the BerryVP graph for
     * multichannel E-AC3.  The current CPU1 scalar path decodes it correctly
     * but measured far below realtime; stereo E-AC3 remains supported. */
    if (ai && ai->valid && ai->format_tag == MR_AUDIO_FORMAT_EAC3 && ai->channels != 2) {
        if (ai->channels == 6)
            printf("Error: E-AC3 5.1 audio is not supported for real-time playback. Stereo E-AC3 only.\n");
        else
            printf("Error: E-AC3 multichannel audio (%u channels) is not supported for real-time playback. Stereo E-AC3 only.\n",
                   (unsigned)ai->channels);
        p06_status_write(P06_STATE_UNSUPPORTED, "E-AC3",
                         "Multichannel E-AC3 is not supported in real time");
        mr_demux_close(d);
        return 20;
    }

    DBG_PRINTF("[D05] before mr_berryvp_open\n");
    st = mr_berryvp_open(bvp, vi);
    DBG_PRINTF("[D06] after mr_berryvp_open st=%ld\n", (long)st);
    if (st != MR_OK) {
        p06_status_write(P06_STATE_ERROR, "H.264 / VideoCore",
                         "Could not open BerryVP video graph");
        mr_demux_close(d);
        return 20;
    }
    if (ac309i_is_mov) {
        /* VPv149 hardware state: MP4 H.264 feed fragments are 128 KiB.
         * This is independent of presentation PTS and of the host-time gate. */
        bvp->video_chunk_limit = 0x00020000UL;
        DBG_PRINTF("[AC309I] MOV/MP4 H.264 fragment cap: 128 KiB (VPv149 proven)\n");
        n76_hlog("AC309I_MP4_128K_FRAGMENT_CAP");
    }

    if (ai && ai->valid && (ai->format_tag == MR_AUDIO_FORMAT_AAC || ai->format_tag == MR_AUDIO_FORMAT_AC3 || ai->format_tag == MR_AUDIO_FORMAT_EAC3)) {
        st = mr_berryvp_config_audio(bvp, ai);
        if (st == MR_OK) {
            audio_enabled = 1;
            n73_prime_ready = 1; /* N75: clock/scheduler already armed in ConfigAudio; no host-side video hold. */
            if (ai->format_tag == MR_AUDIO_FORMAT_AC3)
                DBG_PRINTF("[AC307A] AC3/berry_ac3.module audio armed; N97QD host + N79Q library path\n");
            else if (ai->format_tag == MR_AUDIO_FORMAT_EAC3)
                DBG_PRINTF("[AC308N3] E-AC3 stereo/berry_eac3.module audio armed; stereo HDMI renderer\n");
            else
                DBG_PRINTF("[A03] AAC/BerryFAAD03 audio armed; N75 natural scheduler feed enabled\n");
            n76_hlog("N76_AFTER_CONFIG_AUDIO_OK");
        } else {
            printf("Error: audio setup failed; playback stopped.\n");
            p06_status_write(P06_STATE_ERROR, "H.264 / VideoCore",
                             "Audio setup failed");
            mr_berryvp_close(bvp);
            mr_demux_close(d);
            return 20;
        }
    } else {
        printf("Warning: no supported AAC/AC3/E-AC3 audio track; video will continue without audio.\n");
        n73_prime_ready = 1;
    }

    ac309i_pacer_init(&ac309i_pace, vi, ac309i_is_mov && audio_enabled);
    if (ac309i_pace.enabled) {
        DBG_PRINTF("[AC309I] VPv149 host-time feed pacer: fps=%lu/%lu step=%lu + %lu/%lu us, lead=%lu us\n",
               (unsigned long)vi->rate, (unsigned long)vi->scale,
               (unsigned long)ac309i_pace.step_whole_us,
               (unsigned long)ac309i_pace.step_rem,
               (unsigned long)ac309i_pace.step_den,
               (unsigned long)ac309i_pace.lead_us);
        n76_hlog("AC309I_HOST_TIME_PACER_ACTIVE_LEAD_250MS");
    }

    if (p05_input_open()) n76_hlog("P05_ESC_INPUT_WINDOW_OK");
    else n76_hlog("P05_ESC_INPUT_WINDOW_FAIL");
    {
        char p06_text[128];
        snprintf(p06_text, sizeof p06_text, "%dx%d, %s",
                 vi->width, vi->height, mr_demux_container_name(d));
        p06_status_write(P06_STATE_PLAYING, "H.264 / VideoCore", p06_text);
        n76_hlog("P06_STATUS_PLAYING");
    }

    n76_hlog("N76_BEFORE_DEMUX_LOOP");
    for (;;) {
        if (p05_input_poll_stop()) {
            n76_hlog("P05_ESC_STOP");
            aborted = 1;
            st = MR_EAGAIN;
            break;
        }
        if (seen < 3 && audio_seen < 3) n76_hlog("N76_BEFORE_SETSIGNAL");
        { ULONG n76_sig = bvp_exec_setsignal(0, 0);
          if (seen < 3 && audio_seen < 3 && n76_hostlog) { fprintf(n76_hostlog, "N76_AFTER_SETSIGNAL sig=%08lx\n", (unsigned long)n76_sig); fflush(n76_hostlog); }
          if (n76_sig & SIGBREAKF_CTRL_C) {
            aborted = 1;
            st = MR_EAGAIN;
            break;
          }
        }
        if (seen < 3 && audio_seen < 3) n76_hlog("N76_BEFORE_DEMUX_NEXT");
        if (seen < 3) DBG_PRINTF("[D10] before demux_next_packet seen=%lu\n", seen);
        st = mr_demux_next_packet(d, &pkt);
        if (seen < 3 && audio_seen < 3 && n76_hostlog) { fprintf(n76_hostlog, "N76_AFTER_DEMUX_NEXT st=%ld video=%d len=%lu pts=%d\n", (long)st, pkt.is_video, (unsigned long)pkt.len, pkt.has_pts); fflush(n76_hostlog); }
        if (seen < 3) DBG_PRINTF("[D11] after demux_next_packet st=%ld is_video=%d len=%lu\n", (long)st, pkt.is_video, (unsigned long)pkt.len);
        if (st != MR_OK) break;
        if (!pkt.len) continue;
        if (!pkt.is_video) {
            audio_seen++;
            if (!audio_enabled) continue;
            if (audio_seen <= 4)
                DBG_PRINTF("[A10] audio #%lu len=%lu pts=%s\n", audio_seen,
                       (unsigned long)pkt.len, pkt.has_pts ? "yes" : "no");

            /* PiStorm04: retain compressed startup audio until one real
             * H.264 AU has completed SendVideo. */
            if (!p04_first_video_sent) {
                if (!p04_queue_audio(&pkt)) {
                    printf("Error: cannot buffer startup audio packet %lu (%lu bytes).\n",
                           audio_seen, (unsigned long)pkt.len);
                    p04_free_audio_queue();
                    p05_input_close();
                    n73_free_video_queue();
                    mr_berryvp_close(bvp);
                    mr_demux_close(d);
                    return 20;
                }
                if (n76_hostlog) {
                    fprintf(n76_hostlog,
                            "P04_STARTUP_AUDIO_QUEUED count=%lu bytes=%lu\n",
                            p04_aqueued, p04_abytes);
                    fflush(n76_hostlog);
                }
                continue;
            }

            if (audio_seen <= 3) n76_hlog("N76_BEFORE_SEND_AUDIO");
            st = mr_berryvp_send_audio_packet(bvp, &pkt);
            if (audio_seen <= 3 && n76_hostlog) { fprintf(n76_hostlog, "N76_AFTER_SEND_AUDIO st=%ld\n", (long)st); fflush(n76_hostlog); }
            if (st != MR_OK) {
                printf("Error: audio playback failed at packet %lu.\n", audio_seen);
                mr_berryvp_close(bvp);
                mr_demux_close(d);
                return 20;
            }
            if (ai && ai->format_tag == MR_AUDIO_FORMAT_EAC3 && pkt.has_pts) {
                if (!eac3_pts_clock_valid) {
                    eac3_first_pts_us = pkt.pts_us;
                    eac3_first_wall_us = (uint32_t)ac309i_read_sys_us();
                    eac3_pts_clock_valid = 1;
                    if (n76_hostlog) {
                        fprintf(n76_hostlog,
                                "FAST06_EAC3_ANCHOR pts=$%08lx:%08lx wall=%lu\n",
                                (unsigned long)(pkt.pts_us >> 32),
                                (unsigned long)pkt.pts_us,
                                (unsigned long)eac3_first_wall_us);
                        fflush(n76_hostlog);
                    }
                }
                eac3_last_pts_us = pkt.pts_us;
            }
            audio_sent++;
            if (ac309i_pace.enabled && n73_prime_ready && n73_vhead) {
                st = ac309i_service_mov_video(bvp, &ac309i_pace, &sent, &skipped);
                if (st != MR_OK) {
                    printf("Error: timed MP4 video service failed after audio packet %lu.\n", audio_seen);
                    n73_free_video_queue();
                    mr_berryvp_close(bvp);
                    mr_demux_close(d);
                    return 20;
                }
            }
            if (!n73_prime_ready && audio_sent >= 4) {
                DBG_PRINTF("[N75] LEGACY-GUARD (unexpected) flushing %lu held video AU(s).\n", n73_vqueued);
                n73_prime_ready = 1;
                st = n73_flush_video_queue(bvp, &sent, &skipped);
                if (st != MR_OK) {
                    printf("Error: queued video flush failed after audio initialization.\n");
                    n73_free_video_queue();
                    mr_berryvp_close(bvp);
                    mr_demux_close(d);
                    return 20;
                }
            }
            if ((audio_sent & 127u) == 0)
                DBG_PRINTF("audio packets sent: %lu\n", audio_sent);
            continue;
        }
        seen++;
        if (seen <= 8) {
            unsigned long phi = (unsigned long)(pkt.pts_us >> 32);
            unsigned long plo = (unsigned long)(pkt.pts_us & 0xffffffffUL);
            DBG_PRINTF("DIAG video #%lu len=%lu annexb=%d has_pts=%d pts=$%08lx:%08lx\n",
                   seen, (unsigned long)pkt.len, pkt.is_annexb, pkt.has_pts, phi, plo);
            fflush(stdout);
        }
        if (!n73_prime_ready) {
            if (!n73_queue_video(&pkt)) {
                printf("Error: cannot buffer video access unit %lu (%lu bytes).\n",
                       seen, (unsigned long)pkt.len);
                n73_free_video_queue();
                mr_berryvp_close(bvp);
                mr_demux_close(d);
                return 20;
            }
            if (seen <= 8)
                DBG_PRINTF("[N75] hold video #%lu until Audio-Master (%lu bytes, pts=%s)\n",
                       seen, (unsigned long)pkt.len, pkt.has_pts ? "yes" : "no");
            continue;
        }
        if (ac309i_pace.enabled) {
            if (!n73_queue_video(&pkt)) {
                printf("Error: cannot queue timed MP4 video access unit %lu (%lu bytes).\n",
                       seen, (unsigned long)pkt.len);
                n73_free_video_queue();
                mr_berryvp_close(bvp);
                mr_demux_close(d);
                return 20;
            }
            if (n73_vqueued > ac309i_pace.queue_peak) {
                ac309i_pace.queue_peak = n73_vqueued;
                if (n76_hostlog && (ac309i_pace.queue_peak <= 8u || (ac309i_pace.queue_peak & 15u) == 0u)) {
                    fprintf(n76_hostlog,
                            "AC309I_QUEUE_PEAK=%lu vpts=$%08lx:%08lx\n",
                            ac309i_pace.queue_peak,
                            (unsigned long)(pkt.pts_us >> 32),
                            (unsigned long)pkt.pts_us);
                    fflush(n76_hostlog);
                }
            }
            st = ac309i_service_mov_video(bvp, &ac309i_pace, &sent, &skipped);
            if (st != MR_OK) {
                printf("Error: timed MP4 video service failed at video packet %lu.\n", seen);
                p04_free_audio_queue();
                p05_input_close();
                n73_free_video_queue();
                mr_berryvp_close(bvp);
                mr_demux_close(d);
                return 20;
            }
            if (!p04_first_video_sent && sent) {
                p04_first_video_sent = 1;
                n76_hlog("P04_FIRST_VIDEO_SENT_BEFORE_PCM");
                if (ai && ai->format_tag == MR_AUDIO_FORMAT_EAC3 && !eac3_pts_clock_valid &&
                    p04_ahead && p04_ahead->has_pts) {
                    eac3_first_pts_us = p04_ahead->pts_us;
                    eac3_last_pts_us = (p04_atail && p04_atail->has_pts) ?
                                       p04_atail->pts_us : p04_ahead->pts_us;
                    eac3_first_wall_us = (uint32_t)ac309i_read_sys_us();
                    eac3_pts_clock_valid = 1;
                    if (n76_hostlog) {
                        fprintf(n76_hostlog,
                                "FAST06_EAC3_ANCHOR pts=$%08lx:%08lx wall=%lu\n",
                                (unsigned long)(eac3_first_pts_us >> 32),
                                (unsigned long)eac3_first_pts_us,
                                (unsigned long)eac3_first_wall_us);
                        fflush(n76_hostlog);
                    }
                }
                st = p04_flush_audio_queue(bvp, &audio_sent);
                if (st != MR_OK) {
                    printf("Error: startup audio flush failed after first timed video packet.\n");
                    p04_free_audio_queue();
                    p05_input_close();
                    n73_free_video_queue();
                    mr_berryvp_close(bvp);
                    mr_demux_close(d);
                    return 20;
                }
                if (n76_hostlog) {
                    fprintf(n76_hostlog,
                            "P04_STARTUP_AUDIO_FLUSH_OK sent=%lu remaining=%lu\n",
                            audio_sent, p04_aqueued);
                    fflush(n76_hostlog);
                }
            }
            continue;
        }
        if (seen <= 3) DBG_PRINTF("[D12] before send_packet #%lu\n", seen);
        if (seen <= 3) n76_hlog("N76_BEFORE_SEND_VIDEO");
        st = mr_berryvp_send_packet(bvp, &pkt);
        if (seen <= 3 && n76_hostlog) { fprintf(n76_hostlog, "N76_AFTER_SEND_VIDEO st=%ld\n", (long)st); fflush(n76_hostlog); }
        if (seen <= 3) DBG_PRINTF("[D13] after send_packet #%lu st=%ld\n", seen, (long)st);
        if (st == MR_EAGAIN) {
            skipped++;
            continue;
        }
        if (st != MR_OK) {
            printf("Error: video playback failed at packet %lu.\n", seen);
            mr_berryvp_close(bvp);
            mr_demux_close(d);
            return 20;
        }
        sent++;

        if (!p04_first_video_sent) {
            p04_first_video_sent = 1;
            n76_hlog("P04_FIRST_VIDEO_SENT_BEFORE_PCM");
            if (ai && ai->format_tag == MR_AUDIO_FORMAT_EAC3 && !eac3_pts_clock_valid &&
                p04_ahead && p04_ahead->has_pts) {
                eac3_first_pts_us = p04_ahead->pts_us;
                eac3_last_pts_us = (p04_atail && p04_atail->has_pts) ?
                                   p04_atail->pts_us : p04_ahead->pts_us;
                eac3_first_wall_us = (uint32_t)ac309i_read_sys_us();
                eac3_pts_clock_valid = 1;
                if (n76_hostlog) {
                    fprintf(n76_hostlog,
                            "FAST06_EAC3_ANCHOR pts=$%08lx:%08lx wall=%lu\n",
                            (unsigned long)(eac3_first_pts_us >> 32),
                            (unsigned long)eac3_first_pts_us,
                            (unsigned long)eac3_first_wall_us);
                    fflush(n76_hostlog);
                }
            }
            st = p04_flush_audio_queue(bvp, &audio_sent);
            if (st != MR_OK) {
                printf("Error: startup audio flush failed after first video packet.\n");
                p04_free_audio_queue();
                p05_input_close();
                n73_free_video_queue();
                mr_berryvp_close(bvp);
                mr_demux_close(d);
                return 20;
            }
            if (n76_hostlog) {
                fprintf(n76_hostlog,
                        "P04_STARTUP_AUDIO_FLUSH_OK sent=%lu remaining=%lu\n",
                        audio_sent, p04_aqueued);
                fflush(n76_hostlog);
            }
        }

        if ((sent & 63u) == 0)
            DBG_PRINTF("video packets sent: %lu\n", sent);
    }

    if (aborted) {
        DBG_PRINTF("Ctrl-C: stopping video and returning to Workbench.\n");
    } else if (st != MR_EAGAIN) {
        /* Current MintVID demuxers use MR_EAGAIN for normal EOF. Preserve a
         * diagnostic if that convention changes or a malformed stream stops us. */
        printf("Error: demux ended with status %ld after %lu video packets.\n",
               (long)st, seen);
    }

    if (ac309i_pace.enabled && n73_vhead && !aborted) {
        /* Natural EOF normally leaves only a tiny tail.  Let the absolute
         * timeline reach it without inventing new PTS or changing the grid.
         * Bound this final CPU-only wait to two seconds of the same 1-MHz
         * timer so malformed timing cannot create an endless close hang. */
        uint32_t drain_start = (uint32_t)ac309i_read_sys_us();
        while (n73_vhead) {
            st = ac309i_service_mov_video(bvp, &ac309i_pace, &sent, &skipped);
            if (st != MR_OK) break;
            if (!n73_vhead) break;
            if (p05_input_poll_stop()) {
                n76_hlog("P05_ESC_STOP_DRAIN");
                aborted = 1;
                break;
            }
            if ((bvp_exec_setsignal(0, 0) & SIGBREAKF_CTRL_C) != 0) { aborted = 1; break; }
            if ((uint32_t)(ac309i_read_sys_us() - drain_start) >= 2000000u) break;
        }
    }
    if (ac309i_pace.enabled) {
        uint32_t elapsed = ac309i_pace.started ?
            (uint32_t)(ac309i_read_sys_us() - ac309i_pace.base_us) : 0;
        DBG_PRINTF("[AC309I] HOST-TIME-PACE sent=%lu elapsed=%lu next=%lu lead=%lu feedlatecnt=%lu maxfeedlate=%lu queuepeak=%lu remaining=%lu\n",
               ac309i_pace.sent_aus, (unsigned long)elapsed,
               (unsigned long)ac309i_pace.target_us, (unsigned long)ac309i_pace.lead_us,
               ac309i_pace.late_count, (unsigned long)ac309i_pace.max_late_us,
               ac309i_pace.queue_peak, n73_vqueued);
    }
    if (n73_vhead) {
        printf("Warning: %lu queued video access unit(s) remained at end of playback and were released.\n", n73_vqueued);
        n73_free_video_queue();
    }

    if (p04_ahead) {
        printf("Warning: %lu startup audio packet(s) remained queued because no video AU was submitted.\n",
               p04_aqueued);
        p04_free_audio_queue();
    }

    /* P32/FAST06: at natural E-AC3 EOF, keep the graph alive until the final
     * scheduled PCM tail has had time to present. ESC remains active here. */
    if (!aborted && st == MR_EAGAIN && eac3_pts_clock_valid && ai && ai->sample_rate) {
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
        if (n76_hostlog) {
            fprintf(n76_hostlog,
                    "FAST06_EAC3_EOF expected=%lu elapsed=%lu wait=%lu first=$%08lx:%08lx last=$%08lx:%08lx\n",
                    (unsigned long)expected_us, (unsigned long)elapsed_us,
                    (unsigned long)wait_us,
                    (unsigned long)(eac3_first_pts_us >> 32),
                    (unsigned long)eac3_first_pts_us,
                    (unsigned long)(eac3_last_pts_us >> 32),
                    (unsigned long)eac3_last_pts_us);
            fflush(n76_hostlog);
        }
        if (wait_us) {
            uint32_t wait_start = (uint32_t)ac309i_read_sys_us();
            while ((uint32_t)(ac309i_read_sys_us() - wait_start) < wait_us) {
                if (p05_input_poll_stop()) {
                    n76_hlog("P05_ESC_STOP_DRAIN");
                    aborted = 1;
                    break;
                }
                if ((bvp_exec_setsignal(0, 0) & SIGBREAKF_CTRL_C) != 0) {
                    aborted = 1;
                    break;
                }
            }
            if (n76_hostlog) {
                fprintf(n76_hostlog, "FAST06_EAC3_EOF_DRAIN_DONE waited=%lu aborted=%d\n",
                        (unsigned long)(uint32_t)(ac309i_read_sys_us() - wait_start), aborted);
                fflush(n76_hostlog);
            }
        }
    }

    DBG_PRINTF("mrberry A/V complete: video=%lu sent=%lu preconfig-skipped=%lu bytes=%lu; "
           "audio=%lu sent=%lu adts-bytes=%lu\n",
           seen, sent, skipped, bvp->bytes, audio_seen, audio_sent, bvp->audio_bytes);
    DBG_PRINTF("Closing BerryVP stream (EOS + Workbench return) ...\n");
    st = mr_berryvp_close(bvp);
    p05_input_close();
    mr_demux_close(d);
    if (st != MR_OK) {
        p06_status_write(P06_STATE_ERROR, "H.264 / VideoCore",
                         "VideoCore cleanup failed");
        return 20;
    }

    p06_status_write(P06_STATE_ENDED, "H.264 / VideoCore",
                     aborted ? "Stopped" : "Finished");
    n76_hlog(aborted ? "P06_STATUS_ENDED_STOPPED" : "P06_STATUS_ENDED_FINISHED");
    n76_hlog("N76_END_OK");
    if (n76_hostlog) fclose(n76_hostlog);
    DBG_PRINTF("N76 PASS: host demux trace completed on N75 scheduler base.\n");
    return 0;
}
