#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include "procedure.h"
#include "ata.h"
#include "scsi.h"

enum Api {
    Api_eAta,
    Api_ePosix,
};

struct read_priv {
    const char *api_str;
    int64_t start_lba;
    enum Api api;
    int64_t end_lba;
    int64_t lba_to_process;
    int fd;
    void *buf;
    AtaCommand ata_command;
    ScsiCommand scsi_command;
    int old_readahead;
    uint64_t current_lba;
};
typedef struct read_priv ReadPriv;

#define SECTORS_AT_ONCE 256
#define BLK_SIZE (SECTORS_AT_ONCE * 512) // FIXME hardcode
static int Open(DC_ProcedureCtx *ctx) {
    int r;
    int open_flags;
    ReadPriv *priv = ctx->priv;

    // Setting context
    if (!strcmp(priv->api_str, "ata"))
        priv->api = Api_eAta;
    else if (!strcmp(priv->api_str, "posix"))
        priv->api = Api_ePosix;
    else
        return 1;
    ctx->blk_size = BLK_SIZE;
    priv->current_lba = priv->start_lba;
    priv->end_lba = ctx->dev->capacity / 512;
    priv->lba_to_process = priv->end_lba - priv->start_lba;
    if (priv->lba_to_process <= 0)
        return 1;
    ctx->progress.den = priv->lba_to_process / SECTORS_AT_ONCE;
    if (priv->lba_to_process % SECTORS_AT_ONCE)
        ctx->progress.den++;

    if (priv->api == Api_eAta) {
        open_flags = O_RDWR;
    } else {
        r = posix_memalign(&priv->buf, sysconf(_SC_PAGESIZE), ctx->blk_size);
        if (r)
            return 1;

        open_flags = O_RDONLY | O_DIRECT | O_LARGEFILE | O_NOATIME;
    }

    priv->fd = open(ctx->dev->dev_path, open_flags);
    if (priv->fd == -1) {
        dc_log(DC_LOG_FATAL, "open %s fail\n", ctx->dev->dev_path);
        return 1;
    }

    lseek(priv->fd, 512 * priv->start_lba, SEEK_SET);
    r = ioctl(priv->fd, BLKFLSBUF, NULL);
    if (r == -1)
      dc_log(DC_LOG_WARNING, "Flushing block device buffers failed\n");
    r = ioctl(priv->fd, BLKRAGET, &priv->old_readahead);
    if (r == -1)
      dc_log(DC_LOG_WARNING, "Getting block device readahead setting failed\n");
    r = ioctl(priv->fd, BLKRASET, 0);
    if (r == -1)
      dc_log(DC_LOG_WARNING, "Disabling block device readahead setting failed\n");

    return 0;
}

