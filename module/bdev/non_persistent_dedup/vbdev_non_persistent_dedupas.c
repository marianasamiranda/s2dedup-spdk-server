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

/*
 * This is a simple example of a virtual block device module that passes IO
 * down to a bdev (or bdevs) that its configured to attach to.
 */

#include "spdk/stdinc.h"

#include "vbdev_non_persistent_dedupas.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "spdk_internal/log.h"

#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>


static int vbdev_dedupas_init(void);
static void vbdev_dedupas_get_spdk_running_config(FILE *fp);
static int vbdev_dedupas_get_ctx_size(void);
static void vbdev_dedupas_examine(struct spdk_bdev *bdev);
static void vbdev_dedupas_finish(void);
static int vbdev_dedupas_config_json(struct spdk_json_write_ctx *w);

static struct spdk_bdev_module dedupas_if = {
        .name = "dedupas",
        .module_init = vbdev_dedupas_init,
        .config_text = vbdev_dedupas_get_spdk_running_config,
        .get_ctx_size = vbdev_dedupas_get_ctx_size,
        .examine_config = vbdev_dedupas_examine,
        .module_fini = vbdev_dedupas_finish,
        .config_json = vbdev_dedupas_config_json
};

SPDK_BDEV_MODULE_REGISTER(dedupas, &dedupas_if)

/* List of bdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
struct bdev_names {
        char                        *vbdev_name;
        char                        *bdev_name;
        TAILQ_ENTRY(bdev_names)        link;
};
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

/* List of virtual bdevs and associated info for each. */
struct vbdev_dedupas {
        struct spdk_bdev                *base_bdev; /* the thing we're attaching to */
        struct spdk_bdev_desc                *base_desc; /* its descriptor we get from open */
        struct spdk_bdev                pt_bdev;    /* the PT virtual bdev */
        TAILQ_ENTRY(vbdev_dedupas)        link;
        struct spdk_thread                 *dedup_thread; /* thread for deduplication */ 
};
static TAILQ_HEAD(, vbdev_dedupas) g_pt_nodes = TAILQ_HEAD_INITIALIZER(g_pt_nodes);

/* The pt vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 * If this vbdev needed to implement a poller or a queue for IO, this is where those things
 * would be defined. This dedupas bdev doesn't actually need to allocate a channel, it could
 * simply pass back the channel of the bdev underneath it but for example purposes we will
 * present its own to the upper layers.
 */
struct pt_io_channel {
        struct spdk_io_channel        *base_ch; /* IO channel of base device */
};

/* This module contains the elements necessary for the completion callback functions.
 */
struct dedupas_bdev_io {
        uint8_t test;

        /* bdev related */
        struct spdk_io_channel *ch;

        struct vbdev_dedupas *dedupas_bdev;

        struct spdk_thread *orig_thread;

        /* for bdev_io_wait */
        struct spdk_bdev_io_wait_entry bdev_io_wait;
};


/* Contains the elements necessary to ensure a correct completion.*/
struct complete_args{
        struct spdk_bdev_io *orig_io;
        
        /* number of blocks received */
        int counter;

        /* number of total blocks */
        int goal;
};


static void
vbdev_dedupas_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);


/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
        struct vbdev_dedupas *pt_node  = io_device;

        /* Done with this pt_node. */
        free(pt_node->pt_bdev.name);
        free(pt_node);
}

void reset_dedup_engine(){
        reset_dedup();
}


/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
static int
vbdev_dedupas_destruct(void *ctx)
{
        struct vbdev_dedupas *pt_node = (struct vbdev_dedupas *)ctx;

        /* It is important to follow this exact sequence of steps for destroying
         * a vbdev...
         */

        TAILQ_REMOVE(&g_pt_nodes, pt_node, link);

        /* Unclaim the underlying bdev. */
        spdk_bdev_module_release_bdev(pt_node->base_bdev);

        /* Close the underlying bdev. */
        spdk_bdev_close(pt_node->base_desc);


        /* Reset deduplication engine */
        if (spdk_get_thread() != pt_node->dedup_thread) {
                spdk_thread_send_msg(pt_node->dedup_thread,reset_dedup_engine, NULL);
        } else {
                reset_dedup_engine();
        }

        /* Unregister the io_device. */
        spdk_io_device_unregister(pt_node, _device_unregister_cb);

        return 0;
}


/******************Completion callback functions********************/

/* Completion callback for IO that were issued from this bdev. The original bdev_io
 * is passed in as an arg so we'll complete that one with the appropriate status
 * and then free the one that this module issued.
 */

