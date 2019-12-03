/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * deinterlace video filter - V4L2 M2M
 */

#include <drm_fourcc.h>

#include <linux/videodev2.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct V4L2Queue V4L2Queue;
typedef struct DeintV4L2M2MContext DeintV4L2M2MContext;

typedef struct V4L2PlaneInfo {
    int bytesperline;
    void * mm_addr;
    size_t length;
} V4L2PlaneInfo;

typedef struct V4L2Buffer {
    int enqueued;
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    int num_planes;
    V4L2PlaneInfo plane_info[VIDEO_MAX_PLANES];
    AVDRMFrameDescriptor drm_frame;
    V4L2Queue *q;
} V4L2Buffer;

typedef struct V4L2Queue {
    struct v4l2_format format;
    enum AVPixelFormat av_pix_fmt;
    int num_buffers;
    V4L2Buffer *buffers;
    DeintV4L2M2MContext *ctx;
} V4L2Queue;

typedef struct DeintV4L2M2MContext {
    const AVClass *class;

    int fd;
    int width;
    int height;

    int streaming;

    AVBufferRef *hw_frames_ctx;

    V4L2Queue output;
    V4L2Queue capture;
} DeintV4L2M2MContext;

static int deint_v4l2m2m_prepare_context(DeintV4L2M2MContext *ctx)
{
    struct v4l2_capability cap;
    int ret;

    memset(&cap, 0, sizeof(cap));
    ret = ioctl(ctx->fd, VIDIOC_QUERYCAP, &cap);
    if (ret < 0)
        return ret;

    if (!(cap.capabilities & V4L2_CAP_STREAMING))
        return AVERROR(EINVAL);

    if (cap.capabilities & V4L2_CAP_VIDEO_M2M) {
        ctx->capture.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ctx->output.format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

        return 0;
    }

    if (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) {
        ctx->capture.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ctx->output.format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

        return 0;
    }

    return AVERROR(EINVAL);
}

static int deint_v4l2m2m_try_format(V4L2Queue *queue)
{
    struct v4l2_format *fmt = &queue->format;
    DeintV4L2M2MContext *ctx = queue->ctx;
    int ret, field;

    ret = ioctl(ctx->fd, VIDIOC_G_FMT, fmt);
    if (ret)
        av_log(ctx, AV_LOG_ERROR, "VIDIOC_G_FMT failed: %d\n", ret);

    if (V4L2_TYPE_IS_OUTPUT(fmt->type))
        field = V4L2_FIELD_INTERLACED_TB;
    else
        field = V4L2_FIELD_NONE;

    if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
        fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        fmt->fmt.pix_mp.field = field;
        fmt->fmt.pix_mp.width = ctx->width;
        fmt->fmt.pix_mp.height = ctx->height;
    } else {
        fmt->fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        fmt->fmt.pix.field = field;
        fmt->fmt.pix.width = ctx->width;
        fmt->fmt.pix.height = ctx->height;
    }

    ret = ioctl(ctx->fd, VIDIOC_TRY_FMT, fmt);
    if (ret)
        return AVERROR(EINVAL);

    if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
        if (fmt->fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
            fmt->fmt.pix_mp.field != field) {
            av_log(ctx, AV_LOG_DEBUG, "format not supported for type %d\n", fmt->type);

            return AVERROR(EINVAL);
        }
    } else {
        if (fmt->fmt.pix.pixelformat != V4L2_PIX_FMT_NV12 ||
            fmt->fmt.pix.field != field) {
            av_log(ctx, AV_LOG_DEBUG, "format not supported for type %d\n", fmt->type);

            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static int deint_v4l2m2m_set_format(V4L2Queue *queue, uint32_t field)
{
    struct v4l2_format *fmt = &queue->format;
    DeintV4L2M2MContext *ctx = queue->ctx;
    int ret;

    if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type))
        fmt->fmt.pix_mp.field = field;
    else
        fmt->fmt.pix.field = field;

    ret = ioctl(ctx->fd, VIDIOC_S_FMT, fmt);
    if (ret)
        av_log(ctx, AV_LOG_ERROR, "VIDIOC_S_FMT failed: %d\n", ret);

    return ret;
}