static int Perform(DC_ProcedureCtx *ctx) {
    ssize_t read_ret;
    int ioctl_ret;
    int ret = 0;
    ReadPriv *priv = ctx->priv;
    struct timespec pre, post;
    size_t sectors_to_read = (priv->lba_to_process < SECTORS_AT_ONCE) ? priv->lba_to_process : SECTORS_AT_ONCE;
    int r;

    // Updating context
    ctx->report.lba = priv->current_lba;
    ctx->report.blk_status = DC_BlockStatus_eOk;

    // Preparing to act
    if (priv->api == Api_eAta) {
        memset(&priv->ata_command, 0, sizeof(priv->ata_command));
        memset(&priv->scsi_command, 0, sizeof(priv->scsi_command));
        prepare_ata_command(&priv->ata_command, WIN_VERIFY_EXT /* 42h */, priv->current_lba, sectors_to_read);
        prepare_scsi_command_from_ata(&priv->scsi_command, &priv->ata_command);
    }

    // Timing
    r = clock_gettime(DC_BEST_CLOCK, &pre);
    assert(!r);

    // Acting
    if (priv->api == Api_eAta)
        ioctl_ret = ioctl(priv->fd, SG_IO, &priv->scsi_command);
    else
        read_ret = read(priv->fd, priv->buf, sectors_to_read * 512);

    // Timing
    r = clock_gettime(DC_BEST_CLOCK, &post);
    assert(!r);

    // Error handling
    if (priv->api == Api_eAta) {
        ScsiAtaReturnDescriptor scsi_ata_return;
        fill_scsi_ata_return_descriptor(&scsi_ata_return, &priv->scsi_command);
        int sense_key = get_sense_key_from_sense_buffer(priv->scsi_command.sense_buf);
#if 0
        fprintf(stderr, "scsi status: %hhu, msg status %hhu, host status %hu, driver status %hu, duration %u, auxinfo %u\n",
            scsi_command.io_hdr.status,
            scsi_command.io_hdr.msg_status,
            scsi_command.io_hdr.host_status,
            scsi_command.io_hdr.driver_status,
            scsi_command.io_hdr.duration,
            scsi_command.io_hdr.info);
        fprintf(stderr, "sense buffer, in hex: ");
        int i;
        for (i = 0; i < sizeof(scsi_command.sense_buf); i++)
          fprintf(stderr, "%02hhx", scsi_command.sense_buf[i]);
        fprintf(stderr, "\n");
        fprintf(stderr, "sense key is %d", sense_key);
#endif

        // Error handling
        // Updating context
        if (ioctl_ret) {
            ctx->report.blk_status = DC_BlockStatus_eError;
            ret = 1;
        } else if (priv->scsi_command.io_hdr.duration >= priv->scsi_command.io_hdr.timeout) {
            ctx->report.blk_status = DC_BlockStatus_eTimeout;
        } else if (scsi_ata_return.status.bits.err) {
            if (scsi_ata_return.error.bits.unc)
                ctx->report.blk_status = DC_BlockStatus_eUnc;
            else if (scsi_ata_return.error.bits.idnf)
                ctx->report.blk_status = DC_BlockStatus_eIdnf;
            else if (scsi_ata_return.error.bits.abrt)
                ctx->report.blk_status = DC_BlockStatus_eAbrt;
            else
                ctx->report.blk_status = DC_BlockStatus_eError;
        } else if (scsi_ata_return.status.bits.df) {
            ctx->report.blk_status = DC_BlockStatus_eError;
        } else if (sense_key) {
            if (sense_key == 0x0b)
                ctx->report.blk_status = DC_BlockStatus_eAbrt;
            else
                ctx->report.blk_status = DC_BlockStatus_eError;
        }
    } else {
        if ((int)read_ret != (int)sectors_to_read * 512) {
            // Position of fd is undefined. Set fd position to read next block
            lseek(priv->fd, 512 * priv->current_lba, SEEK_SET);

            // Updating context
            ctx->report.blk_status = DC_BlockStatus_eError;
        }
    }

    // Updating context
    ctx->progress.num++;
    priv->lba_to_process -= sectors_to_read;
    priv->current_lba += sectors_to_read;
    ctx->report.blk_access_time = (post.tv_sec - pre.tv_sec) * 1000000 +
        (post.tv_nsec - pre.tv_nsec) / 1000;

    return ret;
}

static void Close(DC_ProcedureCtx *ctx) {
    ReadPriv *priv = ctx->priv;
    int r = ioctl(priv->fd, BLKRASET, priv->old_readahead);
    if (r == -1)
      dc_log(DC_LOG_WARNING, "Restoring block device readahead setting failed\n");
    free(priv->buf);
    close(priv->fd);
}

static DC_ProcedureOption options[] = {
    { "api", "select operation API: \"posix\" for POSIX read(), \"ata\" for ATA \"READ VERIFY EXT\" command", offsetof(ReadPriv, api_str), DC_ProcedureOptionType_eString, { .str = "ata" } },  // TODO "auto"; TODO flags syntax
    { "start_lba", "set LBA address to begin from", offsetof(ReadPriv, start_lba), DC_ProcedureOptionType_eInt64, { .i64 = 0 } },
    { NULL }
};

DC_Procedure read_test = {
    .name = "read_test",
    .long_name = "Test device with reading",
    .open = Open,
    .perform = Perform,
    .close = Close,
    .priv_data_size = sizeof(ReadPriv),
    .options = options,
};

