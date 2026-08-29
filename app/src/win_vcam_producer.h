#ifndef SC_WIN_VCAM_PRODUCER_H
#define SC_WIN_VCAM_PRODUCER_H

#include "common.h"

#ifdef _WIN32

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#define SC_WIN_VCAM_BACKING_FILE "C:\\ProgramData\\scrcpy-vcam\\frames.bin"
#define SC_WIN_VCAM_MAGIC 0x53435643u /* 'SCVC' */
#define SC_WIN_VCAM_VERSION 1u
#define SC_WIN_VCAM_FOURCC_NV12 0x3231564Eu /* 'NV12' */

struct sc_win_vcam_shared_header {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t fourcc;
    uint32_t frame_size;
    volatile LONG sequence;
    uint64_t timestamp_us;
};

struct sc_win_vcam_producer {
    HANDLE backing_file;
    HANDLE mapping;
    uint8_t *view;
    size_t mapping_size;
    uint32_t width;
    uint32_t height;
    bool enabled;
};

bool
sc_win_vcam_producer_init(struct sc_win_vcam_producer *producer,
                          const AVCodecContext *ctx);

bool
sc_win_vcam_producer_push(struct sc_win_vcam_producer *producer,
                          const AVFrame *frame);

void
sc_win_vcam_producer_destroy(struct sc_win_vcam_producer *producer);

#endif // _WIN32

#endif
