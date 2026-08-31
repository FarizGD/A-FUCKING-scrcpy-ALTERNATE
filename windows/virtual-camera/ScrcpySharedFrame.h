#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <stdint.h>
#include <string.h>

#define SCRCPY_VCAM_BACKING_FILE L"C:\\ProgramData\\scrcpy-vcam\\frames.bin"
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
    HANDLE backingFile = CreateFileW(SCRCPY_VCAM_BACKING_FILE,
                                     GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (backingFile == INVALID_HANDLE_VALUE) {
        hr = ScrcpyFillBlackNv12(scanline, pitch, width, height);
        buffer2D->Unlock2D();
        return hr;
    }

    HANDLE mapping = CreateFileMappingW(backingFile, nullptr,
                                        PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        CloseHandle(backingFile);
        hr = ScrcpyFillBlackNv12(scanline, pitch, width, height);
        buffer2D->Unlock2D();
        return hr;
    }

    BYTE *view = static_cast<BYTE *>(MapViewOfFile(mapping, FILE_MAP_READ,
                                                   0, 0, 0));
    if (!view) {
        CloseHandle(mapping);
        CloseHandle(backingFile);
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
        CloseHandle(backingFile);
        buffer2D->Unlock2D();
        return hr;
    }

    const BYTE *src = view + sizeof(ScrcpyVcamSharedHeader);
    bool copied = false;
    for (int attempt = 0; attempt < 4 && !copied; ++attempt) {
        // The file mapping is read-only in the Media Foundation consumer.
        // InterlockedCompareExchange() is a read-modify-write operation even
        // when exchanging the same value, so it can fault on this view. An
        // aligned 32-bit volatile load is atomic on our Windows x64 target;
        // barriers prevent reordering around the frame copy.
        LONG before = header->sequence;
        MemoryBarrier();
        if (before & 1) {
            Sleep(0);
            continue;
        }

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

        LONG after = header->sequence;
        MemoryBarrier();
        copied = before == after && !(after & 1);
    }

    if (!copied) {
        hr = ScrcpyFillBlackNv12(scanline, pitch, width, height);
    }

    UnmapViewOfFile(view);
    CloseHandle(mapping);
    CloseHandle(backingFile);
    buffer2D->Unlock2D();
    return hr;
}
