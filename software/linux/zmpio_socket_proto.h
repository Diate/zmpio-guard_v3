#ifndef ZMPIO_SOCKET_PROTO_H
#define ZMPIO_SOCKET_PROTO_H

/*
 * zmpiod <-> zmpioctl socket protocol (Step 6.4, docs/PLAN_BUOC_6.md SS4
 * 6.4; extended in Step 7.2 T7.2-6).  Plain text, one command per line, one
 * command per connection -- zmpioctl connects, writes one line, reads one
 * line back, closes.  No persistent session state to get wrong.
 *
 * WATCH is the single exception: it keeps the connection open and streams
 * one line per feature/health message until the client disconnects.  It is
 * a read-only view of what the daemon is already receiving -- a watcher that
 * stops reading is dropped a line at a time and never back-pressures the
 * data path (see watchers_broadcast() in zmpiod.c).
 *
 * Socket file permissions (mode 0660, group ZMPIO_SOCKET_GROUP) are the
 * enforcement mechanism for the pass gate "user ngoai group zmpio bi tu choi
 * command doi trang thai" -- AF_UNIX connect() honours the socket inode's
 * permission bits like any other file, so a non-member's connect() fails
 * with EACCES before zmpiod even sees the attempt.  Neither side needs to
 * parse SO_PEERCRED for this to hold.
 *
 * Requests (zmpioctl -> zmpiod), one line, space-separated:
 *   STATUS
 *   STREAM_STATUS
 *   SET_DSP_CONFIG <coeff_set_id> <fft_scale_shift> <feature_mask>
 *   START_LOG
 *   STOP_LOG
 *   WATCH
 *
 * Responses (zmpiod -> zmpioctl), one line:
 *   STATUS      -> "OK online=<0|1> v2_ready=<0|1> session_id=0x<hex>
 *                   layout_hash=0x<hex> cpu1_crc_drop=<n> cpu0_crc_drop=<n>
 *                   doorbell_count=<n> stream_rx=<n> stream_dropped=<n>
 *                   last_frame_sequence=<n> last_stream_sequence=<n>
 *                   stream_gaps=<n> frame_gaps=<n> capture=<path|disabled>
 *                   capture_records=<n> dsp_health=<n>"
 *   STREAM_STATUS -> "OK cpu1_status=<zmpio_status_t> cpu1_* ... rx_* ...
 *                     wake_us[...] interarrival_us[...]" -- every counter the
 *                     Step 7.2/7.4 gates are checked against, from BOTH
 *                     sides of the ring in one atomic-enough sample.
 *   SET_DSP_CONFIG -> "OK status=<zmpio_v3_config_status_t>" or "ERR <reason>"
 *   START_LOG/STOP_LOG -> "OK" or "ERR <reason>"
 *   WATCH       -> "OK watching", then a stream of:
 *                   "FEATURE stream_seq=<n> dropped=<n> ts_us=<n> frame=<n>
 *                    rms_q24_8=<n> peak_q24_8=<n> domfreq_q16_16=<n>
 *                    verdict=<0|1> thr_q24_8=<n>"
 *                   "HEALTH stream_seq=<n> dropped=<n> ts_us=<n> state=<n>
 *                    fault=<n> feature_count=<n> ctrl_drop=<n>
 *                    bridge_drop=<n>"
 */

#define ZMPIO_SOCKET_DEFAULT_PATH  "/run/zmpio/zmpiod.sock"
#define ZMPIO_SOCKET_ENV_PATH      "ZMPIO_SOCKET_PATH"
#define ZMPIO_SOCKET_GROUP         "zmpio"

/* Longest command line a client may send. */
#define ZMPIO_SOCKET_LINE_MAX      256U

/* Longest response line the daemon may send.  STREAM_STATUS deliberately
 * puts every gate-relevant counter on one line so a test script captures a
 * single consistent sample rather than stitching several round trips
 * together; that line is ~800 characters. */
#define ZMPIO_SOCKET_REPLY_MAX     2048U

#endif
