/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "blockdev_rbd.h"

#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <rbd/librbd.h>
#include <rados/librados.h>

#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/io_channel.h"

#include "bdev_module.h"

static TAILQ_HEAD(, blockdev_rbd_pool_info) g_rbd_pools = TAILQ_HEAD_INITIALIZER(g_rbd_pools);
static TAILQ_HEAD(, blockdev_rbd) g_rbds = TAILQ_HEAD_INITIALIZER(g_rbds);
static int blockdev_rbd_count = 0;

typedef void (*rbd_cb_fn_t)(void *);

struct blockdev_rbd_pool_info {
	const char *name;
	TAILQ_ENTRY(blockdev_rbd_pool_info) tailq;
};

enum blockdev_rbd_data_direction {
	BLOCKDEV_RBD_READ = 0,
	BLOCKDEV_RBD_WRITE = 1,
	BLOCKDEV_RBD_FLUSH = 2,
};

struct blockdev_rbd_io {
	enum blockdev_rbd_data_direction direction;
	int status;
	size_t len;
	rbd_completion_t completion;
	rbd_cb_fn_t cb_fn;
	struct blockdev_rbd_io_channel *ch;
};

struct blockdev_rbd {
	struct spdk_bdev disk;
	const char *rbd_name;
	rbd_image_info_t info;
	struct blockdev_rbd_pool_info *pool_info;
	uint64_t size;
	TAILQ_ENTRY(blockdev_rbd) tailq;
};

struct blockdev_rbd_io_channel {
	rados_ioctx_t io_ctx;
	rados_t cluster;
	struct pollfd pfd;
	rbd_image_t image;
	rbd_completion_t *comps;
	uint32_t queue_depth;
	struct blockdev_rbd *disk;
	struct spdk_poller *poller;
};

static int
blockdev_rados_context_init(const char *rbd_pool_name, rados_t *cluster,
			    rados_ioctx_t *io_ctx)
{
	int ret;

	ret = rados_create(cluster, NULL);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to create rados_t struct\n");
		return -1;
	}

	ret = rados_conf_read_file(*cluster, NULL);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to read conf file\n");
		rados_shutdown(*cluster);
		return -1;
	}

	ret = rados_connect(*cluster);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to connect rbd_pool\n");
		rados_shutdown(*cluster);
	}

	ret = rados_ioctx_create(*cluster, rbd_pool_name, io_ctx);

	if (ret < 0) {
		SPDK_ERRLOG("Failed to create ioctx\n");
		rados_shutdown(*cluster);
		return -1;
	}

	return 0;
}

static int
blockdev_rbd_init(const char *rbd_pool_name, const char *rbd_name, rbd_image_info_t *info)
{
	int ret;
	rados_t cluster = NULL;
	rados_ioctx_t io_ctx = NULL;
	rbd_image_t image = NULL;

	ret = blockdev_rados_context_init(rbd_pool_name, &cluster, &io_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to create rados context for rbd_pool=%s\n",
			    rbd_name);
		return -1;
	}

	ret = rbd_open(io_ctx, rbd_name, &image, NULL);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to open specified rbd device\n");
		goto err;
	}
	ret = rbd_stat(image, info, sizeof(*info));
	rbd_close(image);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to stat specified rbd device\n");
		goto err;
	}

	return 0;
err:
	rados_ioctx_destroy(io_ctx);
	rados_shutdown(cluster);
	return -1;
}

static void
blockdev_rbd_exit(rbd_image_t image)
{
	rbd_flush(image);
	rbd_close(image);
}

static void
blockdev_rbd_finish_aiocb(rbd_completion_t cb, void *arg)
{
	/* Doing nothing here */
}

static int
blockdev_rbd_start_aio(rbd_image_t image, struct blockdev_rbd_io *cmd,
		       void *buf, uint64_t offset, size_t len)
{
	int ret;

	ret = rbd_aio_create_completion((void *)cmd, blockdev_rbd_finish_aiocb,
					&cmd->completion);
	if (ret < 0) {
		return -1;
	}

	if (cmd->direction == BLOCKDEV_RBD_READ) {
		ret = rbd_aio_read(image, offset, len,
				   buf, cmd->completion);

	} else if (cmd->direction == BLOCKDEV_RBD_WRITE) {
		ret = rbd_aio_write(image, offset, len,
				    buf, cmd->completion);
	} else if (cmd->direction == BLOCKDEV_RBD_FLUSH) {
		ret = rbd_aio_flush(image, cmd->completion);
	}

	if (ret < 0) {
		rbd_aio_release(cmd->completion);
		return -1;
	}

	return 0;
}

