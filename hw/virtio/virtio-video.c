/*
 * Virtio Video Device
 *
 * Copyright (C) 2021, Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * Authors: Colin Xu <colin.xu@intel.com>
 *          Zhuocheng Ding <zhuocheng.ding@intel.com>
 */
#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "sysemu/dma.h"
#include "exec/ram_addr.h"
#include "hw/virtio/virtio-video.h"
#include "virtio-video-util.h"
#include "virtio-video-msdk.h"

//#define VIRTIO_VIDEO_DEBUG 1
#if !defined VIRTIO_VIDEO_DEBUG && !defined DEBUG_VIRTIO_VIDEO_ALL
#undef DPRINTF
#define DPRINTF(fmt, ...) do { } while (0)
#endif

static struct {
    virtio_video_device_model id;
    const char *name;
} virtio_video_models[] = {
    {VIRTIO_VIDEO_DEVICE_V4L2_ENC, "v4l2-enc"},
    {VIRTIO_VIDEO_DEVICE_V4L2_DEC, "v4l2-dec"},
};

static struct {
    virtio_video_backend id;
    const char *name;
} virtio_video_backends[] = {
    {VIRTIO_VIDEO_BACKEND_VAAPI, "vaapi"},
    {VIRTIO_VIDEO_BACKEND_FFMPEG, "ffmpeg"},
    {VIRTIO_VIDEO_BACKEND_GSTREAMER, "gstreamer"},
    {VIRTIO_VIDEO_BACKEND_MEDIA_SDK, "media-sdk"},
};

static size_t virtio_video_process_cmd_query_capability(VirtIODevice *vdev,
    virtio_video_query_capability *req,
    virtio_video_query_capability_resp **resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoFormat *fmt;
    VirtIOVideoFormatFrame *fmt_frame;
    int num_descs = 0, i, dir;
    size_t len = sizeof(**resp);
    void *buf;

    switch(req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        dir = VIRTIO_VIDEO_QUEUE_INPUT;
        DPRINTF("CMD_QUERY_CAPABILITY: reported input formats\n");
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        dir = VIRTIO_VIDEO_QUEUE_OUTPUT;
        DPRINTF("CMD_QUERY_CAPABILITY: reported output formats\n");
        break;
    default:
        *resp = g_malloc0(sizeof(**resp));
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        (*resp)->hdr.stream_id = req->hdr.stream_id;
        error_report("CMD_QUERY_CAPABILITY: invalid queue type 0x%x",
                     req->queue_type);
        return len;
    }

    QLIST_FOREACH(fmt, &v->format_list[dir], next) {
        num_descs++;
        len += sizeof(fmt->desc);
        QLIST_FOREACH(fmt_frame, &fmt->frames, next) {
            len += sizeof(fmt_frame->frame) + fmt_frame->frame.num_rates *
                   sizeof(virtio_video_format_range);
        }
    }
    
    *resp = g_malloc0(len);
    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY;
    (*resp)->hdr.stream_id = req->hdr.stream_id;
    (*resp)->num_descs = num_descs;

    buf = (char *)(*resp) + sizeof(virtio_video_query_capability_resp);
    QLIST_FOREACH(fmt, &v->format_list[dir], next) {
        MEMCPY_S(buf, &fmt->desc, sizeof(fmt->desc), sizeof(fmt->desc));
        buf += sizeof(fmt->desc);
        QLIST_FOREACH(fmt_frame, &fmt->frames, next) {
            MEMCPY_S(buf, &fmt_frame->frame, sizeof(fmt_frame->frame), sizeof(fmt_frame->frame));
            buf += sizeof(fmt_frame->frame);
            for (i = 0; i < fmt_frame->frame.num_rates; i++) {
                MEMCPY_S(buf, &fmt_frame->frame_rates[i],
                       sizeof(virtio_video_format_range),
		       sizeof(virtio_video_format_range));
                buf += sizeof(virtio_video_format_range);
            }
        }
    }

    return len;
}

