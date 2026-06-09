// SPDX-License-Identifier: GPL-2.0-only
/**
 * Copyright (c) 2025 Syswonder
 *
 * Syswonder Website:
 *      https://www.syswonder.org
 *
 * Authors:
 *      Guowei Li <2401213322@stu.pku.edu.cn>
 */
#include "virtio_blk.h"
#include "log.h"
#include "virtio.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/stat.h>

/*
 * Threading model
 * ---------------
 * Two threads touch a virtio-blk device:
 *
 *   main thread   (epoll loop in virtio.c)
 *     - calls notify_handler when the guest kicks the virtqueue.
 *     - calls virtio_blk_close on shutdown.
 *     - does NOT touch the virtqueue or BlkDev (except mtx/cond/close).
 *
 *   worker thread (blkproc_thread, one per blk device)
 *     - owns the virtqueue exclusively: drains avail_ring, performs disk I/O,
 *       updates used_ring, and injects IRQs back to the guest.
 *     - only it reads/writes vq->last_avail_idx and vq->last_used_idx.
 *
 * Synchronization between the two threads
 * ---------------------------------------
 * BlkDev.mtx + BlkDev.cond form a standard producer-consumer pair:
 *   - notify_handler locks mtx, signals cond, unlocks.
 *   - the worker waits on cond (with mtx), then drains the virtqueue directly.
 *
 * The virtqueue (avail_ring, desc_table) is single-threaded — the main thread
 * never accesses it. This avoids the intermediate procq and the extra locking
 * the old design required.
 *
 * Cross-CPU shared memory (guest <-> worker)
 * ------------------------------------------
 * avail_ring->idx is written by the guest and read by the worker, hence the
 * ACQUIRE load in vq_is_empty(). used_ring is written by the worker and read
 * by the guest, hence write_barrier() in update_used_ring().
 */

/*
 * Complete a single block request: set the status byte, push the used-ring
 * entry, and free the request.
 */
static void complete_block_operation(struct blkp_req *req, VirtQueue *vq,
                                     int err, ssize_t written_len) {
    uint8_t *vstatus = (uint8_t *)(req->iov[req->iovcnt - 1].iov_base);
    if (err == EOPNOTSUPP)
        *vstatus = VIRTIO_BLK_S_UNSUPP;
    else if (err != 0)
        *vstatus = VIRTIO_BLK_S_IOERR;
    else
        *vstatus = VIRTIO_BLK_S_OK;
    if (err != 0) {
        log_error("virt blk err, num is %d", err);
    }
    update_used_ring(vq, req->idx, written_len + 1);
    free(req->iov);
    free(req);
}

/*
 * Dispatch a parsed block request to the appropriate I/O operation.
 * iov layout: [header][data buffers ...][status byte]
 */
static void blkproc(BlkDev *dev, struct blkp_req *req, VirtQueue *vq) {
    struct iovec *iov = req->iov;
    int n = req->iovcnt, err = 0;
    ssize_t len, written_len = 0;

    switch (req->type) {
    case VIRTIO_BLK_T_IN:
        len = preadv(dev->img_fd, &iov[1], n - 2, req->offset);
        log_debug("preadv, len is %d, offset is %d", len, req->offset);
        if (len < 0) {
            log_error("pread failed");
            err = errno;
        } else {
            written_len = len;
        }
        break;
    case VIRTIO_BLK_T_OUT:
        len = pwritev(dev->img_fd, &iov[1], n - 2, req->offset);
        log_debug("pwritev, len is %d, offset is %d", len, req->offset);
        if (len < 0) {
            log_error("pwrite failed");
            err = errno;
        }
        break;
    case VIRTIO_BLK_T_GET_ID: {
        char s[20] = "hvisor-virblk";
        strncpy(iov[1].iov_base, s, MIN(sizeof(s), iov[1].iov_len));
        written_len = MIN(sizeof(s), iov[1].iov_len);
        break;
    }
    default:
        log_fatal("Operation is not supported");
        err = EOPNOTSUPP;
        break;
    }
    complete_block_operation(req, vq, err, written_len);
}