/* General */
static void
_pt_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
        struct spdk_bdev_io *orig_io = cb_arg;
        int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
        struct dedupas_bdev_io *io_ctx = (struct dedupas_bdev_io *)orig_io->driver_ctx;

        /* We setup this value in the submission routine, just showing here that it is
         * passed back to us.
         */
        if (io_ctx->test != 0x5a) {
                SPDK_ERRLOG("Error, original IO device_ctx is wrong! 0x%x\n",
                            io_ctx->test);
        }

        /* Complete the original IO and then free the one that we created here
         * as a result of issuing an IO via submit_request.
         */
        spdk_bdev_io_complete(orig_io, status);
        spdk_bdev_free_io(bdev_io);
}


/* Write operations */
static void
_pt_complete_write_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
        struct complete_args *args = cb_arg;
        struct spdk_bdev_io *orig_io = args->orig_io;

        struct dedupas_bdev_io *io_ctx = (struct dedupas_bdev_io *)orig_io->driver_ctx;

        /* We setup this value in the submission routine, just showing here that it is
         * passed back to us.
         */
        if (io_ctx->test != 0x5a) {
                SPDK_ERRLOG("Error, original IO device_ctx is wrong! 0x%x\n",
                            io_ctx->test);
        }

        /* Check if all blocks have been written successfully. If so, complete the original IO and then free 
         * the one that we created here as a result of issuing an IO via submit_request.
         */
        if (!success) {
                       int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
                spdk_bdev_io_complete(orig_io, status);
        } else {
                args->counter = args->counter + 1;
                if (args->counter == args->goal){
                        int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
                        spdk_bdev_io_complete(orig_io, status);
                         free(args);
                }
        }

        spdk_bdev_free_io(bdev_io);
}


/* Read operations */
static void
_pt_complete_read_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
        
        struct complete_args *args = cb_arg;
        struct spdk_bdev_io *orig_io = args->orig_io;

        struct dedupas_bdev_io *io_ctx = (struct dedupas_bdev_io *)orig_io->driver_ctx;

        /* We setup this value in the submission routine, just showing here that it is
         * passed back to us.
         */
        if (io_ctx->test != 0x5a) {
                SPDK_ERRLOG("Error, original IO device_ctx is wrong! 0x%x\n",
                            io_ctx->test);
        }

        /* Check if all blocks have been read successfully. If so, complete the original IO 
         * and then free the one that we created here as a result of issuing an IO via submit_request.
         */
        if (!success) {
                int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
                spdk_bdev_io_complete(orig_io, status);
        } else {
                args->counter = args->counter + 1;
                if (args->counter == args->goal){
                        int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
                        spdk_bdev_io_complete(orig_io, status);
                        free(args);
                }
        }

        spdk_bdev_free_io(bdev_io);
}

/**************************************/


static void
vbdev_dedupas_resubmit_io(void *arg)
{
        struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
        struct dedupas_bdev_io *io_ctx = (struct dedupas_bdev_io *)bdev_io->driver_ctx;

        vbdev_dedupas_submit_request(io_ctx->ch, bdev_io);
}


static void
vbdev_dedupas_queue_io(struct spdk_bdev_io *bdev_io)
{
        struct dedupas_bdev_io *io_ctx = (struct dedupas_bdev_io *)bdev_io->driver_ctx;
        int rc;

        io_ctx->bdev_io_wait.bdev = bdev_io->bdev;
        io_ctx->bdev_io_wait.cb_fn = vbdev_dedupas_resubmit_io;
        io_ctx->bdev_io_wait.cb_arg = bdev_io;

        rc = spdk_bdev_queue_io_wait(bdev_io->bdev, io_ctx->ch, &io_ctx->bdev_io_wait);
        if (rc != 0) {
                SPDK_ERRLOG("Queue io failed in vbdev_dedupas_queue_io, rc=%d.\n", rc);
                spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        }
}


/* Contains the elements necessary to submit a read or write request to the base bdev,
 * after the deduplication engine has consulted.
 */
struct submit_after_request_args{
        struct spdk_bdev_io *bdev_io;
        uint64_t * paddrs;
        struct complete_args * complete_args;
        void *buf;
};


/* Submits a read or write request to the base bdev after the deduplication engine has consulted.
 */