static size_t virtio_video_process_cmd_stream_create(VirtIODevice *vdev,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_stream_create(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_stream_destroy(VirtIODevice *vdev,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_stream_destroy(v, req, resp, elem);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_stream_drain(VirtIODevice *vdev,
    virtio_video_stream_drain *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_stream_drain(v, req, resp, elem);
    default:
        return 0;
    }
}

static int virtio_video_get_ramblock_fd(AddressSpace *as,
                                        hwaddr addr,
                                        hwaddr *plen,
                                        bool is_write)
{
    hwaddr len = *plen;
    hwaddr l, xlat;
    MemoryRegion *mr;
    struct RAMBlock *rb;
    FlatView *fv;
    if (len == 0)
    {
        return -1;
    }

    l = len;
    RCU_READ_LOCK_GUARD();
    fv = address_space_to_flatview(as);
    mr = flatview_translate(fv, addr, &xlat, &l, is_write, MEMTXATTRS_UNSPECIFIED);
    rb = mr->ram_block;

    DPRINTF("as:%p, addr:%p, mr:%p, ramblock:%p, file:%d\n", as, (void *)addr, mr, mr->ram_block, rb->fd);
    return rb->fd;
}

static int virtio_video_resource_create_page(VirtIOVideoResource *resource,
    virtio_video_mem_entry *entries, bool output)
{
    VirtIOVideoResourceSlice *slice;
    DMADirection dir = output ? DMA_DIRECTION_FROM_DEVICE :
                                DMA_DIRECTION_TO_DEVICE;
    hwaddr len;
    int i, j, n;
    uint32_t real_size = 0;
    char *remap_p, *remaped_p;
    int fd;

    for (i = 0, n = 0; i < resource->num_planes; i++)
    {
        resource->slices[i] = g_new0(VirtIOVideoResourceSlice,
                                     resource->num_entries[i]);
        DPRINTF("plane:%d, entry:%d\n", i, resource->num_entries[i]);
        for (j = 0; j < resource->num_entries[i]; j++, n++)
        {
            len = entries[n].length;
            slice = &resource->slices[i][j];

            slice->page.base = dma_memory_map(resource->dma_as,
                                              entries[n].addr, &len, dir);
            slice->page.len = len;
            real_size += len;

            if (len < entries[n].length)
            {
                dma_memory_unmap(resource->dma_as, slice->page.base,
                                 slice->page.len, dir, 0);
                goto error;
            }
        }
    }

    if (output)
    {
        #ifdef ENABLE_MEMORY_REMAP
        resource->remapped_base = mmap(NULL, real_size, PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        #else
        resource->remapped_base = MAP_FAILED;
        #endif

        if (resource->remapped_base == MAP_FAILED)
        {
            DPRINTF("remap failed, will use slice\n");
            resource->remapped_base = NULL;
        }
        else
        {
            resource->remapped_size = real_size;
            remap_p = resource->remapped_base;
            for (i = 0, n = 0; i < resource->num_planes; i++)
            {
                for (j = 0; j < resource->num_entries[i]; j++, n++)
                {
                    len = entries[n].length;
                    slice = &resource->slices[i][j];

                    fd = virtio_video_get_ramblock_fd(resource->dma_as, entries[n].addr, &len, dir);
                    if (fd == -1)
                    {
                        DPRINTF("remap failed,fd = %d\n", fd);
                        resource->remapped_base = NULL;
                        break;
                    }
                    remaped_p = (char *)mmap(remap_p, entries[n].length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, entries[n].addr);
                    if (remaped_p == MAP_FAILED || remaped_p != remap_p)
                    {
                        DPRINTF("remap failed, will use slice\n");
                        resource->remapped_base = NULL;
                        break;
                    }
                    slice->page.remapped_addr = remaped_p;
                    DPRINTF("entries[n].addr:%p, len:%d, to %p, hint:%p\n", (int *)entries[n].addr, (int)entries[n].length, remaped_p, (char *)remap_p);
                    remap_p = remaped_p + entries[n].length;
                }
            }
        }
    }

    DPRINTF("Create resource , len = %d\n", real_size);

    return 0;

error:
    for (n = 0; n < j; n++) {
        slice = &resource->slices[i][n];
        dma_memory_unmap(resource->dma_as, slice->page.base, slice->page.len,
                         dir, 0);
    }
    for (n = 0; n <= i; n++) {
        g_free(resource->slices[n]);
    }
    return -1;
}

static size_t virtio_video_process_cmd_resource_create(VirtIODevice *vdev,
    virtio_video_resource_create *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *stream;
    VirtIOVideoResource *resource;
    VirtIOVideoWork *work;
    virtio_video_format format;
    virtio_video_mem_type mem_type;
    size_t len, num_entries = 0;
    int i, dir;

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        error_report("CMD_RESOURCE_CREATE: stream %d not found",
                     req->hdr.stream_id);
        return len;
    }

    qemu_mutex_lock(&stream->mutex);
    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        DPRINTF("virtio_video_process_cmd_resource_create VIRTIO_VIDEO_QUEUE_TYPE_INPUT\n");
        format = stream->in.params.format;
        mem_type = stream->in.mem_type;
        dir = VIRTIO_VIDEO_QUEUE_INPUT;
        QTAILQ_FOREACH(work, &stream->input_work, next) {
            if (work->resource->id == req->resource_id) {
                break;
            }
        }
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        DPRINTF("virtio_video_process_cmd_resource_create VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT\n");
        format = stream->out.params.format;
        mem_type = stream->out.mem_type;
        dir = VIRTIO_VIDEO_QUEUE_OUTPUT;
        QTAILQ_FOREACH(work, &stream->output_work, next) {
            if (work->resource->id == req->resource_id) {
                break;
            }
        }
        break;
    default:
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_RESOURCE_CREATE: invalid queue type 0x%x",
                     req->queue_type);
        goto out;
    }

    if (work != NULL) {
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID;
        error_report("CMD_RESOURCE_CREATE: stream %d resource %d "
                     "already created", stream->id, req->resource_id);
        goto out;
    }

    /* the frontend reuses resource ids without first destroying them, so allow
     * it to replace a resource which is not in use */
    QLIST_FOREACH(resource, &stream->resource_list[dir], next) {
        if (resource->id == req->resource_id) {
            break;
        }
    }
    if (resource != NULL) {
        virtio_video_destroy_resource(resource, mem_type,
                                      dir == VIRTIO_VIDEO_QUEUE_INPUT);
    }

    if (!virtio_video_format_is_valid(format, req->num_planes)) {
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_RESOURCE_CREATE: stream %d try to create a resource"
                     "with %d planes for %s queue whose format is %s",
                     stream->id, req->num_planes,
                     dir == VIRTIO_VIDEO_QUEUE_INPUT ? "input" : "output",
                     virtio_video_format_name(format));
        goto out;
    }

    /* Frontend will not set planes_layout sometimes, try to fix it. */
    if (req->planes_layout != VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE &&
            req->planes_layout != VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER) {
        DPRINTF("CMD_RESOURCE_CREATE: stream %d meet invalid "
                "planes layout (0x%x), fixed up automatically\n",
                stream->id, req->planes_layout);

        if (mem_type == VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES)
            req->planes_layout = VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE;
        else
            req->planes_layout = VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER;
        
        if (stream->in.params.format == VIRTIO_VIDEO_FORMAT_NV12)
            req->planes_layout = VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER;
    }

    resource = g_new0(VirtIOVideoResource, 1);
    resource->dma_as = vdev->dma_as;
    resource->id = req->resource_id;
    resource->planes_layout = req->planes_layout;
    resource->num_planes = req->num_planes;
    resource->remapped_base = NULL;
    MEMCPY_S(&resource->plane_offsets, &req->plane_offsets,
           sizeof(resource->plane_offsets), sizeof(resource->plane_offsets));
   // memcpy(&resource->num_entries, &req->num_entries,
     //      sizeof(resource->num_entries));

    for (i = 0; i < req->num_planes; i++) {
        num_entries += req->num_entries[i];
        resource->num_entries[i] = req->num_entries[i];
    }
    switch (mem_type) {
    case VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES:
    {
        virtio_video_mem_entry *entries;
        size_t size;

        size = sizeof(virtio_video_mem_entry) * num_entries;
        entries = g_malloc(size);
        if (unlikely(iov_to_buf(elem->out_sg, elem->out_num,
                                sizeof(*req), entries, size) != size)) {
            virtio_error(vdev, "virtio-video resource create data incorrect");
            g_free(entries);
            g_free(resource);
            qemu_mutex_unlock(&stream->mutex);
            return 0;
        }

        if (virtio_video_resource_create_page(resource, entries,
                    req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) < 0) {
            error_report("CMD_RESOURCE_CREATE: stream %d failed to "
                         "map guest memory", stream->id);
            g_free(entries);
            g_free(resource);
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            goto out;
        }
        g_free(entries);
        break;
    }
    case VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT:
    {
        /* TODO: support object memory type */
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_RESOURCE_CREATE: stream %d unsupported "
                     "memory type (object)", stream->id);
        g_free(resource);
        goto out;
    }
    default:
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        g_free(resource);
        goto out;
    }

    QLIST_INSERT_HEAD(&stream->resource_list[dir], resource, next);
    // DPRINTF("CMD_RESOURCE_CREATE: stream %d created %s resource %d\n",
    //         stream->id, dir == VIRTIO_VIDEO_QUEUE_INPUT ? "input" : "output",
    //         resource->id);
