/*
 * zmpiod -- see docs/SDD/SDD_13_LINUX_CLIENT_STACK.md.
 *
 * The single process that owns both zmpio-shm and zmpio-doorbell UIO
 * endpoints: the two-producers-on-the-RX-ring hazard (SDD_13, LIM-LNX-010)
 * is closed by there being exactly one process, not by any locking inside
 * libzmpio.  Talks ABI v2/v3 to CPU1 via libzmpio and exposes a tiny
 * group-gated Unix socket to zmpioctl.
 *
 * The ABI v2 TX ring also carries CPU1's feature stream, and this process is
 * the ONLY thing allowed to read it (REQ-STR-002).  Every doorbell wakeup
 * drains the ring, feature/health messages are written to a ZLOG capture
 * file in the same container CPU1 uses on microSD, and any live watcher gets
 * a text copy.  A second reader -- another zmpiod, or any other process
 * attached to the same ring -- would deterministically steal records.
 *
 * Design choices deliberately kept small:
 *   - single-threaded poll() loop, no worker threads;
 *   - one command per client connection, except WATCH which holds the
 *     connection open (see zmpio_socket_proto.h);
 *   - a slow/malicious client can stall the daemon for at most
 *     ZMPIO_CLIENT_IO_TIMEOUT_MS (SO_RCVTIMEO/SO_SNDTIMEO) -- acceptable
 *     because the socket is filesystem-permission-gated to the `zmpio`
 *     group (trusted local operators), not attacker-facing;
 *   - a WATCH client that stops reading is dropped a line at a time
 *     (MSG_DONTWAIT), never blocking the drain;
 *   - CPU1 online/HELLO state is re-established lazily (ensure_online())
 *     rather than treated as fatal at startup, since the init system may
 *     start zmpiod before CPU1's ABI v3 control block is ready.
 *
 * Build: see software/linux/zmpiod/CMakeLists.txt (LIM-LNX-012).
 */

#define _GNU_SOURCE

#include <errno.h>
#include <grp.h>
#include <libgen.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "libzmpio.h"
#include "zmpio_capture.h"
#include "zmpio_socket_proto.h"
#include "zmpio_stats.h"

#define ZMPIO_MAX_CLIENT_FDS       8U
#define ZMPIO_MAX_WATCHERS         4U
#define ZMPIO_CLIENT_IO_TIMEOUT_MS 2000U
#define ZMPIO_HEARTBEAT_PERIOD_MS  1000U
#define ZMPIO_V3_ATTEMPTS          3U
#define ZMPIO_V3_TIMEOUT_MS        500U
#define ZMPIO_V2_TIMEOUT_MS        500U

#define ZMPIO_CAPTURE_DIR_ENV      "ZMPIO_CAPTURE_DIR"
#define ZMPIO_CAPTURE_BYTES_ENV    "ZMPIO_CAPTURE_MAX_BYTES"
#define ZMPIO_CAPTURE_DEFAULT_DIR  "/var/log/zmpio"

static volatile sig_atomic_t g_stop = 0;

typedef struct {
    zmpio_handle_t *zmpio;
    zmpio_v3_link_t link; /* link.online is the single source of truth for
                           * whether ABI v3 traffic can be sent right now. */
    uint32_t remote_protocol_version;
    int v2_ready;
    int v2_version_confirmed; /* true only after a heartbeat returned
                               * ZMPIO_OK -- distinct from
                               * remote_protocol_version != 0, which a
                               * MISMATCH reply also sets (see
                               * zmpio_v2_heartbeat()'s doc comment) and
                               * would otherwise wrongly look "done". */

    /* ---- Step 7 feature stream ---- */
    zmpio_capture_t capture;
    uint64_t feature_messages;
    uint64_t health_messages;

    /*
     * frame_sequence continuity.  A jump is only acceptable when the stream
     * itself explains it: dropped_since_last on the message after the jump
     * (CPU1 could not publish) or a DSP_HEALTH record showing the PL FIFO
     * dropped frames before CPU1 ever saw them.  Anything left over is
     * exactly the "silent data corruption" the 7.3/7.4 gates forbid, so it
     * gets its own counter instead of being folded into the others.
     */
    uint32_t last_frame_sequence;
    int      last_frame_valid;
    uint64_t frame_gaps;
    uint64_t frame_gap_frames;
    uint64_t frame_gap_frames_explained;

    uint8_t  dsp_health_state;
    uint8_t  dsp_health_fault;

    /* ---- Step 7.4 timing ---- */
    zmpio_stats_hist_t wake_hist;         /* doorbell -> drain complete */
    zmpio_stats_hist_t interarrival_hist; /* between FEATURE_V2 messages */
    uint64_t last_feature_us;             /* CLOCK_MONOTONIC, host domain */
    uint64_t doorbell_wakeups;
    uint64_t messages_from_doorbell;
    uint64_t messages_from_tick;          /* safety-net drains: expected 0 in
                                           * a healthy run, and the evidence
                                           * that no timer polling is doing
                                           * the real work */

    /* ---- live watchers ---- */
    int      watcher_fd[ZMPIO_MAX_WATCHERS];
    unsigned watcher_count;
    uint64_t watch_lines_dropped;
} daemon_state_t;