static void submit_after_request(void *arg){
        struct submit_after_request_args *args = arg;
        struct spdk_bdev_io *bdev_io = args->bdev_io;

        struct dedupas_bdev_io *io_ctx = (struct dedupas_bdev_io *)bdev_io->driver_ctx;
        struct pt_io_channel *pt_ch = io_ctx->ch;
        struct spdk_io_channel *ch = spdk_io_channel_from_ctx(io_ctx->ch);
        struct vbdev_dedupas *pt_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_dedupas, pt_bdev);
        int rc = 0;
        
        int blocklen = bdev_io->bdev->blocklen;

             struct complete_args  *c_args = (struct complete_args *) args->complete_args;
        int blockcnt =  bdev_io->u.bdev.num_blocks;
        int i;
             
        switch (bdev_io->type) {
                case SPDK_BDEV_IO_TYPE_READ:
                        if (bdev_io->u.bdev.md_buf == NULL) {
                                for (i=0; i < blockcnt; i++) {
                                        rc = spdk_bdev_read_blocks(pt_node->base_desc, pt_ch->base_ch, 
                                                        args->buf + i*blocklen,
                                                        args->paddrs[i] * 8,
                                                             8,_pt_complete_read_io,
                                                             c_args);
                                }
                                
                                free(args->paddrs);
                        } else {
                                rc = spdk_bdev_readv_blocks_with_md(pt_node->base_desc, pt_ch->base_ch,
                                                bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
                                                bdev_io->u.bdev.md_buf,
                                                bdev_io->u.bdev.offset_blocks,
                                                bdev_io->u.bdev.num_blocks,
                                                _pt_complete_io, bdev_io);
                        }
                        break;
                case SPDK_BDEV_IO_TYPE_WRITE:
                        for (i=0; i < blockcnt -1; i++) {
                                if ( (int64_t) args->paddrs[i] >= 0 ) {
                                        rc = spdk_bdev_write_blocks(pt_node->base_desc, pt_ch->base_ch, args->buf + i*blocklen,
                                                        args->paddrs[i] * 8,
                                                        8, _pt_complete_write_io,
                                                        c_args);
                                } else {
                                        c_args->goal = c_args->goal - 1;
                                }
                        }
                        
                        if ( (int64_t) args->paddrs[i] >= 0 ) {
                                rc = spdk_bdev_write_blocks(pt_node->base_desc, pt_ch->base_ch, args->buf + i*blocklen,
                                                args->paddrs[i] * 8,
                                                8, _pt_complete_write_io,
                                                c_args);
                        } else {
                                c_args->goal = c_args->goal - 1;
                                
                                if (c_args->counter == c_args->goal){
                                        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
                                        free(c_args);
                                }
                        }
                        
                        free(args->paddrs);
                        break;
        }
        
        free(args);

        if (rc != 0) {
                if (rc == -ENOMEM) {
                        SPDK_ERRLOG("No memory, start to queue io for dedupas.\n");
                        io_ctx->ch = ch;
                        vbdev_dedupas_queue_io(bdev_io);
                } else {
                        SPDK_ERRLOG("ERROR on bdev_io submission!\n");
                        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                }
        }
}


/* Callback for getting a buf from the bdev pool in the event that the caller passed
 * in NULL, we need to own the buffer so it doesn't get freed by another vbdev module
 * beneath us before we're done with it. That won't happen in this example but it could
 * if this example were used as a template for something more complex.
 */
static void
pt_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
        struct vbdev_dedupas *pt_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_dedupas,
                                         pt_bdev);
        struct pt_io_channel *pt_ch = spdk_io_channel_get_ctx(ch);
        struct dedupas_bdev_io *io_ctx = (struct dedupas_bdev_io *)bdev_io->driver_ctx;
        int rc;
        int blocklen = bdev_io->bdev->blocklen;

        if (!success) {
                spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                return;
        }

        long int offset = bdev_io->u.bdev.offset_blocks; 

        struct submit_after_request_args * after_args = malloc(sizeof(struct submit_after_request_args));
        after_args->bdev_io = bdev_io;

        if (bdev_io->u.bdev.md_buf == NULL) {
                struct complete_args * c_args = malloc(sizeof(struct complete_args));
                c_args->orig_io = bdev_io;
                c_args->counter = 0;
                c_args->goal = bdev_io->u.bdev.num_blocks;
                
                after_args->paddrs =  malloc(sizeof(uint64_t) * bdev_io->u.bdev.num_blocks);
                after_args->complete_args = c_args;
                after_args->buf = bdev_io->u.bdev.iovs[0].iov_base;

                int i;
                for (i=0; i < bdev_io->u.bdev.num_blocks; i++) {
                        after_args->paddrs[i] = read_block(offset+i);
                }        
                
                spdk_thread_send_msg(io_ctx->orig_thread, submit_after_request, after_args);
                
        } else {
                spdk_thread_send_msg(io_ctx->orig_thread, submit_after_request, after_args);
        }

}


/* Consults the deduplication engine for write requests and obtains a buffer for read requests.  
 * This is executed by the deduplication thread.
 */