out:
    qemu_mutex_unlock(&stream->mutex);
    return len;
}

static size_t virtio_video_process_cmd_resource_queue(VirtIODevice *vdev,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp,
    VirtQueueElement *elem)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_resource_queue(v, req, resp, elem);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_resource_destroy_all(VirtIODevice *vdev,
    virtio_video_resource_destroy_all *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_resource_destroy_all(v, req, resp, elem);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_queue_clear(VirtIODevice *vdev,
    virtio_video_queue_clear *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_queue_clear(v, req, resp, elem);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_get_params(VirtIODevice *vdev,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_get_params(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_set_params(VirtIODevice *vdev,
    virtio_video_set_params *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_set_params(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_query_control(VirtIODevice *vdev,
    virtio_video_query_control *req, virtio_video_query_control_resp **resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_query_control(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_get_control(VirtIODevice *vdev,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_get_control(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_set_control(VirtIODevice *vdev,
    virtio_video_set_control *req, virtio_video_set_control_resp *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_set_control(v, req, resp);
    default:
        return 0;
    }
}

/**
 * Process the command requested without blocking. The responce will not be
 * ready if the requested operation is blocking. The command will be recorded
 * and complete asynchronously.
 *
 * @return - 0 when response is ready, 1 when response is not ready, negative
 * value on failure
 */
static int virtio_video_process_command(VirtIODevice *vdev,
    VirtQueueElement *elem, size_t *resp_size)
{
    virtio_video_cmd_hdr hdr = {0};
    size_t len = *resp_size = 0;
    bool async = false;

#define CMD_GET_REQ(req, len) do {                                          \
        if (unlikely(iov_to_buf(elem->out_sg, elem->out_num, 0,             \
                                req, len) != len)) {                        \
            virtio_error(vdev, "virtio-video command request incorrect");   \
            return -1;                                                      \
        }                                                                   \
    } while (0)
#define CMD_SET_RESP(resp, len, alloc) do {                                 \
        DPRINTF("cmd: resp: 0x%x\n", ((struct virtio_video_cmd_hdr*)resp)->type);       \
        if (len == 0 || resp == NULL) {                                     \
            virtio_error(vdev, "virtio-video command unexpected error");    \
            return -1;                                                      \
        }                                                                   \
        if (unlikely(iov_from_buf(elem->in_sg, elem->in_num, 0,             \
                                  resp, len)!= len)) {                      \
            if (alloc) {                                                    \
                g_free(resp);                                               \
            }                                                               \
            virtio_error(vdev, "virtio-video command response incorrect");  \
            return -1;                                                      \
        }                                                                   \
    } while (0)

    CMD_GET_REQ(&hdr, sizeof(hdr));
    DPRINTF("command %s, stream %d\n", virtio_video_cmd_name(hdr.type),
                                       hdr.stream_id);

    switch (hdr.type) {
    case VIRTIO_VIDEO_CMD_QUERY_CAPABILITY:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_QUERY_CAPABILITY \n");
        virtio_video_query_capability req = {0};
        virtio_video_query_capability_resp *resp = NULL;

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_query_capability(vdev, &req, &resp);
        CMD_SET_RESP(resp, len, true);
        g_free(resp);
        break;
    }
    case VIRTIO_VIDEO_CMD_STREAM_CREATE:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_STREAM_CREATE \n");
        virtio_video_stream_create req = {0};
        virtio_video_cmd_hdr resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_stream_create(vdev, &req, &resp);
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_STREAM_DESTROY \n");
        virtio_video_stream_destroy req = {0};
        virtio_video_cmd_hdr resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_stream_destroy(vdev, &req, &resp, elem);
        if (len == 0) {
            async = true;
            break;
        }
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_STREAM_DRAIN:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_STREAM_DRAIN \n");
        virtio_video_stream_drain req = {0};
        virtio_video_cmd_hdr resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_stream_drain(vdev, &req, &resp, elem);
        if (len == 0) {
            async = true;
            break;
        }
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_RESOURCE_CREATE:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_RESOURCE_CREATE \n");
        virtio_video_resource_create req = {0};
        virtio_video_cmd_hdr resp = {0};

        if (elem->out_num < 2) {
            virtio_error(vdev, "virtio-video command missing headers");
            return -1;
        }

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_resource_create(vdev, &req, &resp, elem);
        if (len == 0)
            return -1;
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_RESOURCE_QUEUE:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_RESOURCE_QUEUE \n");
        virtio_video_resource_queue req = {0};
        virtio_video_resource_queue_resp resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_resource_queue(vdev, &req, &resp, elem);
        if (len == 0) {
            async = true;
            break;
        }
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL \n");
        virtio_video_resource_destroy_all req = {0};
        virtio_video_cmd_hdr resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_resource_destroy_all(vdev, &req, &resp, elem);
        if (len == 0) {
            async = true;
            break;
        }
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_QUEUE_CLEAR \n");
        virtio_video_queue_clear req = {0};
        virtio_video_cmd_hdr resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_queue_clear(vdev, &req, &resp, elem);
        if (len == 0) {
            async = true;
            break;
        }
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_GET_PARAMS:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_GET_PARAMS \n");
        virtio_video_get_params req = {0};
        virtio_video_get_params_resp resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_get_params(vdev, &req, &resp);
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_SET_PARAMS:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_SET_PARAMS \n");
        virtio_video_set_params req = {0};
        virtio_video_cmd_hdr resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_set_params(vdev, &req, &resp);
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_QUERY_CONTROL:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_QUERY_CONTROL \n");
        virtio_video_query_control req = {0};
        virtio_video_query_control_resp *resp = NULL;

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_query_control(vdev, &req, &resp);
        CMD_SET_RESP(resp, len, true);
        g_free(resp);
        break;
    }
    case VIRTIO_VIDEO_CMD_GET_CONTROL:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_GET_CONTROL \n");
        virtio_video_get_control req = {0};
        virtio_video_get_control_resp *resp = NULL;

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_get_control(vdev, &req, &resp);
        CMD_SET_RESP(resp, len, true);
        g_free(resp);
        break;
    }
    case VIRTIO_VIDEO_CMD_SET_CONTROL:
    {
        DPRINTF("cmd: VIRTIO_VIDEO_CMD_SET_CONTROL \n");
        virtio_video_set_control req = {0};
        virtio_video_set_control_resp resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_set_control(vdev, &req, &resp);
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    default:
        DPRINTF("cmd: Unsupported cmd opcode \n");
        error_report("Unsupported cmd opcode: 0x%x", hdr.type);
        break;
    }

    *resp_size = len;
    return async;
}

static void virtio_video_command_vq_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtQueueElement *elem;
    size_t len = 0;
    int ret;

    DPRINTF_EVENT("%s\n", __func__);

    for (;;) {
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem)
            break;

        if (elem->out_num < 1 || elem->in_num < 1) {
            virtio_error(vdev, "virtio-video command missing headers");
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            break;
        }

        qemu_mutex_lock(&v->mutex);
        ret = virtio_video_process_command(vdev, elem, &len);
        qemu_mutex_unlock(&v->mutex);

        if (ret < 0) {
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            break;
        } else if (ret == 0) {
            virtqueue_push(vq, elem, len);
            virtio_notify(vdev, vq);
            g_free(elem);
        } /* or return asynchronously */
    }
}

static void virtio_video_event_vq_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoEvent *event;
    VirtQueueElement *elem;

    for (;;) {
        qemu_mutex_lock(&v->mutex);

        /* handle pending event */
        event = QTAILQ_FIRST(&v->event_queue);
        DPRINTF_EVENT("event_queue_debug, %s, get first event:%p\n", __func__, event);
        if (event && event->elem == NULL) {
            elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
            if (!elem) {
                qemu_mutex_unlock(&v->mutex);
                break;
            }

            if (elem->in_num < 1) {
                virtio_error(vdev, "virtio-video event missing input");
                virtqueue_detach_element(vq, elem, 0);
                g_free(elem);
                qemu_mutex_unlock(&v->mutex);
                break;
            }
            if (elem->in_sg[0].iov_len < sizeof(virtio_video_event)) {
                virtio_error(vdev, "virtio-video event input too short");
                virtqueue_detach_element(vq, elem, 0);
                g_free(elem);
                qemu_mutex_unlock(&v->mutex);
                break;
            }
            event->elem = elem;
            QTAILQ_REMOVE(&v->event_queue, event, next);
            DPRINTF_EVENT("event_queue_debug, %s, remove&complete event:%p \n", __func__, event);
            virtio_video_event_complete(vdev, event);
            qemu_mutex_unlock(&v->mutex);
            continue;
        }

        qemu_mutex_unlock(&v->mutex);
        break;
    }
}

static void virtio_video_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    int i, ret = -1;

    DPRINTF_EVENT("%s\n", __func__);

    if (!v->conf.model) {
        error_setg(errp, "virtio-video model isn't set");
        return;
    }

    for (i = 0; i < ARRAY_SIZE(virtio_video_models); i++) {
        if (!strcmp(v->conf.model, virtio_video_models[i].name)) {
            v->model = virtio_video_models[i].id;
            break;
        }
    }
    if (i == ARRAY_SIZE(virtio_video_models)) {
        error_setg(errp, "Unknown virtio-video model %s", v->conf.model);
        return;
    }

    if (!v->conf.backend) {
        error_setg(errp, "virtio-video backend isn't set");
        return;
    }

    for (i = 0; i < ARRAY_SIZE(virtio_video_backends); i++) {
        if (!strcmp(v->conf.backend, virtio_video_backends[i].name)) {
            v->backend = virtio_video_backends[i].id;
            break;
        }
    }
    if (i == ARRAY_SIZE(virtio_video_backends)) {
        error_setg(errp, "Unknown virtio-video backend %s", v->conf.backend);
        return;
    }

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        virtio_init(vdev, "virtio-video-enc", VIRTIO_ID_VIDEO_ENC,
                    sizeof(virtio_video_config));
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        virtio_init(vdev, "virtio-video-dec", VIRTIO_ID_VIDEO_DEC,
                    sizeof(virtio_video_config));
        break;
    default:
        return;
    }

    v->config.version = VIRTIO_VIDEO_VERSION;
    v->config.max_caps_length = VIRTIO_VIDEO_CAPS_LENGTH_MAX;
    v->config.max_resp_length = VIRTIO_VIDEO_RESPONSE_LENGTH_MAX;

    v->cmd_vq = virtio_add_queue(vdev, VIRTIO_VIDEO_VQ_SIZE,
                                 virtio_video_command_vq_cb);
    v->event_vq = virtio_add_queue(vdev, VIRTIO_VIDEO_VQ_SIZE,
                                   virtio_video_event_vq_cb);

    DPRINTF_EVENT("event_queue_debug, %s, init\n", __func__);
    QTAILQ_INIT(&v->event_queue);
    QLIST_INIT(&v->stream_list);
    for (i = 0; i < VIRTIO_VIDEO_QUEUE_NUM; i++)
        QLIST_INIT(&v->format_list[i]);

    qemu_mutex_init(&v->mutex);
    if (v->conf.iothread) {
        object_ref(OBJECT(v->conf.iothread));
        v->ctx = iothread_get_aio_context(v->conf.iothread);
    } else {
        v->ctx = qemu_get_aio_context();
    }

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        ret = virtio_video_init_msdk(v);
        break;
    default:
        break;
    }

    if (ret) {
        qemu_mutex_destroy(&v->mutex);
        if (v->conf.iothread) {
            object_unref(OBJECT(v->conf.iothread));
        }
        virtio_del_queue(vdev, 0);
        virtio_del_queue(vdev, 1);
        virtio_cleanup(vdev);
        error_setg(errp, "Failed to initialize %s:%s", v->conf.model,
                   v->conf.backend);
    }
}