static void on_signal(int signum)
{
    (void)signum;
    g_stop = 1;
}

static uint64_t monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return ((uint64_t)now.tv_sec * 1000000ULL) +
           ((uint64_t)now.tv_nsec / 1000ULL);
}

static const char *resolve_socket_path(void)
{
    const char *env = getenv(ZMPIO_SOCKET_ENV_PATH);
    return (env != NULL) ? env : ZMPIO_SOCKET_DEFAULT_PATH;
}

/*
 * /run is tmpfs, recreated empty every boot -- unlike the rest of this
 * daemon's assumptions, nothing else in this repo's packaging currently
 * creates dirname(ZMPIO_SOCKET_DEFAULT_PATH), so setup_listen_socket() below
 * must not assume it exists.
 */
static void ensure_parent_dir_exists(const char *path)
{
    char path_copy[512];
    char *dir;
    struct group *grp;

    strncpy(path_copy, path, sizeof(path_copy) - 1U);
    path_copy[sizeof(path_copy) - 1U] = '\0';
    dir = dirname(path_copy); /* may return a pointer into path_copy or a
                               * static buffer -- either way, use it before
                               * calling dirname() again. */

    if (mkdir(dir, 0770) != 0 && errno != EEXIST) {
        fprintf(stderr, "zmpiod: warning: mkdir(%s) failed: %s\n", dir,
                strerror(errno));
        return;
    }

    grp = getgrnam(ZMPIO_SOCKET_GROUP);
    if (grp != NULL) {
        (void)chown(dir, (uid_t)-1, grp->gr_gid);
        (void)chmod(dir, 0770);
    }
}

