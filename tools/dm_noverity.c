/*
 * dm_noverity: Replace dm-verity target with linear target at runtime.
 * After running, /system can be remounted rw.
 *
 * Usage: dm_noverity [device_name]  (default: "system")
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/dm-ioctl.h>

#define BUF_SIZE 16384

static void init_io(struct dm_ioctl *io, const char *name, int buf_size) {
    memset(io, 0, buf_size);
    io->version[0] = 4;
    io->version[1] = 0;
    io->version[2] = 0;
    io->data_size = buf_size;
    io->data_start = sizeof(struct dm_ioctl);
    if (name) strncpy(io->name, name, DM_NAME_LEN - 1);
}

int main(int argc, char *argv[]) {
    const char *dm_name = argc > 1 ? argv[1] : "system";
    char buf[BUF_SIZE];
    struct dm_ioctl *io = (struct dm_ioctl *)buf;

    int fd = open("/dev/device-mapper", O_RDWR);
    if (fd < 0) { perror("open /dev/device-mapper"); return 1; }

    /* Step 1: Get current table info */
    init_io(io, dm_name, BUF_SIZE);
    if (ioctl(fd, DM_TABLE_STATUS, io) < 0) {
        perror("DM_TABLE_STATUS");
        return 1;
    }

    unsigned long long dev_sectors = 0;
    struct dm_target_spec *tgt = (struct dm_target_spec *)(buf + io->data_start);
    char *params = (char *)tgt + sizeof(struct dm_target_spec);
    dev_sectors = tgt->length;
    printf("Current: type='%s' sectors=%llu\n", tgt->target_type, dev_sectors);

    if (strcmp(tgt->target_type, "verity") != 0) {
        printf("Not a verity device, nothing to do.\n");
        close(fd);
        return 0;
    }

    /* Step 2: Get the underlying block device major:minor
       by checking /sys/block/dm-0/slaves/ */
    unsigned int major = 0, minor = 0;
    /* Read from DM_DEV_STATUS to get dev number, or stat the slave device */
    {
        char slave_path[256];
        snprintf(slave_path, sizeof(slave_path),
                 "/sys/block/dm-0/slaves/mmcblk0p14/dev");
        FILE *f = fopen(slave_path, "r");
        if (f) {
            fscanf(f, "%u:%u", &major, &minor);
            fclose(f);
        } else {
            /* Fallback: stat the device */
            struct stat st;
            if (stat("/dev/block/mmcblk0p14", &st) == 0) {
                major = (st.st_rdev >> 8) & 0xfff;
                minor = st.st_rdev & 0xff;
            } else {
                perror("Can't find underlying device");
                return 1;
            }
        }
    }
    printf("Underlying device: %u:%u\n", major, minor);

    /* Step 3: Suspend the dm device */
    init_io(io, dm_name, BUF_SIZE);
    io->flags = DM_SUSPEND_FLAG;
    if (ioctl(fd, DM_DEV_SUSPEND, io) < 0) {
        perror("DM_DEV_SUSPEND (suspend)");
        return 1;
    }
    printf("Suspended.\n");

    /* Step 4: Clear current table */
    init_io(io, dm_name, BUF_SIZE);
    if (ioctl(fd, DM_TABLE_CLEAR, io) < 0) {
        perror("DM_TABLE_CLEAR");
        goto resume;
    }
    printf("Table cleared.\n");

    /* Step 5: Load linear table */
    init_io(io, dm_name, BUF_SIZE);

    tgt = (struct dm_target_spec *)(buf + io->data_start);
    tgt->sector_start = 0;
    tgt->length = dev_sectors;
    strncpy(tgt->target_type, "linear", DM_MAX_TYPE_NAME - 1);
    tgt->next = 0;

    params = (char *)tgt + sizeof(struct dm_target_spec);
    int plen = snprintf(params, BUF_SIZE - io->data_start - sizeof(struct dm_target_spec),
                        "%u:%u 0", major, minor);
    /* Align next to 8 bytes */
    tgt->next = sizeof(struct dm_target_spec) + ((plen + 1 + 7) & ~7);
    io->data_size = io->data_start + tgt->next;
    io->target_count = 1;

    if (ioctl(fd, DM_TABLE_LOAD, io) < 0) {
        perror("DM_TABLE_LOAD (linear)");
        goto resume;
    }
    printf("Linear table loaded: 0 %llu linear %u:%u 0\n", dev_sectors, major, minor);

    /* Step 6: Resume */
resume:
    init_io(io, dm_name, BUF_SIZE);
    io->flags = 0; /* clear DM_SUSPEND_FLAG = resume */
    if (ioctl(fd, DM_DEV_SUSPEND, io) < 0) {
        perror("DM_DEV_SUSPEND (resume)");
        return 1;
    }
    printf("Resumed.\n");

    /* Verify */
    init_io(io, dm_name, BUF_SIZE);
    if (ioctl(fd, DM_TABLE_STATUS, io) == 0) {
        tgt = (struct dm_target_spec *)(buf + io->data_start);
        printf("Verify: type='%s'\n", tgt->target_type);
    }

    close(fd);
    printf("Done. Try: mount -o remount,rw /system\n");
    return 0;
}