static int deint_v4l2m2m_probe_device(DeintV4L2M2MContext *ctx, char *node)
{
    int ret;

    ctx->fd = open(node, O_RDWR | O_NONBLOCK, 0);
    if (ctx->fd < 0)
        return AVERROR(errno);

    ret = deint_v4l2m2m_prepare_context(ctx);
    if (ret)
        goto fail;

    ret = deint_v4l2m2m_try_format(&ctx->capture);
    if (ret)
        goto fail;

    ret = deint_v4l2m2m_try_format(&ctx->output);
    if (ret)
        goto fail;

    return 0;

fail:
    close(ctx->fd);
    ctx->fd = -1;

    return ret;
}

static int deint_v4l2m2m_find_device(DeintV4L2M2MContext *ctx)
{
    int ret = AVERROR(EINVAL);
    struct dirent *entry;
    char node[PATH_MAX];
    DIR *dirp;

    dirp = opendir("/dev");
    if (!dirp)
        return AVERROR(errno);

    for (entry = readdir(dirp); entry; entry = readdir(dirp)) {

        if (strncmp(entry->d_name, "video", 5))
            continue;

        snprintf(node, sizeof(node), "/dev/%s", entry->d_name);
        av_log(ctx, AV_LOG_DEBUG, "probing device %s\n", node);
        ret = deint_v4l2m2m_probe_device(ctx, node);
        if (!ret)
            break;
    }

    closedir(dirp);

    if (ret) {
        av_log(ctx, AV_LOG_ERROR, "Could not find a valid device\n");
        ctx->fd = -1;

        return ret;
    }

    av_log(ctx, AV_LOG_INFO, "Using device %s\n", node);

    return 0;
}

static int deint_v4l2m2m_enqueue_buffer(V4L2Buffer *buf)
{
    int ret;

    ret = ioctl(buf->q->ctx->fd, VIDIOC_QBUF, &buf->buffer);
    if (ret < 0)
        return AVERROR(errno);

    buf->enqueued = 1;

    return 0;
}

static int v4l2_buffer_export_drm(V4L2Buffer* avbuf)
{
    struct v4l2_exportbuffer expbuf;
    int i, ret;

    for (i = 0; i < avbuf->num_planes; i++) {
        memset(&expbuf, 0, sizeof(expbuf));

        expbuf.index = avbuf->buffer.index;
        expbuf.type = avbuf->buffer.type;
        expbuf.plane = i;

        ret = ioctl(avbuf->q->ctx->fd, VIDIOC_EXPBUF, &expbuf);
        if (ret < 0)
            return AVERROR(errno);

        if (V4L2_TYPE_IS_MULTIPLANAR(avbuf->buffer.type)) {
            /* drm frame */
            avbuf->drm_frame.objects[i].size = avbuf->buffer.m.planes[i].length;
            avbuf->drm_frame.objects[i].fd = expbuf.fd;
            avbuf->drm_frame.objects[i].format_modifier = DRM_FORMAT_MOD_LINEAR;
        } else {
            /* drm frame */
            avbuf->drm_frame.objects[0].size = avbuf->buffer.length;
            avbuf->drm_frame.objects[0].fd = expbuf.fd;
            avbuf->drm_frame.objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
        }
    }

    return 0;
}

