#define _GNU_SOURCE

#include "zmpio_uio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define UIO_CLASS_DIR   "/sys/class/uio"
#define MAX_UIO_DEVICES 32U

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

int zmpio_uio_open(const char *uio_name, zmpio_uio_region_t *out)
{
    int uio_index;
    uint64_t map_size = 0U;
    char devpath[64];
    int fd;
    volatile uint8_t *base;

    memset(out, 0, sizeof(*out));

    uio_index = find_uio_by_name(uio_name);
    if (uio_index < 0) {
        errno = ENODEV;
        return -1;
    }

    if (read_sysfs_hex((unsigned)uio_index, "size", &map_size) != 0) {
        return -1;
    }

    snprintf(devpath, sizeof(devpath), "/dev/uio%d", uio_index);
    fd = open(devpath, O_RDWR);
    if (fd < 0) {
        return -1;
    }

    base = (volatile uint8_t *)mmap(NULL, (size_t)map_size,
                                    PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                    0);
    if (base == MAP_FAILED) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    out->fd = fd;
    out->uio_index = uio_index;
    out->base = base;
    out->size = (size_t)map_size;
    return 0;
}

void zmpio_uio_close(zmpio_uio_region_t *region)
{
    if (region->base != NULL) {
        munmap((void *)region->base, region->size);
        region->base = NULL;
    }
    if (region->fd >= 0) {
        close(region->fd);
        region->fd = -1;
    }
}

int zmpio_uio_irq_wait(const zmpio_uio_region_t *region)
{
    uint32_t irq_count;
    ssize_t n;

    n = read(region->fd, &irq_count, sizeof(irq_count));
    if (n == (ssize_t)sizeof(irq_count)) {
        return 0;
    }
    if (errno == EINTR) {
        return 1;
    }
    return -1;
}

int zmpio_uio_irq_reenable(const zmpio_uio_region_t *region)
{
    uint32_t one = 1U;

    if (write(region->fd, &one, sizeof(one)) != (ssize_t)sizeof(one)) {
        return -1;
    }
    return 0;
}