static int blockdev_rbd_library_init(void);
static void blockdev_rbd_library_fini(void);

static int
blockdev_rbd_get_ctx_size(void)
{
	return sizeof(struct blockdev_rbd_io);
}

SPDK_BDEV_MODULE_REGISTER(blockdev_rbd_library_init, blockdev_rbd_library_fini, NULL,
			  blockdev_rbd_get_ctx_size)

static int64_t
blockdev_rbd_read(struct blockdev_rbd *disk, struct spdk_io_channel *ch,
		  struct blockdev_rbd_io *cmd, void *buf, size_t nbytes,
		  uint64_t offset)
{
	struct blockdev_rbd_io_channel *rbdio_ch = spdk_io_channel_get_ctx(ch);

	cmd->ch = rbdio_ch;
	cmd->direction = BLOCKDEV_RBD_READ;
	cmd->len = nbytes;

	return blockdev_rbd_start_aio(rbdio_ch->image, cmd, buf, offset, nbytes);
}

static int64_t
blockdev_rbd_writev(struct blockdev_rbd *disk, struct spdk_io_channel *ch,
		    struct blockdev_rbd_io *cmd, struct iovec *iov,
		    int iovcnt, size_t len, uint64_t offset)
{
	struct blockdev_rbd_io_channel *rbdio_ch = spdk_io_channel_get_ctx(ch);

	if ((iovcnt != 1) || (iov->iov_len != len))
		return -1;

	cmd->ch = (void *)rbdio_ch;
	cmd->direction = BLOCKDEV_RBD_WRITE;

	return blockdev_rbd_start_aio(rbdio_ch->image, cmd, (void *)iov->iov_base, offset, len);
}

static int64_t
blockdev_rbd_flush(struct blockdev_rbd *disk, struct spdk_io_channel *ch,
		   struct blockdev_rbd_io *cmd, uint64_t offset, uint64_t nbytes)
{
	struct blockdev_rbd_io_channel *rbdio_ch = spdk_io_channel_get_ctx(ch);

	cmd->ch = (void *)rbdio_ch;
	cmd->direction = BLOCKDEV_RBD_FLUSH;

	return blockdev_rbd_start_aio(rbdio_ch->image, cmd, NULL, offset, nbytes);
}

static int
blockdev_rbd_destruct(struct spdk_bdev *bdev)
{
	return 0;
}