static void
_dedupas_bdev_io_submit(void *arg)
{
        struct spdk_bdev_io *bdev_io = arg;
        
        struct dedupas_bdev_io *io_ctx = (struct dedupas_bdev_io *)bdev_io->driver_ctx;
        struct pt_io_channel *pt_ch = io_ctx->ch;
        struct spdk_io_channel *ch = spdk_io_channel_from_ctx(io_ctx->ch);
        struct vbdev_dedupas *pt_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_dedupas, pt_bdev);
        int rc = 0;
        
        int blocklen = bdev_io->bdev->blocklen; 

        /* Setup a per IO context value; we don't do anything with it in the vbdev other
         * than confirm we get the same thing back in the completion callback just to
         * demonstrate.
         */
        io_ctx->test = 0x5a;

        switch (bdev_io->type) {
                case SPDK_BDEV_IO_TYPE_READ:
                        spdk_bdev_io_get_buf(bdev_io, pt_read_get_buf_cb,
                                        bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
                        break;
                case SPDK_BDEV_IO_TYPE_WRITE:;
                        
                        struct complete_args * args = malloc(sizeof(struct complete_args));
                        args->orig_io = bdev_io;
                        args->counter = 0;
                        args->goal = bdev_io->u.bdev.num_blocks;

                        uint64_t paddr;
                        int i;
                        
                        struct submit_after_request_args * after_args = malloc(sizeof(struct submit_after_request_args));
                        after_args->bdev_io = bdev_io;
                        after_args->paddrs =  malloc(sizeof(uint64_t) * bdev_io->u.bdev.num_blocks);
                        after_args->complete_args = args;
                        after_args->buf = bdev_io->u.bdev.iovs[0].iov_base;
                        
                        for (i=0; i < bdev_io->u.bdev.num_blocks; i++) {
                                paddr =  write_block(bdev_io->u.bdev.offset_blocks+i, bdev_io->u.bdev.iovs[0].iov_base + i*blocklen);
                                after_args->paddrs[i] = paddr;
                        }
                        spdk_thread_send_msg(io_ctx->orig_thread, submit_after_request, after_args);
                        break;
        }
}


/* Called when someone above submits IO to this vbdev. Read and write requests are sent
 * to _dedupas_bdev_io_submit to be performed by the deduplication thread. 
 */
static void
vbdev_dedupas_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
        struct vbdev_dedupas *pt_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_dedupas, pt_bdev);
        struct pt_io_channel *pt_ch = spdk_io_channel_get_ctx(ch);
        struct dedupas_bdev_io *io_ctx = (struct dedupas_bdev_io *)bdev_io->driver_ctx;
        int rc = 0;
        int blocklen = bdev_io->bdev->blocklen;

        /* Setup a per IO context value; we don't do anything with it in the vbdev other
         * than confirm we get the same thing back in the completion callback just to
         * demonstrate.
         */
        io_ctx->test = 0x5a;
        io_ctx->dedupas_bdev = pt_node;
        io_ctx->ch = pt_ch;
        io_ctx->orig_thread = spdk_get_thread();

        switch (bdev_io->type) {
        case SPDK_BDEV_IO_TYPE_READ:
                if (spdk_get_thread() != pt_node->dedup_thread) {
                        spdk_thread_send_msg(pt_node->dedup_thread, _dedupas_bdev_io_submit, bdev_io);
                } else {
                        _dedupas_bdev_io_submit(bdev_io);
                }
                break;
        case SPDK_BDEV_IO_TYPE_WRITE:
		if (bdev_io->u.bdev.md_buf == NULL) {
			if (spdk_get_thread() != pt_node->dedup_thread) {
				spdk_thread_send_msg(pt_node->dedup_thread, _dedupas_bdev_io_submit, bdev_io);
                        } else {
                                _dedupas_bdev_io_submit(bdev_io);
                        }
                } else {
                        rc = spdk_bdev_writev_blocks_with_md(pt_node->base_desc, pt_ch->base_ch,
                                                             bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
                                                             bdev_io->u.bdev.md_buf,
                                                             bdev_io->u.bdev.offset_blocks,
                                                             bdev_io->u.bdev.num_blocks,
                                                             _pt_complete_io, bdev_io);
                }
                break;
        case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
                rc = spdk_bdev_write_zeroes_blocks(pt_node->base_desc, pt_ch->base_ch,
                                                   bdev_io->u.bdev.offset_blocks,
                                                   bdev_io->u.bdev.num_blocks,
                                                   _pt_complete_io, bdev_io);
                break;
        case SPDK_BDEV_IO_TYPE_UNMAP:
                rc = spdk_bdev_unmap_blocks(pt_node->base_desc, pt_ch->base_ch,
                                            bdev_io->u.bdev.offset_blocks,
                                            bdev_io->u.bdev.num_blocks,
                                            _pt_complete_io, bdev_io);
                break;
        case SPDK_BDEV_IO_TYPE_FLUSH:
                rc = spdk_bdev_flush_blocks(pt_node->base_desc, pt_ch->base_ch,
                                            bdev_io->u.bdev.offset_blocks,
                                            bdev_io->u.bdev.num_blocks,
                                            _pt_complete_io, bdev_io);
                break;
        case SPDK_BDEV_IO_TYPE_RESET:
                spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
                break;
        default:
                SPDK_ERRLOG("dedupas: unknown I/O type %d\n", bdev_io->type);
                spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                return;
        }
        if (rc != 0) {
                if (rc == -ENOMEM) {
                        SPDK_ERRLOG("No memory, start to queue io for dedupas.\n");
                        io_ctx->ch = ch;
                        vbdev_dedupas_queue_io(bdev_io);
                } else {
                        SPDK_ERRLOG("ERROR on bdev_io submission!\n");
                        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                }
        }

}


