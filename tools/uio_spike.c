/*
 * uio_spike.c -- Step 6.2 bring-up spike (docs/PLAN_BUOC_6.md SS4 6.2).
 *
 * Purpose: prove, on real PetaLinux hardware, that the two generic-UIO
 * endpoints declared in deploy/petalinux_overlay/system-user-openamp-
 * template.dtsi behave the way libzmpio (Step 6.3) will assume:
 *
 *   1. zmpio-doorbell's maps/map0/{addr,size,offset} match the DT reg
 *      (0x40002000/0x1000) and common/zmpio_doorbell_regs.h's register
 *      offsets read/write correctly through that mapping.
 *   2. zmpio-shm's map is usable with NO interrupts property (UIO_IRQ_NONE)
 *      -- this is explicitly unverified, see the doc comment on zmpio_shm
 *      in the dtsi.
 *   3. The ACK-before-re-enable ordering (SDD_10 SS5, docs/PLAN_BUOC_6.md
 *      SS4 6.2) does not drop or duplicate doorbell events under the same
 *      10,000-event stress CPU1 already runs for Step 5
 *      (run_doorbell_fault_test() in firmware/cpu0_application/src/main.c).
 *
 * This is a standalone bring-up tool, not part of libzmpio -- it exists to
 * be thrown away once 6.2's pass gate is captured, and is deliberately
 * single-purpose (no CLI framework, no retry policy beyond what the gate
 * needs) so a board log from running it is unambiguous evidence for exactly
 * one thing at a time.
 *
 * Build (on the target's own toolchain, or cross-compiled):
 *   arm-linux-gnueabihf-gcc -std=gnu11 -O2 -Wall -Wextra \
 *       -I<repo_root>/common -o uio_spike tools/uio_spike.c
 *
 * Usage:
 *   uio_spike [doorbell_uio_name] [count]
 *     doorbell_uio_name  default "zmpio-doorbell" -- pass the fallback name
 *                        noted in the dtsi's doc comment
 *                        (e.g. "zmpio_doorbell@40002000") if the per-uioN
 *                        "name" attribute under /sys/class/uio did not
 *                        honour linux,uio-name on this kernel.
 *     count              default 10000 -- exits after this many doorbell
 *                        wakeups (matching the Step 5 stress test size) or
 *                        never if 0.
 *
 * VERIFICATION OUTPUT THIS TOOL MUST PRODUCE FOR THE 6.2 GATE (see
 * docs/PLAN_BUOC_6.md SS4 6.2 "Pass gate 6.2" -- capture stdout to a file
 * under docs/architecture/evidence/step6_uio_spike_<date>_<time>_uart*.log
 * alongside the matching CPU1 UART1 log from run_doorbell_fault_test()):
 *   - the resolved map addr/size/offset line, to diff by hand against
 *     firmware/platform_dual/hw/sdt/pl.dtsi's zmpio_doorbell_0 reg
 *   - "wakeups=<n> dbell_count_delta=<n>" must show n==n==the requested
 *     count with zero drift
 *   - "spurious=<n>" must be 0 outside the deliberate spurious-IRQ test
 *     CPU1 already runs as part of the same fault test
 *   - no "ACK retry exhausted" line (bounded-retry PENDING recheck failing)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zmpio_doorbell_regs.h"

#define UIO_CLASS_DIR      "/sys/class/uio"
#define MAX_UIO_DEVICES    32U
#define DEFAULT_DOORBELL_UIO_NAME "zmpio-doorbell"
#define DEFAULT_TARGET_COUNT      10000U
#define ACK_RECHECK_MAX_RETRIES   16U
#define ACK_RECHECK_SLEEP_US      100U

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signum)
{
    (void)signum;
    g_stop = 1;
}

/* Scans /sys/class/uio/uioN/name for N in [0, MAX_UIO_DEVICES) and returns
 * the uio index whose name matches `want`, or -1 if not found. Deliberately
 * NOT /dev/uio0 by convention -- see this file's header comment and
 * docs/PLAN_BUOC_6.md SS4 6.2's naming requirement. */