static void blockdev_rbd_get_rbuf_cb(struct spdk_bdev_io *bdev_io)
{
	int ret;

	ret = blockdev_rbd_read(bdev_io->ctx,
				bdev_io->ch,
				(struct blockdev_rbd_io *)bdev_io->driver_ctx,
				bdev_io->u.read.buf,
				bdev_io->u.read.nbytes,
				bdev_io->u.read.offset);

	if (ret != 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static int _blockdev_rbd_submit_request(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_rbuf(bdev_io, blockdev_rbd_get_rbuf_cb);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		return blockdev_rbd_writev((struct blockdev_rbd *)bdev_io->ctx,
					   bdev_io->ch,
					   (struct blockdev_rbd_io *)bdev_io->driver_ctx,
					   bdev_io->u.write.iovs,
					   bdev_io->u.write.iovcnt,
					   bdev_io->u.write.len,
					   bdev_io->u.write.offset);
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return blockdev_rbd_flush((struct blockdev_rbd *)bdev_io->ctx,
					  bdev_io->ch,
					  (struct blockdev_rbd_io *)bdev_io->driver_ctx,
					  bdev_io->u.flush.offset,
					  bdev_io->u.flush.length);
	default:
		return -1;
	}
	return 0;
}

static void blockdev_rbd_submit_request(struct spdk_bdev_io *bdev_io)
{
	if (_blockdev_rbd_submit_request(bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
blockdev_rbd_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return true;

	default:
		return false;
	}
}

static void
blockdev_rbd_io_poll(void *arg)
{
	struct blockdev_rbd_io_channel *ch = arg;
	struct blockdev_rbd_io *req;
	int i, io_status, status, rc;

	rc = poll(&ch->pfd, 1, 0);

	/* check the return value of poll since we have only one fd for each channel */
	if (rc != 1) {
		return;
	}

	rc = rbd_poll_io_events(ch->image, ch->comps, ch->queue_depth);
	for (i = 0; i < rc; i++) {
		req = (struct blockdev_rbd_io *)rbd_aio_get_arg(ch->comps[i]);
		io_status = rbd_aio_get_return_value(ch->comps[i]);
		if (req->direction == BLOCKDEV_RBD_READ) {
			if ((int)req->len == io_status) {
				req->status = 0;
			} else {
				req->status = -1;
			}
		} else {
			/* For others, 0 means success */
			if (!io_status) {
				req->status = 0;
			} else {
				req->status = -1;
			}
		}
		status = req->status == 0 ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(req), status);
		rbd_aio_release(req->completion);
	}
}

static void
blockdev_rbd_free_channel(struct blockdev_rbd_io_channel *ch)
{
	if (!ch) {
		return;
	}

	if (ch->image) {
		blockdev_rbd_exit(ch->image);
	}

	if (ch->io_ctx) {
		rados_ioctx_destroy(ch->io_ctx);
	}

	if (ch->cluster) {
		rados_shutdown(ch->cluster);
	}

	if (ch->comps) {
		free(ch->comps);
	}

	if (ch->pfd.fd >= 0) {
		close(ch->pfd.fd);
	}
}

static int
blockdev_rbd_create_cb(void *io_device, uint32_t priority,
		       void *ctx_buf, void *unique_ctx)
{
	struct blockdev_rbd_io_channel *ch = ctx_buf;
	int ret;
	struct blockdev_rbd_pool_info *pool_info;

	ch->disk = (struct blockdev_rbd *)io_device;
	pool_info = ch->disk->pool_info;
	ch->image = NULL;
	ch->io_ctx = NULL;
	ch->pfd.fd = -1;

	ret = blockdev_rados_context_init(pool_info->name, &ch->cluster, &ch->io_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to create rados context for rbd_pool=%s\n",
			    pool_info->name);
		goto err;
	}

	ret = rbd_open(ch->io_ctx, ch->disk->rbd_name, &ch->image, NULL);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to open specified rbd device\n");
		goto err;
	}

	ch->pfd.fd = eventfd(0, EFD_NONBLOCK);
	if (ch->pfd.fd < 0) {
		SPDK_ERRLOG("Failed to get eventfd\n");
		goto err;
	}

	ch->pfd.events = POLLIN;
	ret = rbd_set_image_notification(ch->image, ch->pfd.fd, EVENT_TYPE_EVENTFD);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to set rbd image notification\n");
		goto err;
	}

	ch->queue_depth = 128;
	ch->comps = calloc(sizeof(rbd_completion_t), ch->queue_depth);
	if (!ch->comps) {
		SPDK_ERRLOG("Failed to allocate rbd completion array\n");
		goto err;
	}

	spdk_poller_register(&ch->poller, blockdev_rbd_io_poll, ch,
			     spdk_app_get_current_core(), NULL, 0);

	return 0;

err:
	blockdev_rbd_free_channel(ch);
	return -1;
}

static void
blockdev_rbd_destroy_cb(void *io_device, void *ctx_buf)
{
	struct blockdev_rbd_io_channel *io_channel = ctx_buf;

	blockdev_rbd_free_channel(io_channel);

	spdk_poller_unregister(&io_channel->poller, NULL);
}

static struct spdk_io_channel *
blockdev_rbd_get_io_channel(struct spdk_bdev *bdev, uint32_t priority)
{
	struct blockdev_rbd *rbd_bdev = (struct blockdev_rbd *)bdev;

	return spdk_get_io_channel(rbd_bdev, priority, false, NULL);
}

static const struct spdk_bdev_fn_table rbd_fn_table = {
	.destruct		= blockdev_rbd_destruct,
	.submit_request		= blockdev_rbd_submit_request,
	.io_type_supported	= blockdev_rbd_io_type_supported,
	.get_io_channel		= blockdev_rbd_get_io_channel,
};

static void
blockdev_rbd_library_fini(void)
{
	struct blockdev_rbd_pool_info *pool_info;
	struct blockdev_rbd *rbd;

	while (!TAILQ_EMPTY(&g_rbds)) {
		rbd = TAILQ_FIRST(&g_rbds);
		TAILQ_REMOVE(&g_rbds, rbd, tailq);
		free(rbd);
	}
	while (!TAILQ_EMPTY(&g_rbd_pools)) {
		pool_info = TAILQ_FIRST(&g_rbd_pools);
		TAILQ_REMOVE(&g_rbd_pools, pool_info, tailq);
		free(pool_info);
	}
}