/* We'll just call the base bdev and let it answer however if we were more
 * restrictive for some reason (or less) we could get the response back
 * and modify according to our purposes.
 */
static bool
vbdev_dedupas_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
        struct vbdev_dedupas *pt_node = (struct vbdev_dedupas *)ctx;

        return spdk_bdev_io_type_supported(pt_node->base_bdev, io_type);
}

/* We supplied this as an entry point for upper layers who want to communicate to this
 * bdev.  This is how they get a channel. We are passed the same context we provided when
 * we created our PT vbdev in examine() which, for this bdev, is the address of one of
 * our context nodes. From here we'll ask the SPDK channel code to fill out our channel
 * struct and we'll keep it in our PT node.
 */
static struct spdk_io_channel *
vbdev_dedupas_get_io_channel(void *ctx)
{
        struct vbdev_dedupas *pt_node = (struct vbdev_dedupas *)ctx;
        struct spdk_io_channel *pt_ch = NULL;

        /* The IO channel code will allocate a channel for us which consists of
         * the SPDK channel structure plus the size of our pt_io_channel struct
         * that we passed in when we registered our IO device. It will then call
         * our channel create callback to populate any elements that we need to
         * update.
         */
        pt_ch = spdk_get_io_channel(pt_node);

        return pt_ch;
}

/* This is the output for bdev_get_bdevs() for this vbdev */
static int
vbdev_dedupas_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
        struct vbdev_dedupas *pt_node = (struct vbdev_dedupas *)ctx;

        spdk_json_write_name(w, "dedupas");
        spdk_json_write_object_begin(w);
        spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&pt_node->pt_bdev));
        spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(pt_node->base_bdev));
        spdk_json_write_object_end(w);

        return 0;
}

/* This is used to generate JSON that can configure this module to its current state. */
static int
vbdev_dedupas_config_json(struct spdk_json_write_ctx *w)
{
        struct vbdev_dedupas *pt_node;

        TAILQ_FOREACH(pt_node, &g_pt_nodes, link) {
                spdk_json_write_object_begin(w);
                spdk_json_write_named_string(w, "method", "bdev_dedupas_create");
                spdk_json_write_named_object_begin(w, "params");
                spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(pt_node->base_bdev));
                spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&pt_node->pt_bdev));
                spdk_json_write_object_end(w);
                spdk_json_write_object_end(w);
        }
        return 0;
}

/* We provide this callback for the SPDK channel code to create a channel using
 * the channel struct we provided in our module get_io_channel() entry point. Here
 * we get and save off an underlying base channel of the device below us so that
 * we can communicate with the base bdev on a per channel basis.  If we needed
 * our own poller for this vbdev, we'd register it here.
 */
static int
pt_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
        struct pt_io_channel *pt_ch = ctx_buf;
        struct vbdev_dedupas *pt_node = io_device;

        pt_ch->base_ch = spdk_bdev_get_io_channel(pt_node->base_desc);

        return 0;
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created. If this bdev used its own poller, we'd unregister it here.
 */
static void
pt_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
        struct pt_io_channel *pt_ch = ctx_buf;

        spdk_put_io_channel(pt_ch->base_ch);
}

/* Create the dedupas association from the bdev and vbdev name and insert
 * on the global list. */