static void virtio_video_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoEvent *event, *tmp_event;
    VirtIOVideoFormat *fmt, *tmp_fmt;
    VirtIOVideoFormatFrame *frame, *tmp_frame;
    int i;

    DPRINTF_EVENT("%s\n", __func__);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        virtio_video_uninit_msdk(v);
        break;
    default:
        break;
    }

    QTAILQ_FOREACH_SAFE(event, &v->event_queue, next, tmp_event) {
        if (event->elem) {
            virtqueue_detach_element(v->event_vq, event->elem, 0);
            g_free(event->elem);
        }
        DPRINTF_EVENT("event_queue_debug, %s, remove:%p\n", __func__, event);
        g_free(event);
    }

    for (i = 0; i < VIRTIO_VIDEO_QUEUE_NUM; i++) {
        QLIST_FOREACH_SAFE(fmt, &v->format_list[i], next, tmp_fmt) {
            QLIST_FOREACH_SAFE(frame, &fmt->frames, next, tmp_frame) {
                g_free(frame->frame_rates);
                g_free(frame);
            }
            if (fmt->profile.num)
                g_free(fmt->profile.values);
            if (fmt->level.num)
                g_free(fmt->level.values);
            g_free(fmt);
        }
    }

    qemu_mutex_destroy(&v->mutex);
    if (v->conf.iothread) {
        object_unref(OBJECT(v->conf.iothread));
    }

    virtio_del_queue(vdev, 0);
    virtio_del_queue(vdev, 1);
    virtio_cleanup(vdev);
}