static int deint_v4l2m2m_allocate_buffers(V4L2Queue *queue)
{
    struct v4l2_format *fmt = &queue->format;
    DeintV4L2M2MContext *ctx = queue->ctx;
    struct v4l2_requestbuffers req;
    int ret, i, j, multiplanar;
    uint32_t memory;

    memory = queue->av_pix_fmt == AV_PIX_FMT_DRM_PRIME && V4L2_TYPE_IS_OUTPUT(fmt->type) ?
        V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;

    multiplanar = V4L2_TYPE_IS_MULTIPLANAR(fmt->type);

    memset(&req, 0, sizeof(req));
    req.count = queue->num_buffers;
    req.memory = memory;
    req.type = fmt->type;

    ret = ioctl(ctx->fd, VIDIOC_REQBUFS, &req);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));

        return AVERROR(errno);
    }

    queue->num_buffers = req.count;
    queue->buffers = av_mallocz(queue->num_buffers * sizeof(V4L2Buffer));
    if (!queue->buffers) {
        av_log(ctx, AV_LOG_ERROR, "malloc enomem\n");

        return AVERROR(ENOMEM);
    }

    for (i = 0; i < queue->num_buffers; i++) {
        V4L2Buffer *buf = &queue->buffers[i];

        buf->enqueued = 0;
        buf->q = queue;

        buf->buffer.type = fmt->type;
        buf->buffer.memory = memory;
        buf->buffer.index = i;

        if (multiplanar) {
            buf->buffer.length = VIDEO_MAX_PLANES;
            buf->buffer.m.planes = buf->planes;
        }

        ret = ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf->buffer);
        if (ret < 0) {
            ret = AVERROR(errno);

            goto fail;
        }

        if (multiplanar)
            buf->num_planes = buf->buffer.length;
        else
            buf->num_planes = 1;

        for (j = 0; j < buf->num_planes; j++) {
            V4L2PlaneInfo *info = &buf->plane_info[j];

            if (multiplanar) {
                info->bytesperline = fmt->fmt.pix_mp.plane_fmt[j].bytesperline;
                info->length = buf->buffer.m.planes[j].length;

                if (queue->av_pix_fmt == AV_PIX_FMT_DRM_PRIME)
                    continue;

                info->mm_addr = mmap(NULL, buf->buffer.m.planes[j].length,
                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                     ctx->fd, buf->buffer.m.planes[j].m.mem_offset);
            } else {
                info->bytesperline = fmt->fmt.pix.bytesperline;
                info->length = buf->buffer.length;

                if (queue->av_pix_fmt == AV_PIX_FMT_DRM_PRIME)
                    continue;

                info->mm_addr = mmap(NULL, buf->buffer.length,
                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                     ctx->fd, buf->buffer.m.offset);
            }

            if (info->mm_addr == MAP_FAILED) {
                ret = AVERROR(ENOMEM);

                goto fail;
            }
        }

        if (!V4L2_TYPE_IS_OUTPUT(fmt->type)) {
            ret = deint_v4l2m2m_enqueue_buffer(buf);
            if (ret)
                goto fail;

            if (queue->av_pix_fmt == AV_PIX_FMT_DRM_PRIME) {
                ret = v4l2_buffer_export_drm(buf);
                if (ret)
                    goto fail;
            }
        }
    }

    return 0;

fail:
    /* TODO: close all dmabuf fds */
    av_free(queue->buffers);
    queue->buffers = NULL;

    return ret;
}

static int deint_v4l2m2m_streamon(V4L2Queue *queue)
{
    int type = queue->format.type;
    int ret;

    ret = ioctl(queue->ctx->fd, VIDIOC_STREAMON, &type);
    if (ret < 0)
        return AVERROR(errno);

    return 0;
}

static V4L2Buffer* deint_v4l2m2m_dequeue_buffer(V4L2Queue *queue, int timeout)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    DeintV4L2M2MContext *ctx = queue->ctx;
    struct v4l2_buffer buf = { 0 };
    V4L2Buffer* avbuf = NULL;
    struct pollfd pfd;
    short events;
    int ret;

    if (V4L2_TYPE_IS_OUTPUT(queue->format.type))
        events =  POLLOUT | POLLWRNORM;
    else
        events = POLLIN | POLLRDNORM;

    pfd.events = events;
    pfd.fd = ctx->fd;

    for (;;) {
        ret = poll(&pfd, 1, timeout);
        if (ret > 0)
            break;
        if (errno == EINTR)
            continue;
        return NULL;
    }

    if (pfd.revents & POLLERR)
        return NULL;

    if (pfd.revents & events) {
        memset(&buf, 0, sizeof(buf));
        buf.memory = V4L2_MEMORY_MMAP;
        buf.type = queue->format.type;
        if (V4L2_TYPE_IS_MULTIPLANAR(queue->format.type)) {
            memset(planes, 0, sizeof(planes));
            buf.length = VIDEO_MAX_PLANES;
            buf.m.planes = planes;
        }

        ret = ioctl(ctx->fd, VIDIOC_DQBUF, &buf);
        if (ret) {
            if (errno != EAGAIN)
                av_log(ctx, AV_LOG_DEBUG, "VIDIOC_DQBUF, errno (%s)\n",
                       av_err2str(AVERROR(errno)));
            return NULL;
        }

        avbuf = &queue->buffers[buf.index];
        avbuf->enqueued = 0;
        avbuf->buffer = buf;
        if (V4L2_TYPE_IS_MULTIPLANAR(queue->format.type)) {
            memcpy(avbuf->planes, planes, sizeof(planes));
            avbuf->buffer.m.planes = avbuf->planes;
        }

        return avbuf;
    }

    return NULL;
}