static int
vbdev_dedupas_insert_name(const char *bdev_name, const char *vbdev_name)
{
        struct bdev_names *name;

        TAILQ_FOREACH(name, &g_bdev_names, link) {
                if (strcmp(vbdev_name, name->vbdev_name) == 0) {
                        SPDK_ERRLOG("dedupas bdev %s already exists\n", vbdev_name);
                        return -EEXIST;
                }
        }

        name = calloc(1, sizeof(struct bdev_names));
        if (!name) {
                SPDK_ERRLOG("could not allocate bdev_names\n");
                return -ENOMEM;
        }

        name->bdev_name = strdup(bdev_name);
        if (!name->bdev_name) {
                SPDK_ERRLOG("could not allocate name->bdev_name\n");
                free(name);
                return -ENOMEM;
        }

        name->vbdev_name = strdup(vbdev_name);
        if (!name->vbdev_name) {
                SPDK_ERRLOG("could not allocate name->vbdev_name\n");
                free(name->bdev_name);
                free(name);
                return -ENOMEM;
        }

        TAILQ_INSERT_TAIL(&g_bdev_names, name, link);

        return 0;
}



/* On init, just parse config file and build list of pt vbdevs and bdev name pairs. */
static int
vbdev_dedupas_init(void)
{
        struct spdk_conf_section *sp = NULL;
        const char *conf_bdev_name = NULL;
        const char *conf_vbdev_name = NULL;
        struct bdev_names *name;
        int i, rc;

        sp = spdk_conf_find_section(NULL, "Passthru");
        if (sp == NULL) {
                return 0;
        }

        for (i = 0; ; i++) {
                if (!spdk_conf_section_get_nval(sp, "PT", i)) {
                        break;
                }

                conf_bdev_name = spdk_conf_section_get_nmval(sp, "PT", i, 0);
                if (!conf_bdev_name) {
                        SPDK_ERRLOG("Passthru configuration missing bdev name\n");
                        break;
                }

                conf_vbdev_name = spdk_conf_section_get_nmval(sp, "PT", i, 1);
                if (!conf_vbdev_name) {
                        SPDK_ERRLOG("Passthru configuration missing pt_bdev name\n");
                        break;
                }

                rc = vbdev_dedupas_insert_name(conf_bdev_name, conf_vbdev_name);
                if (rc != 0) {
                        return rc;
                }
        }
        TAILQ_FOREACH(name, &g_bdev_names, link) {
                SPDK_NOTICELOG("conf parse matched: %s\n", name->bdev_name);
        }
        return 0;
}

/* Called when the entire module is being torn down. */
static void
vbdev_dedupas_finish(void)
{
        struct bdev_names *name;

        while ((name = TAILQ_FIRST(&g_bdev_names))) {
                TAILQ_REMOVE(&g_bdev_names, name, link);
                free(name->bdev_name);
                free(name->vbdev_name);
                free(name);
        }
}

/* During init we'll be asked how much memory we'd like passed to us
 * in bev_io structures as context. Here's where we specify how
 * much context we want per IO.
 */
static int
vbdev_dedupas_get_ctx_size(void)
{
        return sizeof(struct dedupas_bdev_io);
}

/* Called when SPDK wants to save the current config of this vbdev module to
 * a file.
 */
static void
vbdev_dedupas_get_spdk_running_config(FILE *fp)
{
        struct bdev_names *names = NULL;

        fprintf(fp, "\n[Passthru]\n");
        TAILQ_FOREACH(names, &g_bdev_names, link) {
                fprintf(fp, "  PT %s %s\n", names->bdev_name, names->vbdev_name);
        }
        fprintf(fp, "\n");
}

/* Where vbdev_dedupas_config_json() is used to generate per module JSON config data, this
 * function is called to output any per bdev specific methods. For the PT module, there are
 * none.
 */
static void
vbdev_dedupas_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
        /* No config per bdev needed */
}

/* When we register our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_dedupas_fn_table = {
        .destruct                = vbdev_dedupas_destruct,
        .submit_request                = vbdev_dedupas_submit_request,
        .io_type_supported        = vbdev_dedupas_io_type_supported,
        .get_io_channel                = vbdev_dedupas_get_io_channel,
        .dump_info_json                = vbdev_dedupas_dump_info_json,
        .write_config_json        = vbdev_dedupas_write_config_json,
};

/* Called when the underlying base bdev goes away. */
static void
vbdev_dedupas_base_bdev_hotremove_cb(void *ctx)
{
        struct vbdev_dedupas *pt_node, *tmp;
        struct spdk_bdev *bdev_find = ctx;

        TAILQ_FOREACH_SAFE(pt_node, &g_pt_nodes, link, tmp) {
                if (bdev_find == pt_node->base_bdev) {
                        spdk_bdev_unregister(&pt_node->pt_bdev, NULL, NULL);
                }
        }
}

int _block_len;

/* Called to initiate the deduplication engine */
void init_dedup_engine(uint64_t blockcnt){
        init_dedup(blockcnt, 1 * 1024 * 1024 * 1024/ _block_len,  _block_len);
}


