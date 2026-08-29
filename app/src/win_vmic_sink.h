#ifndef SC_WIN_VMIC_SINK_H
#define SC_WIN_VMIC_SINK_H

#include "common.h"

#ifdef _WIN32

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include <libavcodec/avcodec.h>

#include "trait/frame_sink.h"

#define SC_WIN_VMIC_MAPPING_NAME "Local\\ScrcpyVirtualMicrophonePcm"
#define SC_WIN_VMIC_MAGIC 0x43494D53u /* 'SMIC' */
#define SC_WIN_VMIC_VERSION 1u
#define SC_WIN_VMIC_SAMPLE_FORMAT_F32 1u

struct sc_win_vmic_shared_header {
    uint32_t magic;
    uint32_t version;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t sample_format;
    uint32_t capacity_frames;
    volatile LONG write_frame;
    volatile LONG sequence;
    volatile LONG64 total_frames;
};

struct sc_win_vmic_sink {
    struct sc_frame_sink frame_sink;

    HANDLE mapping;
    struct sc_win_vmic_shared_header *header;
    float *samples;
    size_t mapping_size;
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t capacity_frames;
    bool opened;
};

void
sc_win_vmic_sink_init(struct sc_win_vmic_sink *sink);

void
sc_win_vmic_sink_destroy(struct sc_win_vmic_sink *sink);

#endif // _WIN32

#endif // SC_WIN_VMIC_SINK_H