static int setup_listen_socket(const char *path)
{
    int fd;
    struct sockaddr_un addr;
    struct group *grp;

    ensure_parent_dir_exists(path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("zmpiod: socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "zmpiod: socket path too long: %s\n", path);
        close(fd);
        return -1;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1U);

    /* A stale socket file from a prior crashed run must not block bind()
     * -- this is part of what makes "kill -9 then restart" fast. */
    unlink(path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("zmpiod: bind");
        close(fd);
        return -1;
    }

    if (chmod(path, 0660) != 0) {
        fprintf(stderr, "zmpiod: warning: chmod(%s, 0660) failed: %s\n",
                path, strerror(errno));
    }

    grp = getgrnam(ZMPIO_SOCKET_GROUP);
    if (grp == NULL) {
        fprintf(stderr,
                "zmpiod: warning: group \"%s\" does not exist -- socket "
                "left owned by the daemon's own group, policy gate will "
                "not behave as documented\n",
                ZMPIO_SOCKET_GROUP);
    } else if (chown(path, (uid_t)-1, grp->gr_gid) != 0) {
        fprintf(stderr, "zmpiod: warning: chown(%s, group %s) failed: %s\n",
                path, ZMPIO_SOCKET_GROUP, strerror(errno));
    }

    if (listen(fd, (int)ZMPIO_MAX_CLIENT_FDS) != 0) {
        perror("zmpiod: listen");
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * Lazily (re)establishes ABI v2/v3 link state.  Safe to call repeatedly --
 * cheap no-ops once already ready/online, and each attempt is short
 * (ZMPIO_V3_ATTEMPTS * ZMPIO_V3_TIMEOUT_MS <= 1.5s) so it never blocks the
 * poll() loop for long even when CPU1 is not up yet.
 */
static void ensure_online(daemon_state_t *state)
{
    if (!state->v2_ready) {
        if (zmpio_v2_wait_ready(state->zmpio, 200U) == ZMPIO_OK) {
            state->v2_ready = 1;
        }
    }
    if (state->v2_ready && !state->v2_version_confirmed) {
        zmpio_status_t hb_status = zmpio_v2_heartbeat(
            state->zmpio, 200U, &state->remote_protocol_version);
        if (hb_status == ZMPIO_OK) {
            state->v2_version_confirmed = 1;
        } else if (hb_status == ZMPIO_ERR_PROTOCOL_VERSION_MISMATCH) {
            fprintf(stderr,
                    "zmpiod: ABI v2 protocol version MISMATCH remote=%u "
                    "local=%u -- refusing v2 traffic, will keep retrying "
                    "(CPU1 may be mid-reflash)\n",
                    state->remote_protocol_version, IPC_PROTOCOL_VERSION);
        }
    }
    if (!state->link.online) {
        if (zmpio_v3_wait_cpu1_ready(state->zmpio, 200U) == ZMPIO_OK) {
            zmpio_status_t status = zmpio_v3_hello(
                state->zmpio, ZMPIO_V3_ATTEMPTS, ZMPIO_V3_TIMEOUT_MS,
                &state->link);
            if (status == ZMPIO_OK) {
                fprintf(stderr,
                        "zmpiod: ABI v3 ONLINE session_id=0x%08x "
                        "layout_hash=0x%08x\n",
                        state->link.session_id,
                        state->link.remote_layout_hash);
            } else if (status == ZMPIO_ERR_LAYOUT_MISMATCH) {
                fprintf(stderr,
                        "zmpiod: ABI v3 layout_hash MISMATCH -- CPU1 image "
                        "and this build of common/ have drifted, refusing "
                        "to send commands\n");
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Live watchers                                                       */
/* ------------------------------------------------------------------ */

static void watcher_remove(daemon_state_t *state, unsigned index)
{
    if (index >= state->watcher_count) {
        return;
    }
    close(state->watcher_fd[index]);
    for (; index + 1U < state->watcher_count; ++index) {
        state->watcher_fd[index] = state->watcher_fd[index + 1U];
    }
    --state->watcher_count;
}

/*
 * MSG_DONTWAIT throughout: a watcher that has stopped reading must cost the
 * data path nothing.  A short/failed write drops that line for that client
 * and counts it -- the capture file and the counters remain complete, so a
 * dropped watch line is never a data loss, only a display gap.
 */
static void watchers_broadcast(daemon_state_t *state, const char *line)
{
    unsigned i = 0U;
    size_t len = strlen(line);

    while (i < state->watcher_count) {
        ssize_t sent = send(state->watcher_fd[i], line, len,
                            MSG_NOSIGNAL | MSG_DONTWAIT);

        if (sent < 0) {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                /* Buffer full: skip this line for this client only.  Line
                 * framing survives because nothing partial was written. */
                ++state->watch_lines_dropped;
                ++i;
                continue;
            }
            watcher_remove(state, i);
            continue;
        }
        if ((size_t)sent != len) {
            /* A partial line would leave this client's stream unframed --
             * every later line would be misread rather than merely missing.
             * Drop the watcher instead of feeding it garbage; the capture
             * file and the counters are unaffected. */
            ++state->watch_lines_dropped;
            watcher_remove(state, i);
            continue;
        }
        ++i;
    }
}

/* ------------------------------------------------------------------ */
/* Stream sink                                                         */
/* ------------------------------------------------------------------ */

static void note_frame_sequence(daemon_state_t *state, uint32_t frame_sequence,
                                uint32_t dropped_since_last)
{
    if (state->last_frame_valid) {
        uint32_t expected = state->last_frame_sequence + 1U;

        if (frame_sequence != expected) {
            uint32_t missing = frame_sequence - expected; /* wraps correctly */

            ++state->frame_gaps;
            state->frame_gap_frames += (uint64_t)missing;
            /* dropped_since_last accounts for the frames CPU1 could not
             * publish.  Whatever is left has to be explained by the PL
             * counters in DSP_HEALTH instead; the soak report
             * (tools/soak_report.py) is what reconciles the two. */
            state->frame_gap_frames_explained +=
                (uint64_t)((dropped_since_last < missing) ? dropped_since_last
                                                          : missing);
        }
    }
    state->last_frame_sequence = frame_sequence;
    state->last_frame_valid = 1;
}

static void handle_feature_message(daemon_state_t *state,
                                   const ipc_feature_stream_v1_t *feature)
{
    char line[ZMPIO_SOCKET_REPLY_MAX];
    uint64_t now_us = monotonic_us();

    ++state->feature_messages;
    note_frame_sequence(state, feature->frame.frame_sequence,
                        feature->dropped_since_last);

    if (state->last_feature_us != 0U) {
        zmpio_stats_add(&state->interarrival_hist,
                        now_us - state->last_feature_us);
    }
    state->last_feature_us = now_us;

    /* Two records, exactly as CPU1 writes them to the card: the capture is
     * then directly comparable with LOGnnnnn.BIN through one parser
     * (T7.2-5).  flags mirrors CPU1's own logger_submit() calls -- 0 for the
     * frame, the rule verdict for the rule record. */
    (void)zmpio_capture_write(&state->capture, LOG_SOURCE_FEATURE_V2,
                              feature->timestamp_us, 0U, &feature->frame,
                              (uint16_t)sizeof(feature->frame));
    (void)zmpio_capture_write(&state->capture, LOG_SOURCE_ANOMALY_RULE_V1,
                              feature->timestamp_us,
                              (uint16_t)feature->rule.verdict, &feature->rule,
                              (uint16_t)sizeof(feature->rule));

    if (state->watcher_count != 0U) {
        snprintf(line, sizeof(line),
                 "FEATURE stream_seq=%u dropped=%u ts_us=%llu frame=%u "
                 "rms_q24_8=%u peak_q24_8=%u domfreq_q16_16=%u verdict=%u "
                 "thr_q24_8=%u\n",
                 feature->stream_sequence, feature->dropped_since_last,
                 (unsigned long long)feature->timestamp_us,
                 feature->frame.frame_sequence, feature->frame.rms_q24_8,
                 feature->frame.peak_q24_8,
                 feature->frame.dominant_frequency_q16_16,
                 feature->rule.verdict, feature->rule.threshold_q24_8);
        watchers_broadcast(state, line);
    }
}

static void handle_health_message(daemon_state_t *state,
                                  const ipc_dsp_health_stream_v1_t *health)
{
    char line[ZMPIO_SOCKET_REPLY_MAX];
    /* The trailing 20 bytes of the message are byte-identical to CPU1's
     * zlog_dsp_health_t (asserted in zlog_feature_v2.h), so the capture
     * record is written straight from them. */
    const void *zlog_payload = &health->health_version;

    ++state->health_messages;
    state->dsp_health_state = health->state;
    state->dsp_health_fault = health->fault;

    (void)zmpio_capture_write(&state->capture, LOG_SOURCE_DSP_HEALTH,
                              health->timestamp_us, (uint16_t)health->state,
                              zlog_payload, 20U);

    fprintf(stderr, "zmpiod: DSP health state=%u fault=%u feature_count=%u "
                    "ctrl_drop=%u bridge_drop=%u\n",
            health->state, health->fault, health->feature_count,
            health->ctrl_drop_count, health->bridge_drop_count);

    if (state->watcher_count != 0U) {
        snprintf(line, sizeof(line),
                 "HEALTH stream_seq=%u dropped=%u ts_us=%llu state=%u "
                 "fault=%u feature_count=%u ctrl_drop=%u bridge_drop=%u\n",
                 health->stream_sequence, health->dropped_since_last,
                 (unsigned long long)health->timestamp_us, health->state,
                 health->fault, health->feature_count,
                 health->ctrl_drop_count, health->bridge_drop_count);
        watchers_broadcast(state, line);
    }
}

/* libzmpio guarantees payload_len is exactly the matching struct size before
 * calling this (zmpio_v2.c drain_one()), so the copies below need no further
 * validation.  The copy itself is needed because `payload` points into a
 * buffer libzmpio reuses on the next message. */
static void on_stream_message(uint32_t msg_type, const void *payload,
                              uint32_t payload_len, void *user)
{
    daemon_state_t *state = (daemon_state_t *)user;

    if (msg_type == (uint32_t)MSG_TYPE_FEATURE_V2) {
        ipc_feature_stream_v1_t feature;

        if (payload_len != sizeof(feature)) {
            return;
        }
        memcpy(&feature, payload, sizeof(feature));
        handle_feature_message(state, &feature);
    } else if (msg_type == (uint32_t)MSG_TYPE_DSP_HEALTH) {
        ipc_dsp_health_stream_v1_t health;

        if (payload_len != sizeof(health)) {
            return;
        }
        memcpy(&health, payload, sizeof(health));
        handle_health_message(state, &health);
    }
}

/* Returns the number of messages taken off the ring. */
static uint32_t drain_stream(daemon_state_t *state)
{
    uint32_t drained = 0U;

    if (zmpio_v2_drain(state->zmpio, 0U, &drained) != ZMPIO_OK) {
        return 0U;
    }
    return drained;
}

/* ------------------------------------------------------------------ */
/* Socket commands                                                     */
/* ------------------------------------------------------------------ */

static ssize_t read_line(int fd, char *buf, size_t buf_size)
{
    size_t used = 0U;

    while (used + 1U < buf_size) {
        ssize_t n = recv(fd, &buf[used], 1U, 0);

        if (n < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : -1;
        }
        if (n == 0) {
            break; /* peer closed */
        }
        if (buf[used] == '\n') {
            break;
        }
        ++used;
    }
    buf[used] = '\0';
    return (ssize_t)used;
}

static void write_response(int fd, const char *line)
{
    size_t len = strlen(line);
    size_t sent = 0U;

    while (sent < len) {
        ssize_t n = send(fd, line + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return;
        }
        sent += (size_t)n;
    }
    (void)send(fd, "\n", 1U, MSG_NOSIGNAL);
}

static void handle_status(daemon_state_t *state, char *out, size_t out_size)
{
    uint32_t cpu1_crc = 0U, cpu0_crc = 0U;
    zmpio_v2_stream_stats_t stats;

    zmpio_v3_get_counters(state->zmpio, &cpu1_crc, &cpu0_crc);
    zmpio_v2_get_stream_stats(state->zmpio, &stats);
    snprintf(out, out_size,
             "OK online=%d v2_ready=%d session_id=0x%08x "
             "layout_hash=0x%08x cpu1_crc_drop=%u cpu0_crc_drop=%u "
             "doorbell_count=%u stream_rx=%llu stream_dropped=%llu "
             "last_frame_sequence=%u last_stream_sequence=%u "
             "stream_gaps=%llu frame_gaps=%llu capture=%s "
             "capture_records=%llu dsp_health=%u",
             state->link.online ? 1 : 0,
             (state->v2_ready && state->v2_version_confirmed) ? 1 : 0,
             state->link.session_id, state->link.remote_layout_hash,
             cpu1_crc, cpu0_crc, zmpio_doorbell_count(state->zmpio),
             (unsigned long long)stats.stream_messages,
             (unsigned long long)stats.reported_drops,
             stats.last_frame_sequence, stats.last_stream_sequence,
             (unsigned long long)stats.stream_gaps,
             (unsigned long long)state->frame_gaps,
             state->capture.enabled ? state->capture.path : "disabled",
             (unsigned long long)state->capture.records_written,
             state->dsp_health_state);
}

/*
 * Everything the 7.2/7.4 gates need to be checked mechanically, in one line:
 * this side's totals, CPU1's own totals, and the timing histograms.  Kept
 * separate from STATUS because it costs an ABI v2 round trip to CPU1 while
 * STATUS is answered purely from local state.
 */
static void handle_stream_status(daemon_state_t *state, char *out,
                                 size_t out_size)
{
    zmpio_v2_stream_stats_t stats;
    ipc_stream_status_t cpu1;
    char wake[128];
    char inter[128];
    zmpio_status_t status;

    zmpio_v2_get_stream_stats(state->zmpio, &stats);
    zmpio_stats_format(&state->wake_hist, wake, sizeof(wake));
    zmpio_stats_format(&state->interarrival_hist, inter, sizeof(inter));

    memset(&cpu1, 0, sizeof(cpu1));
    if (!state->v2_ready || !state->v2_version_confirmed) {
        status = ZMPIO_ERR_NOT_READY;
    } else {
        status = zmpio_v2_stream_status(state->zmpio, ZMPIO_V2_TIMEOUT_MS,
                                        &cpu1);
    }

    snprintf(out, out_size,
             "OK cpu1_status=%d cpu1_feature_count=%u cpu1_published=%u "
             "cpu1_dropped=%u cpu1_health_published=%u cpu1_ctrl_drop=%u "
             "cpu1_bridge_drop=%u cpu1_doorbell_rings=%u "
             "rx_feature=%llu rx_health=%llu rx_reported_drops=%llu "
             "rx_stream_gaps=%llu rx_stream_gap_messages=%llu "
             "rx_malformed=%llu rx_no_sink=%llu orphan_overflow=%llu "
             "frame_gaps=%llu frame_gap_frames=%llu "
             "frame_gap_frames_explained=%llu "
             "doorbell_wakeups=%llu msgs_from_doorbell=%llu "
             "msgs_from_tick=%llu watch_lines_dropped=%llu "
             "capture_records=%llu capture_rotations=%llu "
             "capture_write_errors=%llu wake_us[%s] interarrival_us[%s]",
             (int)status, cpu1.feature_count, cpu1.frames_published,
             cpu1.frames_dropped, cpu1.health_published, cpu1.ctrl_drop_count,
             cpu1.bridge_drop_count, cpu1.doorbell_rings,
             (unsigned long long)stats.feature_messages,
             (unsigned long long)stats.health_messages,
             (unsigned long long)stats.reported_drops,
             (unsigned long long)stats.stream_gaps,
             (unsigned long long)stats.stream_gap_messages,
             (unsigned long long)stats.malformed,
             (unsigned long long)stats.no_sink_dropped,
             (unsigned long long)stats.orphan_overflow,
             (unsigned long long)state->frame_gaps,
             (unsigned long long)state->frame_gap_frames,
             (unsigned long long)state->frame_gap_frames_explained,
             (unsigned long long)state->doorbell_wakeups,
             (unsigned long long)state->messages_from_doorbell,
             (unsigned long long)state->messages_from_tick,
             (unsigned long long)state->watch_lines_dropped,
             (unsigned long long)state->capture.records_written,
             (unsigned long long)state->capture.rotations,
             (unsigned long long)state->capture.write_errors,
             wake, inter);
}

static void handle_set_dsp_config(daemon_state_t *state, const char *args,
                                  char *out, size_t out_size)
{
    zmpio_v3_set_dsp_config_t config;
    int32_t config_status = 0;
    zmpio_status_t status;
    unsigned coeff_set_id = 0U, fft_scale_shift = 0U, feature_mask = 0U;

    if (!state->link.online) {
        snprintf(out, out_size, "ERR not_online");
        return;
    }
    if (sscanf(args, "%u %u %u", &coeff_set_id, &fft_scale_shift,
              &feature_mask) != 3) {
        snprintf(out, out_size, "ERR bad_args");
        return;
    }

    memset(&config, 0, sizeof(config));
    config.coeff_set_id = coeff_set_id;
    config.fft_scale_shift = fft_scale_shift;
    config.feature_mask = feature_mask;

    status = zmpio_v3_set_dsp_config(state->zmpio, &state->link, &config,
                                     ZMPIO_V3_ATTEMPTS, ZMPIO_V3_TIMEOUT_MS,
                                     &config_status);
    if (status != ZMPIO_OK) {
        snprintf(out, out_size, "ERR transport_status_%d", (int)status);
        return;
    }
    snprintf(out, out_size, "OK status=%d", config_status);
}

static void handle_log_command(daemon_state_t *state, msg_type_t type,
                               char *out, size_t out_size)
{
    ipc_message_t reply;
    zmpio_status_t status;

    if (!state->v2_ready || !state->v2_version_confirmed) {
        snprintf(out, out_size, "ERR not_ready");
        return;
    }
    status = zmpio_v2_request(state->zmpio, type, NULL, 0U,
                              ZMPIO_V2_TIMEOUT_MS, &reply);
    if (status != ZMPIO_OK) {
        snprintf(out, out_size, "ERR transport_status_%d", (int)status);
        return;
    }
    if (reply.header.type != (uint32_t)MSG_TYPE_ACK) {
        snprintf(out, out_size, "ERR cpu1_error");
        return;
    }
    snprintf(out, out_size, "OK");
}

/* Returns 1 when the connection has been handed to the watcher list and must
 * NOT be closed by the caller. */
static int handle_client(int fd, daemon_state_t *state)
{
    char line[ZMPIO_SOCKET_LINE_MAX];
    char response[ZMPIO_SOCKET_REPLY_MAX];
    struct timeval io_timeout;
    ssize_t n;

    io_timeout.tv_sec = (time_t)(ZMPIO_CLIENT_IO_TIMEOUT_MS / 1000U);
    io_timeout.tv_usec = (suseconds_t)(ZMPIO_CLIENT_IO_TIMEOUT_MS % 1000U) *
                         1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));

    n = read_line(fd, line, sizeof(line));
    if (n <= 0) {
        return 0;
    }

    ensure_online(state);

    if (strcmp(line, "STATUS") == 0) {
        handle_status(state, response, sizeof(response));
    } else if (strcmp(line, "STREAM_STATUS") == 0) {
        handle_stream_status(state, response, sizeof(response));
    } else if (strncmp(line, "SET_DSP_CONFIG ", 15U) == 0) {
        handle_set_dsp_config(state, line + 15, response, sizeof(response));
    } else if (strcmp(line, "START_LOG") == 0) {
        handle_log_command(state, MSG_TYPE_LOG_START, response,
                           sizeof(response));
    } else if (strcmp(line, "STOP_LOG") == 0) {
        handle_log_command(state, MSG_TYPE_LOG_STOP, response,
                           sizeof(response));
    } else if (strcmp(line, "WATCH") == 0) {
        if (state->watcher_count >= ZMPIO_MAX_WATCHERS) {
            snprintf(response, sizeof(response), "ERR too_many_watchers");
        } else {
            write_response(fd, "OK watching");
            state->watcher_fd[state->watcher_count++] = fd;
            return 1;
        }
    } else {
        snprintf(response, sizeof(response), "ERR unknown_command");
    }

    write_response(fd, response);
    return 0;
}

static uint64_t capture_max_bytes_from_env(void)
{
    const char *env = getenv(ZMPIO_CAPTURE_BYTES_ENV);

    if (env == NULL) {
        return 0U; /* writer's default */
    }
    return strtoull(env, NULL, 0);
}

int main(void)
{
    daemon_state_t state;
    int listen_fd;
    const char *socket_path = resolve_socket_path();
    const char *capture_dir = getenv(ZMPIO_CAPTURE_DIR_ENV);
    zmpio_status_t open_status;
    uint64_t last_heartbeat_us;

    memset(&state, 0, sizeof(state));
    zmpio_stats_reset(&state.wake_hist);
    zmpio_stats_reset(&state.interarrival_hist);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* CPU1/the PL bitstream may not be programmed yet this early in boot
     * (init ordering, not this daemon's job to fix -- the udev rule + init
     * script in 7.5 is what should gate this service's start on the UIO
     * devices actually existing). Retry with a short bounded backoff rather
     * than exiting non-zero and having the init system give up. */
    for (;;) {
        open_status = zmpio_open(&state.zmpio);
        if (open_status == ZMPIO_OK) {
            break;
        }
        fprintf(stderr,
                "zmpiod: zmpio_open failed (status=%d), retrying in 1s -- "
                "is the PL bitstream programmed and are the UIO nodes "
                "present (cat /sys/class/uio/uio*/name)?\n",
                (int)open_status);
        if (g_stop) {
            return 1;
        }
        sleep(1U);
    }

    /* Register the sink BEFORE anything can drain: a stream message that
     * arrives with no sink is counted and discarded (libzmpio.h), and the
     * ensure_online() heartbeat below already drains. */
    (void)zmpio_v2_set_stream_sink(state.zmpio, on_stream_message, &state);

    if (zmpio_capture_open(&state.capture,
                           (capture_dir != NULL) ? capture_dir
                                                 : ZMPIO_CAPTURE_DEFAULT_DIR,
                           capture_max_bytes_from_env()) != 0) {
        fprintf(stderr,
                "zmpiod: continuing WITHOUT an on-disk capture -- the live "
                "stream and all counters still work, but there will be no "
                "file to cross-check against the microSD log\n");
    }

    listen_fd = setup_listen_socket(socket_path);
    if (listen_fd < 0) {
        zmpio_capture_close(&state.capture);
        zmpio_close(state.zmpio);
        return 1;
    }

    ensure_online(&state);
    last_heartbeat_us = monotonic_us();

    fprintf(stderr, "zmpiod: ready, socket=%s\n", socket_path);

    /*
     * The doorbell fd sits in the SAME poll() set as the listening socket and
     * the watchers -- this, not a timer, is what satisfies the requirement
     * that a CPU1 event wakes poll() directly rather than relying on timer
     * polling, and it carries the Step 7 feature stream. The
     * ZMPIO_HEARTBEAT_PERIOD_MS
     * timeout is a 1 Hz fallback for ensure_online()/flush and a safety-net
     * drain whose message count is reported separately
     * (msgs_from_tick): if that number is not ~0 in an evidence run, the
     * doorbell path is not doing its job and the gate has not been met.
     */
    while (!g_stop) {
        struct pollfd fds[2U + ZMPIO_MAX_WATCHERS];
        const int doorbell_slot = 0;
        const int listen_slot = 1;
        nfds_t nfds;
        unsigned i;
        unsigned polled_watchers;
        int poll_result;
        uint64_t now_us;

        fds[doorbell_slot].fd = zmpio_doorbell_fd(state.zmpio);
        fds[doorbell_slot].events = POLLIN;
        fds[doorbell_slot].revents = 0;

        fds[listen_slot].fd = listen_fd;
        fds[listen_slot].events = POLLIN;
        fds[listen_slot].revents = 0;

        for (i = 0U; i < state.watcher_count; ++i) {
            fds[2U + i].fd = state.watcher_fd[i];
            /* Watchers never send anything after WATCH; POLLIN here means
             * "closed", which is how a finished `zmpioctl watch` is
             * reaped. */
            fds[2U + i].events = POLLIN;
            fds[2U + i].revents = 0;
        }
        nfds = (nfds_t)(2U + state.watcher_count);
        /* Snapshot BEFORE accept(): a WATCH accepted below grows
         * watcher_count, and the reaping loop must not read fds[] slots that
         * this poll() never filled -- their revents is uninitialised stack,
         * which would drop the brand-new watcher at random. */
        polled_watchers = state.watcher_count;

        poll_result = poll(fds, nfds, (int)ZMPIO_HEARTBEAT_PERIOD_MS);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("zmpiod: poll");
            break;
        }

        if ((poll_result > 0) && (fds[doorbell_slot].revents & POLLIN)) {
            uint64_t wake_start_us = monotonic_us();
            uint32_t drained;

            /* Data is already available -- this will not block. Servicing
             * (drain/ACK/re-enable) happens inside libzmpio, same ordering
             * tools/uio_spike.c validates on real hardware in Step 6.2. */
            if (zmpio_wait_doorbell(state.zmpio) == ZMPIO_OK) {
                ++state.doorbell_wakeups;
                drained = drain_stream(&state);
                state.messages_from_doorbell += drained;
                zmpio_stats_add(&state.wake_hist,
                                monotonic_us() - wake_start_us);
            }
        }

        if ((poll_result > 0) && (fds[listen_slot].revents & POLLIN)) {
            int client_fd = accept(listen_fd, NULL, NULL);
            if (client_fd >= 0) {
                if (!handle_client(client_fd, &state)) {
                    close(client_fd);
                }
            }
        }

        /* Walk backwards so removing one does not skip the next. */
        for (i = polled_watchers; i > 0U; --i) {
            short revents = fds[2U + (i - 1U)].revents;

            if ((revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
                watcher_remove(&state, i - 1U);
            }
        }

        now_us = monotonic_us();
        if ((now_us - last_heartbeat_us) >=
            ((uint64_t)ZMPIO_HEARTBEAT_PERIOD_MS * 1000ULL)) {
            uint32_t drained;

            ensure_online(&state);
            /* Safety net only: if the doorbell ever stops arriving (a GIC
             * re-init on CPU1's side, a lost UIO re-enable), the stream
             * still gets through, and msgs_from_tick makes that visible
             * instead of hiding it. */
            drained = drain_stream(&state);
            state.messages_from_tick += drained;
            zmpio_capture_flush(&state.capture);
            last_heartbeat_us = now_us;
        }
    }

    fprintf(stderr, "zmpiod: shutting down\n");
    while (state.watcher_count > 0U) {
        watcher_remove(&state, state.watcher_count - 1U);
    }
    close(listen_fd);
    unlink(socket_path);
    zmpio_capture_close(&state.capture);
    zmpio_close(state.zmpio);
    return 0;
}