struct freeblocks_init_args{
        struct spdk_bdev_desc * desc;
        uint64_t blockcnt;
};

/* Called to initiate the deduplication engine freeblocks module */
void init_dedup_engine_freeblocks(void * args){

        struct freeblocks_init_args * data = args;
        struct spdk_io_channel* ch = spdk_bdev_get_io_channel(data->desc);;

        init_dedup_freeblocks(data->desc, ch, data->blockcnt,  1 * 1024 * 1024 * 1024/ _block_len, _block_len);

        free(data);
}


/* Create and register the dedupas vbdev if we find it in our list of bdev names.
 * This can be called either by the examine path or RPC method.
 */
static int
vbdev_dedupas_register(struct spdk_bdev *bdev)
{
        struct bdev_names *name;
        struct vbdev_dedupas *pt_node;
        int rc = 0;

        /* Check our list of names from config versus this bdev and if
         * there's a match, create the pt_node & bdev accordingly.
         */
        TAILQ_FOREACH(name, &g_bdev_names, link) {
                if (strcmp(name->bdev_name, bdev->name) != 0) {
                        continue;
                }

                SPDK_NOTICELOG("Match on %s\n", bdev->name);
                pt_node = calloc(1, sizeof(struct vbdev_dedupas));
                if (!pt_node) {
                        rc = -ENOMEM;
                        SPDK_ERRLOG("could not allocate pt_node\n");
                        break;
                }

                /* The base bdev that we're attaching to. */
                pt_node->base_bdev = bdev;
                pt_node->pt_bdev.name = strdup(name->vbdev_name);
                if (!pt_node->pt_bdev.name) {
                        rc = -ENOMEM;
                        SPDK_ERRLOG("could not allocate pt_bdev name\n");
                        free(pt_node);
                        break;
                }
                pt_node->pt_bdev.product_name = "dedupas";

                /* Copy some properties from the underlying base bdev. */
                pt_node->pt_bdev.write_cache = bdev->write_cache;
                pt_node->pt_bdev.required_alignment = bdev->required_alignment;
                pt_node->pt_bdev.optimal_io_boundary = bdev->optimal_io_boundary;
                //pt_node->pt_bdev.blocklen = bdev->blocklen;
                pt_node->pt_bdev.blocklen = _block_len; //bdev->blocklen;
                pt_node->pt_bdev.blockcnt = ((bdev->blockcnt)/8) - 1 * 1024 * 1024 * 1024 / _block_len;
                //pt_node->pt_bdev.blockcnt = bdev->blockcnt - 1 * 1024 * 1024 *1024/ 512;

                pt_node->pt_bdev.md_interleave = bdev->md_interleave;
                pt_node->pt_bdev.md_len = bdev->md_len;
                pt_node->pt_bdev.dif_type = bdev->dif_type;
                pt_node->pt_bdev.dif_is_head_of_md = bdev->dif_is_head_of_md;
                pt_node->pt_bdev.dif_check_flags = bdev->dif_check_flags;

                /* This is the context that is passed to us when the bdev
                 * layer calls in so we'll save our pt_bdev node here.
                 */
                pt_node->pt_bdev.ctxt = pt_node;
                pt_node->pt_bdev.fn_table = &vbdev_dedupas_fn_table;
                pt_node->pt_bdev.module = &dedupas_if;
                TAILQ_INSERT_TAIL(&g_pt_nodes, pt_node, link);

                spdk_io_device_register(pt_node, pt_bdev_ch_create_cb, pt_bdev_ch_destroy_cb,
                                        sizeof(struct pt_io_channel),
                                        name->vbdev_name);

                struct spdk_cpuset cpuset = {};
                spdk_cpuset_zero(&cpuset);
                spdk_cpuset_set_cpu(&cpuset, 1,true);
                struct spdk_thread*  new_thread = spdk_thread_create("dedup_thread", &cpuset);
                if (new_thread == NULL)
                {
			SPDK_ERRLOG("First thread creation failed.\n");
                        return ;
                }

                pt_node->dedup_thread = new_thread;
        
                if (spdk_get_thread() != pt_node->dedup_thread) {
			spdk_thread_send_msg(pt_node->dedup_thread, init_dedup_engine,  pt_node->pt_bdev.blockcnt);
		} else {
			init_dedup_engine(pt_node->pt_bdev.blockcnt);
		}        

                SPDK_NOTICELOG("io_device created at: 0x%p\n", pt_node);

                rc = spdk_bdev_open(bdev, true, vbdev_dedupas_base_bdev_hotremove_cb,
                                    bdev, &pt_node->base_desc);
                if (rc) {
                        SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev));
                        TAILQ_REMOVE(&g_pt_nodes, pt_node, link);
                        spdk_io_device_unregister(pt_node, NULL);
                        free(pt_node->pt_bdev.name);
                        free(pt_node);
                        break;
                }
                SPDK_NOTICELOG("bdev opened\n");

                rc = spdk_bdev_module_claim_bdev(bdev, pt_node->base_desc, pt_node->pt_bdev.module);
                if (rc) {
                        SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(bdev));
                        spdk_bdev_close(pt_node->base_desc);
                        TAILQ_REMOVE(&g_pt_nodes, pt_node, link);
                        spdk_io_device_unregister(pt_node, NULL);
                        free(pt_node->pt_bdev.name);
                        free(pt_node);
                        break;
                }
                SPDK_NOTICELOG("bdev claimed\n");

                rc = spdk_bdev_register(&pt_node->pt_bdev);
                if (rc) {
                        SPDK_ERRLOG("could not register pt_bdev\n");
                        spdk_bdev_module_release_bdev(&pt_node->pt_bdev);
                        spdk_bdev_close(pt_node->base_desc);
                        TAILQ_REMOVE(&g_pt_nodes, pt_node, link);
                        spdk_io_device_unregister(pt_node, NULL);
                        free(pt_node->pt_bdev.name);
                        free(pt_node);
                        break;
                }

                SPDK_NOTICELOG("non_persistent_deduplication bdev registered\n");
                SPDK_NOTICELOG("created dedup_bdev for: %s\n", name->vbdev_name);

                struct freeblocks_init_args * f_args = malloc(sizeof(struct freeblocks_init_args));
                f_args->desc = pt_node->base_desc;
                f_args->blockcnt =  pt_node->pt_bdev.blockcnt;

                spdk_cpuset_zero(&cpuset);
                spdk_cpuset_set_cpu(&cpuset, 2,true);
                struct spdk_thread*  new_thread_dedup_freeblocks = spdk_thread_create("dedup_thread_freeblocks", &cpuset); 

                if (spdk_get_thread() != pt_node->dedup_thread) {
                        spdk_thread_send_msg(new_thread_dedup_freeblocks, init_dedup_engine_freeblocks, f_args);
                } else {
                        init_dedup_engine_freeblocks(f_args);
                }
        }

        return rc;
}