static V4L2Buffer *deint_v4l2m2m_find_free_buf(V4L2Queue *queue)
{
    int i;

    for (i = 0; i < queue->num_buffers; i++)
        if (!queue->buffers[i].enqueued)
            return &queue->buffers[i];

    return NULL;
}

static int v4l2_bufref_to_buf(V4L2Buffer *out, int plane, const uint8_t* data, int size, int offset, AVBufferRef* bref)
{
    unsigned int bytesused, length;

    if (plane >= out->num_planes)
        return AVERROR(EINVAL);

    length = out->plane_info[plane].length;
    bytesused = FFMIN(size+offset, length);

    memcpy((uint8_t*)out->plane_info[plane].mm_addr+offset, data, FFMIN(size, length-offset));

    if (V4L2_TYPE_IS_MULTIPLANAR(out->buffer.type)) {
        out->planes[plane].bytesused = bytesused;
        out->planes[plane].length = length;
    } else {
        out->buffer.bytesused = bytesused;
        out->buffer.length = length;
    }

    return 0;
}

static int deint_v4l2m2m_enqueue(V4L2Queue *queue, const AVFrame* frame)
{
    V4L2Buffer *buf;
    int ret, i;

    if (V4L2_TYPE_IS_OUTPUT(queue->format.type))
        while (deint_v4l2m2m_dequeue_buffer(queue, 0));

    buf = deint_v4l2m2m_find_free_buf(queue);
    if (!buf)
        return AVERROR(ENOMEM);

    if (buf->buffer.memory == V4L2_MEMORY_DMABUF) {
        AVDRMFrameDescriptor *drm_desc = (AVDRMFrameDescriptor *)frame->data[0];

        if (V4L2_TYPE_IS_MULTIPLANAR(buf->buffer.type))
            for (i = 0; i < drm_desc->nb_objects; i++)
                buf->buffer.m.planes[i].m.fd = drm_desc->objects[i].fd;
        else
            buf->buffer.m.fd = drm_desc->objects[0].fd;
    } else {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
        int height = V4L2_TYPE_IS_MULTIPLANAR(queue->format.type) ?
            queue->format.fmt.pix_mp.height : queue->format.fmt.pix.height;
        int planes_nb = 0;
        int offset = 0;

        for (i = 0; i < desc->nb_components; i++)
            planes_nb = FFMAX(planes_nb, desc->comp[i].plane + 1);

        for (i = 0; i < planes_nb; i++) {
            int size, h = height;

            if (i == 1 || i == 2)
                h = AV_CEIL_RSHIFT(h, desc->log2_chroma_h);

            size = frame->linesize[i] * h;
            ret = v4l2_bufref_to_buf(buf, 0, frame->data[i], size, offset, frame->buf[i]);
            if (ret)
                return ret;
            offset += size;
        }
    }

    return deint_v4l2m2m_enqueue_buffer(buf);
}

static void v4l2_free_buffer(void *opaque, uint8_t *unused)
{
    V4L2Buffer *buf = opaque;

    if (!V4L2_TYPE_IS_OUTPUT(buf->q->format.type))
        deint_v4l2m2m_enqueue_buffer(buf);
}

static uint8_t *v4l2_get_drm_frame(V4L2Buffer *avbuf, int height)
{
    AVDRMFrameDescriptor *drm_desc = &avbuf->drm_frame;
    AVDRMLayerDescriptor *layer;

    /* fill the DRM frame descriptor */
    drm_desc->nb_objects = avbuf->num_planes;
    drm_desc->nb_layers = 1;

    layer = &drm_desc->layers[0];
    layer->nb_planes = avbuf->num_planes;

    for (int i = 0; i < avbuf->num_planes; i++) {
        layer->planes[i].object_index = i;
        layer->planes[i].offset = 0;
        layer->planes[i].pitch = avbuf->plane_info[i].bytesperline;
    }

    layer->format = DRM_FORMAT_NV12;

    if (avbuf->num_planes == 1) {
        layer->nb_planes = 2;

        layer->planes[1].object_index = 0;
        layer->planes[1].offset = avbuf->plane_info[0].bytesperline * height;
        layer->planes[1].pitch = avbuf->plane_info[0].bytesperline;
    }

    return (uint8_t *)drm_desc;
}