static struct blkp_req *virtq_blk_handle_one_request(VirtQueue *vq);

/*
 * Worker thread entry point — one per virtio-blk device.
 *
 * The worker is the sole owner of the virtqueue:
 *   1. Wait on cond until notify_handler signals or close is set.
 *   2. Drain the avail_ring in a disable-notify / process / enable-notify
 *      loop to suppress redundant guest notifications while we're busy.
 *   3. Inject a single IRQ after each batch to tell the guest about
 *      completed requests.
 *   4. Loop back to step 1.
 *
 * virtio_inject_irq() is a no-op if no used-ring entries were added, so
 * calling it unconditionally after each batch is safe.
 */
static void *blkproc_thread(void *arg) {
    VirtIODevice *vdev = arg;
    BlkDev *dev = vdev->dev;
    VirtQueue *vq = vdev->vqs;
    struct blkp_req *breq;

    for (;;) {
        // Hold mtx to check the close flag and wait on cond.
        pthread_mutex_lock(&dev->mtx);
        while (vq_is_empty(vq) && !dev->close)
            pthread_cond_wait(&dev->cond, &dev->mtx);
        if (dev->close) {
            pthread_mutex_unlock(&dev->mtx);
            break;
        }
        pthread_mutex_unlock(&dev->mtx);

        // Drain all pending requests. The double-checked loop follows the
        // standard virtio pattern: disable-notify, process until empty,
        // enable-notify, then re-check in case the guest added buffers
        // while notifications were suppressed.
        while (!vq_is_empty(vq)) {
            virtqueue_disable_notify(vq);
            while (!vq_is_empty(vq)) {
                breq = virtq_blk_handle_one_request(vq);
                if (breq)
                    blkproc(dev, breq, vq);
            }
            virtqueue_enable_notify(vq);
        }

        // Tell the guest that used-ring entries are available.
        virtio_inject_irq(vq);
    }

    // close flag was set; exit the thread.
    pthread_exit(NULL);
    return NULL;
}

/*
 * Allocate and zero-initialize a BlkDev. The worker thread is NOT started
 * here — start_blk_worker() is called after virtio_blk_init() succeeds.
 */
BlkDev *init_blk_dev(VirtIODevice *vdev) {
    BlkDev *dev = calloc(1, sizeof(BlkDev));
    vdev->dev = dev;
    dev->config.capacity = -1;
    dev->config.size_max = -1;
    dev->config.seg_max = BLK_SEG_MAX;
    dev->img_fd = -1;
    dev->close = 0;
    pthread_mutex_init(&dev->mtx, NULL);
    pthread_cond_init(&dev->cond, NULL);
    return dev;
}

/*
 * Start the per-device I/O worker thread. Must be called after the virtqueue
 * is allocated (init_virtio_queue) and the backing image is opened
 * (virtio_blk_init), but before the guest activates the device.
 */
void start_blk_worker(VirtIODevice *vdev) {
    BlkDev *dev = vdev->dev;
    pthread_create(&dev->tid, NULL, blkproc_thread, vdev);
}

/*
 * Open the backing image file and populate the virtio-blk config.
 * The worker has not started yet, so no concurrency concerns here.
 */
int virtio_blk_init(VirtIODevice *vdev, const char *img_path) {
    int img_fd = open(img_path, O_RDWR);
    BlkDev *dev = vdev->dev;
    struct stat st;
    uint64_t blk_size;
    if (img_fd == -1) {
        log_error("cannot open %s, Error code is %d", img_path, errno);
        return -1;
    }
    if (fstat(img_fd, &st) == -1) {
        log_error("cannot stat %s, Error code is %d", img_path, errno);
        close(img_fd);
        return -1;
    }
    blk_size = st.st_size / 512; // 512 bytes per block, as per virtio-blk spec
    if (blk_size == 0) {
        // st_size may be 0 for real block devices; try BLKGETSIZE64
        uint64_t size64;
        if (ioctl(img_fd, BLKGETSIZE64, &size64) == 0)
            blk_size = size64 / 512;
    }
    dev->config.capacity = blk_size;
    dev->config.size_max = blk_size;
    dev->img_fd = img_fd;
    vdev->virtio_close = virtio_blk_close;
    log_info("debug: virtio_blk_init: %s, size is %lld", img_path,
             dev->config.capacity);
    return 0;
}

