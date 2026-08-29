#include "win_vcam_producer.h"

#ifdef _WIN32

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/pixfmt.h>

#include "util/log.h"

static uint64_t
sc_win_vcam_timestamp_us(void) {
    return GetTickCount64() * 1000ULL;
}

static void
copy_plane(uint8_t *dst, size_t dst_stride, const uint8_t *src,
           ptrdiff_t src_stride, uint32_t width, uint32_t height) {
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(dst + y * dst_stride, src + y * src_stride, width);
    }
}

bool
sc_win_vcam_producer_init(struct sc_win_vcam_producer *producer,
                          const AVCodecContext *ctx) {
    memset(producer, 0, sizeof(*producer));

    if (ctx->pix_fmt != AV_PIX_FMT_YUV420P) {
        LOGW("Windows virtual camera producer expects YUV420P, got pix_fmt=%d",
             ctx->pix_fmt);
        return false;
    }

    if (!ctx->width || !ctx->height || (ctx->width & 1) || (ctx->height & 1)) {
        LOGE("Windows virtual camera requires an even non-zero frame size");
        return false;
    }

    uint64_t frame_size64 = (uint64_t) ctx->width * ctx->height * 3 / 2;
    uint64_t mapping_size64 = sizeof(struct sc_win_vcam_shared_header)
                            + frame_size64;
    if (mapping_size64 > SIZE_MAX || mapping_size64 > UINT32_MAX) {
        LOGE("Windows virtual camera frame mapping is too large");
        return false;
    }

    size_t mapping_size = (size_t) mapping_size64;
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                        PAGE_READWRITE, 0,
                                        (DWORD) mapping_size,
                                        SC_WIN_VCAM_MAPPING_NAME);
    if (!mapping) {
        LOGE("Could not create Windows virtual camera mapping (error %lu)",
             GetLastError());
        return false;
    }

    uint8_t *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                  mapping_size);
    if (!view) {
        LOGE("Could not map Windows virtual camera memory (error %lu)",
             GetLastError());
        CloseHandle(mapping);
        return false;
    }

    struct sc_win_vcam_shared_header *header =
        (struct sc_win_vcam_shared_header *) view;
    memset(header, 0, sizeof(*header));
    header->magic = SC_WIN_VCAM_MAGIC;
    header->version = SC_WIN_VCAM_VERSION;
    header->width = (uint32_t) ctx->width;
    header->height = (uint32_t) ctx->height;
    header->stride = (uint32_t) ctx->width;
    header->fourcc = SC_WIN_VCAM_FOURCC_NV12;
    header->frame_size = (uint32_t) frame_size64;
    header->sequence = 0;
    header->timestamp_us = 0;

    producer->mapping = mapping;
    producer->view = view;
    producer->mapping_size = mapping_size;
    producer->width = (uint32_t) ctx->width;
    producer->height = (uint32_t) ctx->height;
    producer->enabled = true;

    LOGI("Windows virtual camera producer ready: %ux%u NV12 (%s)",
         producer->width, producer->height, SC_WIN_VCAM_MAPPING_NAME);
    return true;
}

bool
sc_win_vcam_producer_push(struct sc_win_vcam_producer *producer,
                          const AVFrame *frame) {
    if (!producer->enabled) {
        return true;
    }

    assert(frame->format == AV_PIX_FMT_YUV420P);

    if ((uint32_t) frame->width != producer->width
            || (uint32_t) frame->height != producer->height) {
        LOGE("Windows virtual camera resolution changed from %ux%u to %dx%d",
             producer->width, producer->height, frame->width, frame->height);
        return false;
    }

    struct sc_win_vcam_shared_header *header =
        (struct sc_win_vcam_shared_header *) producer->view;
    uint8_t *dst = producer->view + sizeof(*header);

    uint32_t width = producer->width;
    uint32_t height = producer->height;

    // NV12 = full-size Y plane followed by interleaved UV at half height.
    copy_plane(dst, width, frame->data[0], frame->linesize[0], width, height);

    uint8_t *dst_uv = dst + (size_t) width * height;
    uint32_t chroma_width = width / 2;
    uint32_t chroma_height = height / 2;
    for (uint32_t y = 0; y < chroma_height; ++y) {
        const uint8_t *src_u = frame->data[1] + y * frame->linesize[1];
        const uint8_t *src_v = frame->data[2] + y * frame->linesize[2];
        uint8_t *row = dst_uv + (size_t) y * width;
        for (uint32_t x = 0; x < chroma_width; ++x) {
            row[2 * x] = src_u[x];
            row[2 * x + 1] = src_v[x];
        }
    }

    header->timestamp_us = sc_win_vcam_timestamp_us();
    MemoryBarrier();
    InterlockedIncrement(&header->sequence);

    return true;
}

void
sc_win_vcam_producer_destroy(struct sc_win_vcam_producer *producer) {
    if (!producer->enabled) {
        return;
    }

    UnmapViewOfFile(producer->view);
    CloseHandle(producer->mapping);
    memset(producer, 0, sizeof(*producer));
    LOGD("Windows virtual camera producer stopped");
}

#endif // _WIN32