static int deint_v4l2m2m_dequeue_frame(V4L2Queue *queue, AVFrame* frame, int timeout)
{
    DeintV4L2M2MContext *ctx = queue->ctx;
    V4L2Buffer* avbuf = NULL;
    int i;

    avbuf = deint_v4l2m2m_dequeue_buffer(queue, timeout);
    if (!avbuf) {
        av_log(ctx, AV_LOG_ERROR, "dequeueing failed\n");
        return AVERROR(EINVAL);
    }

    if (queue->av_pix_fmt == AV_PIX_FMT_DRM_PRIME) {
        frame->buf[0] = av_buffer_create((uint8_t *) &avbuf->drm_frame,
                                sizeof(avbuf->drm_frame),
                                v4l2_free_buffer,
                                avbuf, AV_BUFFER_FLAG_READONLY);
        if (!frame->buf[0])
            return AVERROR(ENOMEM);

        frame->data[0] = (uint8_t *)v4l2_get_drm_frame(avbuf, ctx->height);
        frame->format = AV_PIX_FMT_DRM_PRIME;
        frame->hw_frames_ctx = av_buffer_ref(ctx->hw_frames_ctx);
    } else {
        frame->format = AV_PIX_FMT_NV12;

        for (i = 0; i < avbuf->num_planes; i++) {
            if (i >= avbuf->num_planes)
                return AVERROR(EINVAL);

            frame->buf[i] = av_buffer_create((char *)avbuf->plane_info[i].mm_addr + avbuf->planes[i].data_offset,
                                             avbuf->plane_info[i].length, v4l2_free_buffer, avbuf, 0);
            if (!frame->buf[i])
                return AVERROR(ENOMEM);

            frame->linesize[i] = avbuf->plane_info[i].bytesperline;
            frame->data[i] = frame->buf[i]->data;
        }

        if (avbuf->num_planes == 1) {
            frame->linesize[1] = avbuf->plane_info[0].bytesperline;
            frame->data[1] = frame->buf[0]->data + avbuf->plane_info[0].bytesperline * ctx->height;
        }
    }

    frame->height = ctx->height;
    frame->width = ctx->width;

    if (avbuf->buffer.flags & V4L2_BUF_FLAG_ERROR) {
        av_log(ctx, AV_LOG_ERROR, "driver decode error\n");
        frame->decode_error_flags |= FF_DECODE_ERROR_INVALID_BITSTREAM;
    }

    return 0;
}

static int deint_v4l2m2m_dequeue(AVFilterContext *avctx, AVFrame *input_frame, int field)
{
    DeintV4L2M2MContext *ctx = avctx->priv;
    AVFilterLink *outlink    = avctx->outputs[0];
    AVFrame *output_frame    = NULL;
    int err;

    if (outlink->format == AV_PIX_FMT_DRM_PRIME)
        output_frame = av_frame_alloc();
    else
        output_frame = ff_get_video_buffer(outlink, ctx->width, ctx->height);

    if (!output_frame)
        return AVERROR(ENOMEM);

    err = deint_v4l2m2m_dequeue_frame(&ctx->capture, output_frame, field ? 0 : -1);
    if (err < 0) {
        av_log(ctx, AV_LOG_ERROR, "no frame (field %d)\n", field);
        goto fail;
    }

    err = av_frame_copy_props(output_frame, input_frame);
    if (err < 0)
        goto fail;

    output_frame->interlaced_frame = 0;

    output_frame->pts += field;

    return ff_filter_frame(outlink, output_frame);

fail:
    av_frame_free(&output_frame);
    return err;
}