static struct blockdev_rbd_pool_info *
blockdev_rbd_pool_info_init(const char *rbd_pool_name)
{
	struct blockdev_rbd_pool_info *pool_info;

	TAILQ_FOREACH(pool_info, &g_rbd_pools, tailq) {
		if (!strcmp(pool_info->name, rbd_pool_name)) {
			return pool_info;
		}
	}

	pool_info = calloc(1, sizeof(struct blockdev_rbd_pool_info));
	if (!pool_info) {
		SPDK_ERRLOG("Failed to allocate blockdev_rbd_pool_info struct\n");
		return NULL;
	}

	pool_info->name = rbd_pool_name;
	TAILQ_INSERT_TAIL(&g_rbd_pools, pool_info, tailq);

	return pool_info;
}

int
spdk_bdev_rbd_create(const char *pool_name, const char *rbd_name, uint32_t block_size)
{
	struct blockdev_rbd_pool_info *pool_info;
	struct blockdev_rbd *rbd;
	int ret;

	pool_info = blockdev_rbd_pool_info_init(pool_name);
	if (pool_info == NULL) {
		SPDK_ERRLOG("failed to create blockdev_rbd_pool_info\n");
		return -1;
	}

	rbd = calloc(1, sizeof(struct blockdev_rbd));
	if (rbd == NULL) {
		SPDK_ERRLOG("Failed to allocate blockdev_rbd struct\n");
		free(pool_info);
		return -1;
	}

	rbd->pool_info = pool_info;
	rbd->rbd_name = rbd_name;
	ret = blockdev_rbd_init(pool_info->name, rbd_name, &rbd->info);
	if (ret < 0) {
		free(pool_info);
		free(rbd);
		SPDK_ERRLOG("Failed to init rbd device\n");
		return -1;
	}

	snprintf(rbd->disk.name, SPDK_BDEV_MAX_NAME_LENGTH, "Ceph%d",
		 blockdev_rbd_count);
	snprintf(rbd->disk.product_name, SPDK_BDEV_MAX_PRODUCT_NAME_LENGTH, "Ceph rbd");
	blockdev_rbd_count++;

	rbd->disk.write_cache = 0;
	rbd->disk.blocklen = block_size;
	rbd->disk.blockcnt = rbd->info.size / rbd->disk.blocklen;
	rbd->disk.ctxt = rbd;
	rbd->disk.fn_table = &rbd_fn_table;

	SPDK_NOTICELOG("Add %s rbd disk to lun\n", rbd->disk.name);
	TAILQ_INSERT_TAIL(&g_rbds, rbd, tailq);

	spdk_io_device_register(&rbd->disk, blockdev_rbd_create_cb,
				blockdev_rbd_destroy_cb,
				sizeof(struct blockdev_rbd_io_channel));
	spdk_bdev_register(&rbd->disk);
	return 0;
}

static int
blockdev_rbd_library_init(void)
{
	int i, ret;
	const char *val;
	const char *pool_name;
	const char *rbd_name;
	uint32_t block_size;

	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "Ceph");

	if (sp == NULL) {
		/*
		 * Ceph section not found.  Do not initialize any rbd LUNS.
		 */
		return 0;
	}

	/* Init rbd block devices */
	for (i = 0; ; i++) {
		val = spdk_conf_section_get_nval(sp, "Ceph", i);
		if (val == NULL)
			break;

		/* get the Rbd_pool name */
		pool_name = spdk_conf_section_get_nmval(sp, "Ceph", i, 0);
		if (pool_name == NULL) {
			SPDK_ERRLOG("Ceph%d: rbd pool name needs to be provided\n", i);
			goto cleanup;
		}

		rbd_name = spdk_conf_section_get_nmval(sp, "Ceph", i, 1);
		if (rbd_name == NULL) {
			SPDK_ERRLOG("Ceph%d: format error\n", i);
			goto cleanup;
		}

		val = spdk_conf_section_get_nmval(sp, "Ceph", i, 2);

		if (val == NULL) {
			block_size = 512; /* default value */
		} else {
			block_size = (int)strtol(val, NULL, 10);
			if (block_size & 0x1ff) {
				SPDK_ERRLOG("current block_size = %d, it should be multiple of 512\n",
					    block_size);
				goto cleanup;
			}
		}

		ret = spdk_bdev_rbd_create(pool_name, rbd_name, block_size);
		if (ret) {
			goto cleanup;
		}
	}

	return 0;
cleanup:
	blockdev_rbd_library_fini();
	return -1;
}