static void virtio_video_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    MEMCPY_S(config, &v->config, sizeof(v->config), sizeof(v->config));
}

static void virtio_video_set_config(VirtIODevice *vdev, const uint8_t *config)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    MEMCPY_S(&v->config, config, sizeof(v->config), sizeof(v->config));
}

static uint64_t virtio_video_get_features(VirtIODevice *vdev, uint64_t features,
                                          Error **errp)
{
    virtio_add_feature(&features, VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES);

    /* TODO: support object memory type */
    /* NOTE: frontend will try guest page first if both are presented */
    /* virtio_add_feature(&features, VIRTIO_VIDEO_F_RESOURCE_VIRTIO_OBJECT); */

    virtio_add_feature(&features, VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG);
    return features;
}

static const VMStateDescription vmstate_virtio_video = {
    .name = "virtio-video",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static Property virtio_video_properties[] = {
    DEFINE_PROP_STRING("model", VirtIOVideo, conf.model),
    DEFINE_PROP_STRING("backend", VirtIOVideo, conf.backend),
    DEFINE_PROP_LINK("iothread", VirtIOVideo, conf.iothread, TYPE_IOTHREAD,
                     IOThread *),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_video_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_virtio_video;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, virtio_video_properties);
    vdc->realize = virtio_video_device_realize;
    vdc->unrealize = virtio_video_device_unrealize;
    vdc->get_config = virtio_video_get_config;
    vdc->set_config = virtio_video_set_config;
    vdc->get_features = virtio_video_get_features;
}

static const TypeInfo virtio_video_info = {
    .name          = TYPE_VIRTIO_VIDEO,
    .parent        = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOVideo),
    .class_init    = virtio_video_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_video_info);
}

type_init(virtio_register_types)