static int find_uio_by_name(const char *want)
{
    unsigned i;

    for (i = 0U; i < MAX_UIO_DEVICES; ++i) {
        char path[128];
        char name[128];
        FILE *f;
        size_t len;

        snprintf(path, sizeof(path), UIO_CLASS_DIR "/uio%u/name", i);
        f = fopen(path, "r");
        if (f == NULL) {
            continue;
        }
        if (fgets(name, sizeof(name), f) == NULL) {
            fclose(f);
            continue;
        }
        fclose(f);

        len = strlen(name);
        while ((len > 0U) &&
               ((name[len - 1U] == '\n') || (name[len - 1U] == '\r'))) {
            name[--len] = '\0';
        }

        if (strcmp(name, want) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Reads one decimal/hex sysfs attribute (e.g. maps/map0/{addr,size,offset})
 * as a 64-bit value. sysfs prints these as "0xNNNNNNNN\n". */
static int read_sysfs_hex(unsigned uio_index, const char *attr, uint64_t *out)
{
    char path[160];
    FILE *f;
    int rc;
    unsigned long long value;

    snprintf(path, sizeof(path), UIO_CLASS_DIR "/uio%u/maps/map0/%s",
             uio_index, attr);
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    rc = fscanf(f, "%llx", &value);
    fclose(f);
    if (rc != 1) {
        return -1;
    }
    *out = (uint64_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    const char *uio_name =
        (argc > 1) ? argv[1] : DEFAULT_DOORBELL_UIO_NAME;
    unsigned long target_count =
        (argc > 2) ? strtoul(argv[2], NULL, 10) : DEFAULT_TARGET_COUNT;
    int uio_index;
    char devpath[64];
    int fd;
    uint64_t map_addr = 0U, map_size = 0U, map_offset = 0U;
    volatile uint8_t *regs;
    unsigned long wakeups = 0U;
    unsigned long spurious = 0U;
    unsigned long ack_retry_exhausted_count = 0U;
    uint32_t dbell_count_start;

    signal(SIGINT, on_sigint);

    uio_index = find_uio_by_name(uio_name);
    if (uio_index < 0) {
        fprintf(stderr,
                "uio_spike: no UIO device named \"%s\" under " UIO_CLASS_DIR
                " -- check `cat " UIO_CLASS_DIR "/uio*/name`, "
                "uio_pdrv_genirq.of_id=generic-uio on the kernel cmdline, "
                "and CONFIG_UIO_PDRV_GENIRQ\n",
                uio_name);
        return 1;
    }

    if (read_sysfs_hex((unsigned)uio_index, "addr", &map_addr) != 0 ||
        read_sysfs_hex((unsigned)uio_index, "size", &map_size) != 0 ||
        read_sysfs_hex((unsigned)uio_index, "offset", &map_offset) != 0) {
        fprintf(stderr, "uio_spike: failed to read maps/map0/* for uio%d\n",
                uio_index);
        return 1;
    }
    /* This line is the exact evidence docs/PLAN_BUOC_6.md T6.2-4 asks for:
     * diff it by hand against pl.dtsi's zmpio_doorbell_0 reg (0x40002000
     * size 0x1000). */
    printf("uio_spike: uio%d name=\"%s\" addr=0x%llx size=0x%llx "
           "offset=0x%llx\n",
           uio_index, uio_name, (unsigned long long)map_addr,
           (unsigned long long)map_size, (unsigned long long)map_offset);

    snprintf(devpath, sizeof(devpath), "/dev/uio%d", uio_index);
    fd = open(devpath, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "uio_spike: open(%s) failed: %s\n", devpath,
                strerror(errno));
        return 1;
    }

    regs = (volatile uint8_t *)mmap(NULL, (size_t)map_size,
                                    PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                    0);
    if (regs == MAP_FAILED) {
        fprintf(stderr, "uio_spike: mmap failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    dbell_count_start =
        *(volatile uint32_t *)(regs + ZMPIO_DOORBELL_REG_COUNT);
    printf("uio_spike: DBELL_COUNT at start = %u, target wakeups = %lu\n",
           dbell_count_start, target_count);

    /* uio_pdrv_genirq blocking-read protocol (Documentation/driver-api/
     * uio-howto.rst "Waiting for interrupts"): read() blocks until an
     * interrupt has occurred since the last successful read, then returns
     * the cumulative interrupt count in a uint32_t. The IRQ is masked by
     * the driver on assertion and must be explicitly re-enabled with a
     * write() of a nonzero uint32_t once userspace is done handling it --
     * exactly mirroring CPU0's own bare-metal doorbell driver
     * (firmware/cpu0_application/src/pl_doorbell.c) mask-on-ISR-entry /
     * enable-after-drain discipline. */
    while ((g_stop == 0) &&
           ((target_count == 0U) || (wakeups < target_count))) {
        uint32_t irq_count;
        ssize_t n;
        uint32_t status;
        unsigned retry;
        int acked = 0;

        n = read(fd, &irq_count, sizeof(irq_count));
        if (n != (ssize_t)sizeof(irq_count)) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "uio_spike: read() failed: %s\n",
                    strerror(errno));
            break;
        }
        ++wakeups;

        status = *(volatile uint32_t *)(regs + ZMPIO_DOORBELL_REG_STATUS);
        if ((status & ZMPIO_DOORBELL_BIT_PENDING) == 0U) {
            /* Woke up with nothing pending -- either a genuinely spurious
             * IRQ (Step 5's storm/spurious test) or a race where CPU1
             * already re-armed and re-fired between our read() and this
             * STATUS read; either way, count it and move on WITHOUT
             * touching ACK (ACK is only meaningful once PENDING was
             * actually observed set, per SDD_10). */
            ++spurious;
        } else {
            /* ACK before re-enable, in that order -- see the doc comment
             * on this ordering requirement in docs/PLAN_BUOC_6.md SS4 6.2.
             * Re-enabling the UIO IRQ first would let a SET landing between
             * re-enable and ACK be silently cleared by this ACK without
             * ever being counted. */
            *(volatile uint32_t *)(regs + ZMPIO_DOORBELL_REG_ACK) =
                ZMPIO_DOORBELL_BIT_ACK;

            for (retry = 0U; retry < ACK_RECHECK_MAX_RETRIES; ++retry) {
                status =
                    *(volatile uint32_t *)(regs + ZMPIO_DOORBELL_REG_STATUS);
                if ((status & ZMPIO_DOORBELL_BIT_PENDING) == 0U) {
                    acked = 1;
                    break;
                }
                /* Still pending: a new SET landed after our ACK (level
                 * signal, SDD_10 SS5) -- ACK again and recheck, bounded. */
                *(volatile uint32_t *)(regs + ZMPIO_DOORBELL_REG_ACK) =
                    ZMPIO_DOORBELL_BIT_ACK;
                usleep(ACK_RECHECK_SLEEP_US);
            }
            if (!acked) {
                ++ack_retry_exhausted_count;
                fprintf(stderr,
                        "uio_spike: ACK retry exhausted at wakeup %lu "
                        "(STATUS still PENDING after %u retries)\n",
                        wakeups, ACK_RECHECK_MAX_RETRIES);
            }
        }

        /* Re-enable the UIO IRQ last, only after ACK (or the spurious
         * no-op above) has settled -- write of a nonzero uint32_t per the
         * uio_pdrv_genirq protocol. */
        {
            uint32_t one = 1U;
            if (write(fd, &one, sizeof(one)) != (ssize_t)sizeof(one)) {
                fprintf(stderr,
                        "uio_spike: re-enable write() failed at wakeup "
                        "%lu: %s\n",
                        wakeups, strerror(errno));
                break;
            }
        }

        if ((wakeups % 1000U) == 0U) {
            uint32_t now =
                *(volatile uint32_t *)(regs + ZMPIO_DOORBELL_REG_COUNT);
            printf("uio_spike: wakeups=%lu dbell_count=%u spurious=%lu\n",
                   wakeups, now, spurious);
        }
    }

    {
        uint32_t dbell_count_end =
            *(volatile uint32_t *)(regs + ZMPIO_DOORBELL_REG_COUNT);
        printf("uio_spike: DONE wakeups=%lu dbell_count_delta=%u "
               "spurious=%lu ack_retry_exhausted=%lu\n",
               wakeups, (unsigned)(dbell_count_end - dbell_count_start),
               spurious, ack_retry_exhausted_count);
    }

    munmap((void *)regs, (size_t)map_size);
    close(fd);

    return (ack_retry_exhausted_count == 0U) ? 0 : 1;
}