static int deint_v4l2m2m_config_props(AVFilterLink *outlink)
{
    AVFilterLink *inlink     = outlink->src->inputs[0];
    AVFilterContext *avctx   = outlink->src;
    DeintV4L2M2MContext *ctx = avctx->priv;
    V4L2Queue *capture       = &ctx->capture;
    V4L2Queue *output        = &ctx->output;
    int ret;

    ctx->height = avctx->inputs[0]->h;
    ctx->width = avctx->inputs[0]->w;

    outlink->frame_rate = av_mul_q(inlink->frame_rate,
                                   (AVRational){ 2, 1 });
    outlink->time_base  = av_mul_q(inlink->time_base,
                                   (AVRational){ 1, 2 });

    ret = deint_v4l2m2m_find_device(ctx);
    if (ret)
        return ret;

    output->av_pix_fmt = avctx->inputs[0]->format;
    if (output->av_pix_fmt == AV_PIX_FMT_DRM_PRIME) {
        if (!inlink->hw_frames_ctx) {
            av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
            return AVERROR(EINVAL);
        }

        ctx->hw_frames_ctx = av_buffer_ref(inlink->hw_frames_ctx);
        if (!ctx->hw_frames_ctx)
            return AVERROR(ENOMEM);
    }

    capture->av_pix_fmt = avctx->outputs[0]->format;

    ret = deint_v4l2m2m_set_format(capture, V4L2_FIELD_NONE);
    if (ret)
        return ret;

    ret = deint_v4l2m2m_allocate_buffers(capture);
    if (ret)
        return ret;

    ret = deint_v4l2m2m_streamon(capture);
    if (ret)
        return ret;

    return 0;
}

static int deint_v4l2m2m_query_formats(AVFilterContext *avctx)
{
    static const enum AVPixelFormat pixel_formats[] = {
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NONE,
    };

    return ff_set_common_formats(avctx, ff_make_format_list(pixel_formats));
}

static int deint_v4l2m2m_filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *avctx   = link->dst;
    DeintV4L2M2MContext *ctx = avctx->priv;
    V4L2Queue *output        = &ctx->output;
    int ret;

    if (!ctx->streaming) {
        unsigned int field;

        /*if (in->interlaced_frame)
            field = V4L2_FIELD_NONE;
        else*/ if (in->top_field_first)
            field = V4L2_FIELD_INTERLACED_TB;
        else
            field = V4L2_FIELD_INTERLACED_BT;

        ret = deint_v4l2m2m_set_format(output, field);
        if (ret)
            return ret;

        ret = deint_v4l2m2m_allocate_buffers(output);
        if (ret)
            return ret;

        ret = deint_v4l2m2m_streamon(output);
        if (ret)
            return ret;
    }

    ret = deint_v4l2m2m_enqueue(output, in);
    if (ret)
        return ret;


    if (ctx->streaming >= 1) {
        ret = deint_v4l2m2m_dequeue(avctx, in, 0);
        if (ret)
            return ret;

        ret = deint_v4l2m2m_dequeue(avctx, in, 1);
        if (ret)
            return ret;
    }

    if (ctx->streaming < 2)
        ctx->streaming++;

    av_frame_free(&in);

    return 0;
}

static av_cold int deint_v4l2m2m_init(AVFilterContext *avctx)
{
    DeintV4L2M2MContext *ctx = avctx->priv;

    ctx->fd = -1;
    ctx->output.ctx = ctx;
    ctx->output.num_buffers = 6;
    ctx->capture.ctx = ctx;
    ctx->capture.num_buffers = 6;

    return 0;
}

static void deint_v4l2m2m_uninit(AVFilterContext *avctx)
{
    DeintV4L2M2MContext *ctx = avctx->priv;
    V4L2Queue *capture       = &ctx->capture;
    V4L2Queue *output        = &ctx->output;

    av_buffer_unref(&ctx->hw_frames_ctx);

    if (capture->buffers)
        av_free(capture->buffers);

    if (output->buffers)
        av_free(output->buffers);

    if (ctx->fd > -1)
        close(ctx->fd);
}

static const AVOption deinterlace_v4l2m2m_options[] = {
    { NULL },
};

AVFILTER_DEFINE_CLASS(deinterlace_v4l2m2m);

static const AVFilterPad deint_v4l2m2m_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = deint_v4l2m2m_filter_frame,
    },
    { NULL }
};

static const AVFilterPad deint_v4l2m2m_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = deint_v4l2m2m_config_props,
    },
    { NULL }
};

AVFilter ff_vf_deinterlace_v4l2m2m = {
    .name           = "deinterlace_v4l2m2m",
    .description    = NULL_IF_CONFIG_SMALL("V4L2 M2M deinterlacer"),
    .priv_size      = sizeof(DeintV4L2M2MContext),
    .init           = &deint_v4l2m2m_init,
    .uninit         = &deint_v4l2m2m_uninit,
    .query_formats  = &deint_v4l2m2m_query_formats,
    .inputs         = deint_v4l2m2m_inputs,
    .outputs        = deint_v4l2m2m_outputs,
    .priv_class     = &deinterlace_v4l2m2m_class,
};
