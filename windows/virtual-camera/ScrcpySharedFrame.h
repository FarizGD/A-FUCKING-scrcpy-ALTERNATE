#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <stdint.h>
#include <string.h>

#define SCRCPY_VCAM_MAPPING_NAME L"Local\\ScrcpyVirtualCameraFrames"
#define SCRCPY_VCAM_MAGIC 0x53435643u
#define SCRCPY_VCAM_VERSION 1u
#define SCRCPY_VCAM_FOURCC_NV12 0x3231564Eu

struct ScrcpyVcamSharedHeader {
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

inline HRESULT ScrcpyFillBlackNv12(BYTE *scanline, LONG pitch,
                                   UINT width, UINT height) {
    if (!scanline || pitch <= 0) {
        return E_INVALIDARG;
    }
    for (UINT y = 0; y < height; ++y) {
        memset(scanline + (size_t) y * pitch, 16, width);
    }
    BYTE *uv = scanline + (size_t) pitch * height;
    for (UINT y = 0; y < height / 2; ++y) {
        memset(uv + (size_t) y * pitch, 128, width);
    }
    return S_OK;
}

inline HRESULT ScrcpyCopySharedNv12(IMFSample *sample, UINT width, UINT height) {
    if (!sample) {
        return E_POINTER;
    }

    wil::com_ptr_nothrow<IMFMediaBuffer> mediaBuffer;
    RETURN_IF_FAILED(sample->GetBufferByIndex(0, &mediaBuffer));

    wil::com_ptr_nothrow<IMF2DBuffer2> buffer2D;
    RETURN_IF_FAILED(mediaBuffer->QueryInterface(IID_PPV_ARGS(&buffer2D)));

    BYTE *scanline = nullptr;
    LONG pitch = 0;
    BYTE *start = nullptr;
    DWORD length = 0;
    RETURN_IF_FAILED(buffer2D->Lock2DSize(MF2DBuffer_LockFlags_Write,
                                         &scanline, &pitch, &start, &length));

    HRESULT hr = S_OK;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE,
                                      SCRCPY_VCAM_MAPPING_NAME);
    if (!mapping) {
        hr = ScrcpyFillBlackNv12(scanline, pitch, width, height);
        buffer2D->Unlock2D();
        return hr;
    }

    BYTE *view = static_cast<BYTE *>(MapViewOfFile(mapping, FILE_MAP_READ,
                                                   0, 0, 0));
    if (!view) {
        CloseHandle(mapping);
        hr = ScrcpyFillBlackNv12(scanline, pitch, width, height);
        buffer2D->Unlock2D();
        return hr;
    }

    auto *header = reinterpret_cast<ScrcpyVcamSharedHeader *>(view);
    const size_t expected = (size_t) width * height * 3 / 2;
    if (header->magic != SCRCPY_VCAM_MAGIC
            || header->version != SCRCPY_VCAM_VERSION
            || header->fourcc != SCRCPY_VCAM_FOURCC_NV12
            || header->width != width || header->height != height
            || header->stride != width || header->frame_size < expected) {
        hr = ScrcpyFillBlackNv12(scanline, pitch, width, height);
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        buffer2D->Unlock2D();
        return hr;
    }

    const BYTE *src = view + sizeof(ScrcpyVcamSharedHeader);
    bool copied = false;
    for (int attempt = 0; attempt < 4 && !copied; ++attempt) {
        LONG before = InterlockedCompareExchange(&header->sequence, 0, 0);
        if (before & 1) {
            Sleep(0);
            continue;
        }

        MemoryBarrier();
        for (UINT y = 0; y < height; ++y) {
            memcpy(scanline + (size_t) y * pitch,
                   src + (size_t) y * width, width);
        }

        BYTE *dstUv = scanline + (size_t) pitch * height;
        const BYTE *srcUv = src + (size_t) width * height;
        for (UINT y = 0; y < height / 2; ++y) {
            memcpy(dstUv + (size_t) y * pitch,
                   srcUv + (size_t) y * width, width);
        }
        MemoryBarrier();

        LONG after = InterlockedCompareExchange(&header->sequence, 0, 0);
        copied = before == after && !(after & 1);
    }

    if (!copied) {
        hr = ScrcpyFillBlackNv12(scanline, pitch, width, height);
    }

    UnmapViewOfFile(view);
    CloseHandle(mapping);
    buffer2D->Unlock2D();
    return hr;
}
