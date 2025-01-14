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
#ifndef QEMU_VIRTIO_VIDEO_H
#define QEMU_VIRTIO_VIDEO_H


#include <time.h>
#include "mfx/mfxvideo.h"
#include "standard-headers/linux/virtio_video.h"
#include "hw/virtio/virtio.h"
#include "sysemu/iothread.h"
#include "block/aio.h"

#define DEBUG_VIRTIO_VIDEO       //mask to disable all log
//#define DEBUG_VIRTIO_VIDEO_ALL //enable all log
//#define DEBUG_VIRTIO_VIDEO_IOV
#define DEBUG_VIRTIO_VIDEO_EVENT
//#define ENABLE_MEMORY_REMAP

#ifdef DEBUG_VIRTIO_VIDEO
#define DPRINTF(fmt, ...) \
    do { \
        struct timespec ts;    \
        clock_gettime(CLOCK_MONOTONIC, &ts); \
        fprintf(stderr, "%d""[%ld.%03ld |%s:%d] " fmt, \
        gettid(), \
        ts.tv_sec, ts.tv_nsec / 1000000, __FUNCTION__, __LINE__, ##__VA_ARGS__); } while (0)
    // do { fprintf(stderr, "virtio-video: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#ifdef DEBUG_VIRTIO_VIDEO_IOV
#define DPRINTF_IOV DPRINTF
#else
#define DPRINTF_IOV(fmt, ...) do { } while (0)
#endif

#ifdef DEBUG_VIRTIO_VIDEO_EVENT
#define DPRINTF_EVENT DPRINTF
#else
#define DPRINTF_EVENT(fmt, ...) do { } while (0)
#endif


#define TYPE_VIRTIO_VIDEO "virtio-video-device"

#define VIRTIO_VIDEO_VQ_SIZE 256

#define VIRTIO_VIDEO_VERSION 0
#define VIRTIO_VIDEO_CAPS_LENGTH_MAX 1024
#define VIRTIO_VIDEO_RESPONSE_LENGTH_MAX 1024

#define VIRTIO_VIDEO_QUEUE_NUM 2
#define VIRTIO_VIDEO_QUEUE_INPUT 0
#define VIRTIO_VIDEO_QUEUE_OUTPUT 1

#define VIRTIO_VIDEO(obj) \
        OBJECT_CHECK(VirtIOVideo, (obj), TYPE_VIRTIO_VIDEO)

#define MEMCPY_S(dest, src, destsz, srcsz)     memcpy(dest, src, MIN(destsz,srcsz))

typedef enum virtio_video_device_model {
    VIRTIO_VIDEO_DEVICE_V4L2_ENC = 1,
    VIRTIO_VIDEO_DEVICE_V4L2_DEC,
} virtio_video_device_model;

typedef enum virtio_video_backend {
    VIRTIO_VIDEO_BACKEND_VAAPI = 1,
    VIRTIO_VIDEO_BACKEND_FFMPEG,
    VIRTIO_VIDEO_BACKEND_GSTREAMER,
    VIRTIO_VIDEO_BACKEND_MEDIA_SDK,
} virtio_video_backend;

typedef enum virtio_video_stream_state {
    STREAM_STATE_INIT = 0,
    STREAM_STATE_RUNNING,
    STREAM_STATE_DRAIN,
    STREAM_STATE_INPUT_PAUSED,
    STREAM_STATE_TERMINATE,
    STREAM_STATE_DRAIN_PLUS_CLEAR,
    STREAM_STATE_DRAIN_PLUS_CLEAR_DISTROY,
} virtio_video_stream_state;

typedef union VirtIOVideoResourceSlice {
    struct {
        void *base;
        void *remapped_addr;
        hwaddr len;
    } page;
    struct {
        uint64_t uuid_low;
        uint64_t uuid_high;
    } object;
} VirtIOVideoResourceSlice;

typedef struct VirtIOVideoResource {
    AddressSpace *dma_as;
    uint32_t id;
    uint32_t planes_layout;
    uint32_t num_planes;
    uint32_t plane_offsets[VIRTIO_VIDEO_MAX_PLANES];
    uint32_t num_entries[VIRTIO_VIDEO_MAX_PLANES];
    VirtIOVideoResourceSlice *slices[VIRTIO_VIDEO_MAX_PLANES];
    void *remapped_base;
    uint32_t remapped_size;
    QLIST_ENTRY(VirtIOVideoResource) next;
} VirtIOVideoResource;

typedef struct VirtIOVideoStream VirtIOVideoStream;

/**
 * Tracks the work of a VIRTIO_VIDEO_CMD_RESOURCE_QUEUE command
 *
 * @resource, queue_type:   come from the request of guest
 * @timestamp:              serves as input for VIRTIO_VIDEO_QUEUE_TYPE_INPUT,
 *                          and output for VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT
 * @flags, size:            used for the response to guest
 */
typedef struct VirtIOVideoWork {
    VirtIOVideoStream *parent;
    VirtQueueElement *elem;
    VirtIOVideoResource *resource;
    uint32_t queue_type;
    uint64_t timestamp;
    uint32_t flags;
    uint32_t size;
    void *opaque;
    QTAILQ_ENTRY(VirtIOVideoWork) next;
} VirtIOVideoWork;

/**
 * Represents a frame being decoded or encoded
 *
 * An input VirtIOVideoWork just represents a buffer for input data. For
 * decoder, it is possible that the data is not enough for decoding of one
 * frame. So we need a seperate frame queue for the frames actually being
 * decoded.
 */
typedef struct VirtIOVideoFrame {
    uint64_t timestamp;
    void *opaque;
    bool used;
    uint32_t id;
    QTAILQ_ENTRY(VirtIOVideoFrame) next;
} VirtIOVideoFrame;

typedef struct VirtIOVideoQueueInfo {
    virtio_video_mem_type mem_type;
    virtio_video_params params;
    bool setted;
} VirtIOVideoQueueInfo;

/* 0 indicates that the control is invalid for current stream */
typedef struct VirtIOVideoControlInfo {
    uint32_t bitrate;
    uint32_t profile;
    uint32_t level;
} VirtIOVideoControlInfo;

/* stream-wide commands such as CMD_STREAM_DRAIN and CMD_QUEUE_CLEAR */
typedef struct VirtIOVideoCmd {
    VirtQueueElement *elem;
    uint32_t cmd_type;
} VirtIOVideoCmd;

typedef struct VirtIOVideo VirtIOVideo;

struct VirtIOVideoStream {
    uint32_t id;
    char tag[64];
    VirtIOVideo *parent;
    VirtIOVideoQueueInfo in;
    VirtIOVideoQueueInfo out;
    VirtIOVideoControlInfo control;
    virtio_video_stream_state state;
    int csd_received_after_clear;
    QemuMutex mutex;
    void *opaque;
    QLIST_HEAD(, VirtIOVideoResource) resource_list[VIRTIO_VIDEO_QUEUE_NUM];
    VirtIOVideoCmd inflight_cmd;
    QTAILQ_HEAD(, VirtIOVideoFrame) pending_frames;
    QTAILQ_HEAD(, VirtIOVideoWork) input_work;
    QTAILQ_HEAD(, VirtIOVideoWork) output_work;
    QLIST_ENTRY(VirtIOVideoStream) next;

    bool bTdRun;
    bool bVpp;
    bool bPreenc;
    bool bParamSetDone;
    QemuMutex mutex_out;
    uint32_t queue_clear_type;
    mfxVideoParam *mvp;
    bool bHasOutput;
};

typedef struct VirtIOVideoControl {
    uint32_t num;
    uint32_t *values;
} VirtIOVideoControl;

typedef struct VirtIOVideoFormatFrame {
    virtio_video_format_frame frame;
    virtio_video_format_range *frame_rates;
    QLIST_ENTRY(VirtIOVideoFormatFrame) next;
} VirtIOVideoFormatFrame;

/* profile & level only apply to coded format */
typedef struct VirtIOVideoFormat {
    virtio_video_format_desc desc;
    QLIST_HEAD(, VirtIOVideoFormatFrame) frames;
    VirtIOVideoControl profile;
    VirtIOVideoControl level;
    QLIST_ENTRY(VirtIOVideoFormat) next;
} VirtIOVideoFormat;

typedef struct VirtIOVideoConf {
    char *model;
    char *backend;
    IOThread *iothread;
} VirtIOVideoConf;

typedef struct VirtIOVideoEvent {
    VirtQueueElement *elem;
    uint32_t event_type;
    uint32_t stream_id;
    QTAILQ_ENTRY(VirtIOVideoEvent) next;
} VirtIOVideoEvent;

struct VirtIOVideo {
    VirtIODevice parent_obj;
    VirtIOVideoConf conf;
    virtio_video_device_model model;
    virtio_video_backend backend;
    virtio_video_config config;
    VirtQueue *cmd_vq, *event_vq;
    QTAILQ_HEAD(, VirtIOVideoEvent) event_queue;
    QLIST_HEAD(, VirtIOVideoStream) stream_list;
    QLIST_HEAD(, VirtIOVideoFormat) format_list[VIRTIO_VIDEO_QUEUE_NUM];
    void *opaque;
    QemuMutex mutex;
    AioContext *ctx;

    QLIST_HEAD(, VirtIOVideoStream) overdue_stream_list;
    QemuThread overdue_thread;
    QemuMutex overdue_mutex;
    bool overdue_run;
    QemuEvent overdue_event;
};

typedef struct EncodePresetParameters {
    uint16_t GopRefDist;

    uint16_t TargetUsage;

    uint16_t RateControlMethod;
    uint16_t ExtBRCUsage;
    uint16_t AsyncDepth;
    uint16_t BRefType;
    uint16_t AdaptiveMaxFrameSize;
    uint16_t LowDelayBRC;

    uint16_t IntRefType;
    uint16_t IntRefCycleSize;
    uint16_t IntRefQPDelta;
    uint16_t IntRefCycleDist;

    uint16_t WeightedPred;
    uint16_t WeightedBiPred;

    bool EnableBPyramid;
    bool EnablePPyramid;
} EncPresPara;

typedef struct DependentPresetParameters {
    uint16_t TargetKbps;
    uint16_t MaxKbps;
    uint16_t GopPicSize;
    uint16_t BufferSizeInKB;
    uint16_t LookAheadDepth;
    uint16_t MaxFrameSize;
} DepPresPara;

typedef struct VirtIOVideoEncodeParamPreset {
    EncPresPara epp;
    DepPresPara dpp;
} VirtIOVideoEncodeParamPreset;

typedef enum ExtBRCType {
    EXTBRC_DEFAULT,
    EXTBRC_OFF,
    EXTBRC_ON,
    EXTBRC_IMPLICIT
} ExtBRCType;

typedef enum EPresetModes
{
    PRESET_DEFAULT,
    PRESET_DSS,
    PRESET_CONF,
    PRESET_GAMING,
    PRESET_MAX_MODES
} EPresetModes;

typedef enum EPresetCodecs
{
    PRESET_AVC,
    PRESET_HEVC,
    PRESET_MAX_CODECS
} EPresetCodecs;
/* end */

#endif /* QEMU_VIRTIO_VIDEO_H */