/* Create the dedupas disk from the given bdev and vbdev name. */
int
bdev_dedupas_create_disk(const char *bdev_name, const char *vbdev_name,  int par_blocklen)
{
        struct spdk_bdev *bdev = NULL;
        int rc = 0;
        
        /* Insert the bdev into our global name list even if it doesn't exist yet,
         * it may show up soon...
         */
        rc = vbdev_dedupas_insert_name(bdev_name, vbdev_name);
        if (rc) {
                return rc;
        }

        bdev = spdk_bdev_get_by_name(bdev_name);
        if (!bdev) {
                /* This is not an error, we tracked the name above and it still
                 * may show up later.
                 */
                SPDK_NOTICELOG("vbdev creation deferred pending base bdev arrival\n");
                return 0;
        }
	
	_block_len = par_blocklen;       
        
	return vbdev_dedupas_register(bdev);
}

void
bdev_dedupas_delete_disk(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
        struct bdev_names *name;

        if (!bdev || bdev->module != &dedupas_if) {
                cb_fn(cb_arg, -ENODEV);
                return;
        }

        /* Remove the association (vbdev, bdev) from g_bdev_names. This is required so that the
         * vbdev does not get re-created if the same bdev is constructed at some other time,
         * unless the underlying bdev was hot-removed.
         */
        TAILQ_FOREACH(name, &g_bdev_names, link) {
                if (strcmp(name->vbdev_name, bdev->name) == 0) {
                        TAILQ_REMOVE(&g_bdev_names, name, link);
                        free(name->bdev_name);
                        free(name->vbdev_name);
                        free(name);
                        break;
                }
        }

        /* Additional cleanup happens in the destruct callback. */
        spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

/* Because we specified this function in our pt bdev function table when we
 * registered our pt bdev, we'll get this call anytime a new bdev shows up.
 * Here we need to decide if we care about it and if so what to do. We
 * parsed the config file at init so we check the new bdev against the list
 * we built up at that time and if the user configured us to attach to this
 * bdev, here's where we do it.
 */
static void
vbdev_dedupas_examine(struct spdk_bdev *bdev)
{
        vbdev_dedupas_register(bdev);

        spdk_bdev_module_examine_done(&dedupas_if);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_dedupas", SPDK_LOG_VBDEV_DEDUPAS)
