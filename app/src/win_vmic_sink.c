#include "win_vmic_sink.h"

#ifdef _WIN32

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <libavutil/samplefmt.h>

#include "util/log.h"

#define DOWNCAST(SINK) container_of(SINK, struct sc_win_vmic_sink, frame_sink)

static uint32_t
sc_win_vmic_get_channels(const AVCodecContext *ctx) {
#ifdef SCRCPY_LAVU_HAS_CHLAYOUT
    return (uint32_t) ctx->ch_layout.nb_channels;
#else
    return (uint32_t) ctx->channels;
#endif
}

static bool
sc_win_vmic_open(struct sc_win_vmic_sink *sink, const AVCodecContext *ctx) {
    uint32_t channels = sc_win_vmic_get_channels(ctx);
    if (!channels || channels > 8 || ctx->sample_rate <= 0) {
        LOGE("Unsupported virtual microphone format: %u channels @ %d Hz",
             channels, ctx->sample_rate);
        return false;
    }

    enum AVSampleFormat fmt = ctx->sample_fmt;
    if (fmt != AV_SAMPLE_FMT_FLT && fmt != AV_SAMPLE_FMT_FLTP) {
        const char *name = av_get_sample_fmt_name(fmt);
        LOGE("Windows virtual mic currently requires float audio (got %s)",
             name ? name : "unknown");
        return false;
    }

    // Two seconds is enough to absorb scheduling jitter while consumers keep
    // their own read cursor. The producer never blocks if a consumer is late.
    uint32_t capacity_frames = (uint32_t) ctx->sample_rate * 2u;
    size_t sample_count = (size_t) capacity_frames * channels;
    size_t mapping_size = sizeof(struct sc_win_vmic_shared_header)
                        + sample_count * sizeof(float);

    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                        PAGE_READWRITE, 0,
                                        (DWORD) mapping_size,
                                        SC_WIN_VMIC_MAPPING_NAME);
    if (!mapping) {
        LOGE("Could not create virtual microphone shared memory: %lu",
             GetLastError());
        return false;
    }

    void *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapping_size);
    if (!view) {
        LOGE("Could not map virtual microphone shared memory: %lu",
             GetLastError());
        CloseHandle(mapping);
        return false;
    }

    sink->mapping = mapping;
    sink->header = view;
    sink->samples = (float *) (sink->header + 1);
    sink->mapping_size = mapping_size;
    sink->channels = channels;
    sink->sample_rate = (uint32_t) ctx->sample_rate;
    sink->capacity_frames = capacity_frames;

    memset(view, 0, mapping_size);
    sink->header->magic = SC_WIN_VMIC_MAGIC;
    sink->header->version = SC_WIN_VMIC_VERSION;
    sink->header->sample_rate = sink->sample_rate;
    sink->header->channels = channels;
    sink->header->sample_format = SC_WIN_VMIC_SAMPLE_FORMAT_F32;
    sink->header->capacity_frames = capacity_frames;
    sink->header->write_frame = 0;
    sink->header->sequence = 0;
    sink->header->total_frames = 0;

    sink->opened = true;
    LOGI("Windows virtual microphone producer ready: %u Hz, %u channel%s (%s)",
         sink->sample_rate, channels, channels == 1 ? "" : "s",
         SC_WIN_VMIC_MAPPING_NAME);
    return true;
}

static void
sc_win_vmic_close(struct sc_win_vmic_sink *sink) {
    if (!sink->opened) {
        return;
    }

    sink->header->magic = 0;
    MemoryBarrier();

    UnmapViewOfFile(sink->header);
    CloseHandle(sink->mapping);

    sink->mapping = NULL;
    sink->header = NULL;
    sink->samples = NULL;
    sink->mapping_size = 0;
    sink->opened = false;
}

static void
sc_win_vmic_copy_frame(struct sc_win_vmic_sink *sink, const AVFrame *frame,
                       uint32_t src_start, uint32_t count,
                       uint32_t dst_frame) {
    uint32_t channels = sink->channels;
    uint32_t capacity = sink->capacity_frames;
    enum AVSampleFormat fmt = (enum AVSampleFormat) frame->format;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t src_frame = src_start + i;
        uint32_t out_frame = (dst_frame + i) % capacity;
        float *dst = &sink->samples[(size_t) out_frame * channels];

        if (fmt == AV_SAMPLE_FMT_FLT) {
            const float *src = (const float *) frame->extended_data[0]
                             + (size_t) src_frame * channels;
            memcpy(dst, src, channels * sizeof(float));
        } else {
            assert(fmt == AV_SAMPLE_FMT_FLTP);
            for (uint32_t ch = 0; ch < channels; ++ch) {
                const float *plane = (const float *) frame->extended_data[ch];
                dst[ch] = plane[src_frame];
            }
        }
    }
}

static bool
sc_win_vmic_push(struct sc_win_vmic_sink *sink, const AVFrame *frame) {
    if (!sink->opened || frame->nb_samples <= 0) {
        return true;
    }

    enum AVSampleFormat fmt = (enum AVSampleFormat) frame->format;
    if (fmt != AV_SAMPLE_FMT_FLT && fmt != AV_SAMPLE_FMT_FLTP) {
        LOGE("Virtual microphone audio format changed unexpectedly");
        return false;
    }

    uint32_t original_count = (uint32_t) frame->nb_samples;
    uint32_t count = original_count;
    uint32_t src_start = 0;
    if (count > sink->capacity_frames) {
        // Keep only the newest part if a pathological frame is larger than the
        // whole ring buffer.
        count = sink->capacity_frames;
        src_start = original_count - count;
    }

    uint32_t write_frame = (uint32_t) sink->header->write_frame;
    sc_win_vmic_copy_frame(sink, frame, src_start, count, write_frame);

    uint32_t next = (write_frame + count) % sink->capacity_frames;

    // Publish samples before advancing the cursor. Interlocked operations are
    // full memory barriers on Windows, so consumers never observe a cursor for
    // data that has not finished copying yet.
    InterlockedExchange(&sink->header->write_frame, (LONG) next);
    InterlockedExchangeAdd64(&sink->header->total_frames, (LONG64) count);
    InterlockedIncrement(&sink->header->sequence);
    return true;
}

static bool
sc_win_vmic_frame_sink_open(struct sc_frame_sink *frame_sink,
                            const AVCodecContext *ctx) {
    struct sc_win_vmic_sink *sink = DOWNCAST(frame_sink);
    return sc_win_vmic_open(sink, ctx);
}

static void
sc_win_vmic_frame_sink_close(struct sc_frame_sink *frame_sink) {
    struct sc_win_vmic_sink *sink = DOWNCAST(frame_sink);
    sc_win_vmic_close(sink);
}

static bool
sc_win_vmic_frame_sink_push(struct sc_frame_sink *frame_sink,
                            const AVFrame *frame) {
    struct sc_win_vmic_sink *sink = DOWNCAST(frame_sink);
    return sc_win_vmic_push(sink, frame);
}

void
sc_win_vmic_sink_init(struct sc_win_vmic_sink *sink) {
    static const struct sc_frame_sink_ops ops = {
        .open = sc_win_vmic_frame_sink_open,
        .close = sc_win_vmic_frame_sink_close,
        .push = sc_win_vmic_frame_sink_push,
    };

    memset(sink, 0, sizeof(*sink));
    sink->frame_sink.ops = &ops;
}

void
sc_win_vmic_sink_destroy(struct sc_win_vmic_sink *sink) {
    sc_win_vmic_close(sink);
}

#endif // _WIN32
