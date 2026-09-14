/*
 * zmpioctl -- Step 6.4 (docs/PLAN_BUOC_6.md SS4 6.4), extended in Step 7.2
 * (T7.2-6).  Thin client for zmpiod's Unix socket (see
 * software/linux/zmpio_socket_proto.h for the wire protocol both sides
 * share).  One command per invocation, exit code 0 on "OK ...", 1 on
 * "ERR ..."/timeout/connect failure -- scriptable by design, same spirit as
 * `systemctl is-active` or similar single-shot status tools.
 *
 * `watch` is the one long-running mode: it prints one line per feature/health
 * message until interrupted.  It never talks to CPU1 itself -- zmpiod is
 * still the only reader of the ABI v2 ring (REQ-STR-002), and this is just a
 * relay of what the daemon already received.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "zmpio_socket_proto.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signum)
{
    (void)signum;
    g_stop = 1;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s status\n"
            "       %s stream-status\n"
            "       %s watch\n"
            "       %s set-dsp-config <coeff_set_id> <fft_scale_shift> <feature_mask>\n"
            "       %s start-log\n"
            "       %s stop-log\n"
            "\n"
            "Connects to %s (override with %s).\n",
            program, program, program, program, program, program,
            ZMPIO_SOCKET_DEFAULT_PATH, ZMPIO_SOCKET_ENV_PATH);
}

static int connect_to_daemon(void)
{
    int fd;
    struct sockaddr_un addr;
    const char *path = getenv(ZMPIO_SOCKET_ENV_PATH);

    if (path == NULL) {
        path = ZMPIO_SOCKET_DEFAULT_PATH;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("zmpioctl: socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1U);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "zmpioctl: connect(%s): %s\n", path,
                strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int send_line(int fd, const char *command)
{
    size_t len = strlen(command);
    size_t sent = 0U;

    while (sent < len) {
        ssize_t n = send(fd, command + sent, len - sent, 0);
        if (n <= 0) {
            fprintf(stderr, "zmpioctl: send failed: %s\n", strerror(errno));
            return -1;
        }
        sent += (size_t)n;
    }
    if (send(fd, "\n", 1U, 0) != 1) {
        fprintf(stderr, "zmpioctl: send failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Reads one '\n'-terminated line.  Returns its length, 0 on clean EOF before
 * any byte, -1 on error. */
static ssize_t read_reply_line(int fd, char *line, size_t line_size)
{
    size_t used = 0U;

    while (used + 1U < line_size) {
        ssize_t n = recv(fd, &line[used], 1U, 0);

        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            break;
        }
        if (line[used] == '\n') {
            break;
        }
        ++used;
    }
    line[used] = '\0';
    return (ssize_t)used;
}

static int send_command_and_print_reply(const char *command)
{
    int fd;
    char line[ZMPIO_SOCKET_REPLY_MAX];
    ssize_t used;

    fd = connect_to_daemon();
    if (fd < 0) {
        return 1;
    }
    if (send_line(fd, command) != 0) {
        close(fd);
        return 1;
    }

    used = read_reply_line(fd, line, sizeof(line));
    close(fd);

    if (used <= 0) {
        fprintf(stderr, "zmpioctl: no reply from zmpiod\n");
        return 1;
    }

    printf("%s\n", line);
    return (strncmp(line, "OK", 2U) == 0) ? 0 : 1;
}

/*
 * Streams until the daemon closes the connection or the user interrupts.
 * Output is line-buffered explicitly so `zmpioctl watch | tee evidence.log`
 * records lines as they arrive rather than in 4 KiB bursts -- which matters
 * for a HIL run where the log has to line up with a fault injected by hand.
 */
static int watch_stream(void)
{
    int fd;
    char line[ZMPIO_SOCKET_REPLY_MAX];
    ssize_t used;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fd = connect_to_daemon();
    if (fd < 0) {
        return 1;
    }
    if (send_line(fd, "WATCH") != 0) {
        close(fd);
        return 1;
    }

    used = read_reply_line(fd, line, sizeof(line));
    if ((used <= 0) || (strncmp(line, "OK", 2U) != 0)) {
        fprintf(stderr, "zmpioctl: %s\n", (used > 0) ? line : "no reply");
        close(fd);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    while (!g_stop) {
        used = read_reply_line(fd, line, sizeof(line));
        if (used < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (used == 0) {
            break; /* daemon closed the connection */
        }
        printf("%s\n", line);
    }

    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    char command[ZMPIO_SOCKET_LINE_MAX];

    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "status") == 0) {
        snprintf(command, sizeof(command), "STATUS");
    } else if (strcmp(argv[1], "stream-status") == 0) {
        snprintf(command, sizeof(command), "STREAM_STATUS");
    } else if (strcmp(argv[1], "watch") == 0) {
        return watch_stream();
    } else if (strcmp(argv[1], "set-dsp-config") == 0) {
        if (argc != 5) {
            print_usage(argv[0]);
            return 2;
        }
        snprintf(command, sizeof(command), "SET_DSP_CONFIG %s %s %s",
                 argv[2], argv[3], argv[4]);
    } else if (strcmp(argv[1], "start-log") == 0) {
        snprintf(command, sizeof(command), "START_LOG");
    } else if (strcmp(argv[1], "stop-log") == 0) {
        snprintf(command, sizeof(command), "STOP_LOG");
    } else {
        print_usage(argv[0]);
        return 2;
    }

    return send_command_and_print_reply(command);
}