/*
 * Parse a single descriptor chain from the virtqueue into a blkp_req.
 *
 * Returns the request on success, or NULL on a malformed descriptor chain
 * (in which case no used-ring entry is written — the guest will time out
 * and reset the device).
 */
static struct blkp_req *virtq_blk_handle_one_request(VirtQueue *vq) {
    log_debug("virtq_blk_handle_one_request enter");
    struct blkp_req *breq;
    struct iovec *iov = NULL;
    uint16_t *flags;
    int i, n;
    BlkReqHead *hdr;
    breq = malloc(sizeof(struct blkp_req));
    n = process_descriptor_chain(vq, &breq->idx, &iov, &flags, 0, true);
    breq->iov = iov;

    // A valid blk request has at least a header + status byte
    if (n < 2 || n > BLK_SEG_MAX + 2) {
        log_error("iov's num is wrong, n is %d", n);
        goto err_out;
    }

    // Header must be device-readable
    if ((flags[0] & VRING_DESC_F_WRITE) != 0) {
        log_error("virt queue's desc chain header should not be writable!");
        goto err_out;
    }

    if (iov[0].iov_len != sizeof(BlkReqHead)) {
        log_error("the size of blk header is %d, it should be %d!",
                  iov[0].iov_len, sizeof(BlkReqHead));
        goto err_out;
    }

    // Status byte must be device-writable and exactly 1 byte
    if (iov[n - 1].iov_len != 1 || ((flags[n - 1] & VRING_DESC_F_WRITE) == 0)) {
        log_error(
            "status iov is invalid!, status len is %d, flag is %d, n is %d",
            iov[n - 1].iov_len, flags[n - 1], n);
        goto err_out;
    }

    hdr = (BlkReqHead *)(iov[0].iov_base);
    uint64_t offset = hdr->sector * SECTOR_BSIZE;
    breq->type = hdr->type;
    breq->iovcnt = n;
    breq->offset = offset;

    // Data buffer flags must be consistent with the request type:
    //   VIRTIO_BLK_T_OUT -> device-readable (guest wrote data)
    //   VIRTIO_BLK_T_IN  -> device-writable (guest wants data back)
    for (i = 1; i < n - 1; i++)
        if (((flags[i] & VRING_DESC_F_WRITE) == 0) !=
            (breq->type == VIRTIO_BLK_T_OUT)) {
            log_error("flag is conflict with operation");
            goto err_out;
        }

    free(flags);
    return breq;

err_out:
    free(flags);
    free(iov);
    free(breq);
    return NULL;
}

/*
 * Called by the main thread when the guest writes to the queue_notify MMIO
 * register. Wakes up the worker thread so it can drain the virtqueue.
 */
int virtio_blk_notify_handler(VirtIODevice *vdev, VirtQueue *vq) {
    BlkDev *dev = vdev->dev;
    (void)vq;

    // Wake up the worker thread. mtx pairs with the worker's cond_wait.
    pthread_mutex_lock(&dev->mtx);
    pthread_cond_signal(&dev->cond);
    pthread_mutex_unlock(&dev->mtx);
    return 0;
}

/*
 * Shut down the blk device: signal close, wait for the worker to exit,
 * then release all resources.
 */
void virtio_blk_close(VirtIODevice *vdev) {
    BlkDev *dev = vdev->dev;

    // Signal the worker to exit, then wait for it to finish.
    pthread_mutex_lock(&dev->mtx);
    dev->close = 1;
    pthread_cond_signal(&dev->cond);
    pthread_mutex_unlock(&dev->mtx);

    // Wait for the worker thread to exit.
    if (dev->tid)
        pthread_join(dev->tid, NULL);

    // Worker is done — safe to tear down synchronization objects.
    pthread_mutex_destroy(&dev->mtx);
    pthread_cond_destroy(&dev->cond);

    // Release all resources.
    close(dev->img_fd);
    free(dev);
    free(vdev->vqs);
    free(vdev);
}
