#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kMappingName[] = L"Local\\ScrcpyVirtualMicrophonePcm";
constexpr uint32_t kMagic = 0x43494D53u; // 'SMIC'
constexpr uint32_t kVersion = 1u;
constexpr uint32_t kSampleFormatF32 = 1u;

struct SharedHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t sampleFormat;
    uint32_t capacityFrames;
    volatile LONG writeFrame;
    volatile LONG sequence;
    volatile LONG64 totalFrames;
};

struct Mapping {
    HANDLE handle = nullptr;
    SharedHeader* header = nullptr;
    const float* samples = nullptr;

    ~Mapping() {
        if (header) {
            UnmapViewOfFile(header);
        }
        if (handle) {
            CloseHandle(handle);
        }
    }
};

bool openMapping(Mapping& mapping) {
    mapping.handle = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
    if (!mapping.handle) {
        std::wcerr << L"Could not open " << kMappingName
                   << L". Start scrcpy with SCRCPY_WIN_VMIC=1 first. Error: "
                   << GetLastError() << L"\n";
        return false;
    }

    void* view = MapViewOfFile(mapping.handle, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        std::wcerr << L"MapViewOfFile failed: " << GetLastError() << L"\n";
        return false;
    }

    mapping.header = static_cast<SharedHeader*>(view);
    mapping.samples = reinterpret_cast<const float*>(mapping.header + 1);

    if (mapping.header->magic != kMagic || mapping.header->version != kVersion) {
        std::wcerr << L"Unsupported scrcpy virtual mic mapping protocol.\n";
        return false;
    }
    if (mapping.header->sampleFormat != kSampleFormatF32) {
        std::wcerr << L"Unsupported shared sample format.\n";
        return false;
    }
    if (!mapping.header->sampleRate || !mapping.header->channels
            || !mapping.header->capacityFrames) {
        std::wcerr << L"Invalid shared audio header.\n";
        return false;
    }

    return true;
}

bool containsInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }

    std::wstring a = haystack;
    std::wstring b = needle;
    std::transform(a.begin(), a.end(), a.begin(), towlower);
    std::transform(b.begin(), b.end(), b.begin(), towlower);
    return a.find(b) != std::wstring::npos;
}

IMMDevice* findRenderDevice(const std::wstring& namePart) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        std::wcerr << L"Could not create MMDeviceEnumerator: 0x" << std::hex << hr << L"\n";
        return nullptr;
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    enumerator->Release();
    if (FAILED(hr)) {
        std::wcerr << L"EnumAudioEndpoints failed: 0x" << std::hex << hr << L"\n";
        return nullptr;
    }

    UINT count = 0;
    collection->GetCount(&count);
    IMMDevice* result = nullptr;

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }

        IPropertyStore* props = nullptr;
        std::wstring friendly;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value))
                    && value.vt == VT_LPWSTR && value.pwszVal) {
                friendly = value.pwszVal;
            }
            PropVariantClear(&value);
            props->Release();
        }

        if (containsInsensitive(friendly, namePart)) {
            std::wcout << L"Using render endpoint: " << friendly << L"\n";
            result = device;
            break;
        }

        device->Release();
    }

    collection->Release();
    return result;
}

bool isFloatFormat(const WAVEFORMATEX* fmt) {
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (fmt->wFormatTag != WAVE_FORMAT_EXTENSIBLE
            || fmt->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }
    auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
    return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
}

bool isPcm16Format(const WAVEFORMATEX* fmt) {
    if (fmt->wBitsPerSample != 16) {
        return false;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_PCM) {
        return true;
    }
    if (fmt->wFormatTag != WAVE_FORMAT_EXTENSIBLE
            || fmt->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }
    auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
    return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
}

float readSample(const Mapping& mapping, uint64_t absoluteFrame, uint32_t channel,
                 uint64_t publishedTotal, uint32_t publishedWriteFrame) {
    uint32_t capacity = mapping.header->capacityFrames;
    uint64_t age = publishedTotal - absoluteFrame;
    uint32_t ringFrame = (publishedWriteFrame + capacity
                        - static_cast<uint32_t>(age % capacity)) % capacity;
    return mapping.samples[static_cast<size_t>(ringFrame) * mapping.header->channels + channel];
}

void writeOutputFrame(BYTE* dst, UINT32 frameIndex, const WAVEFORMATEX* mix,
                      const Mapping& mapping, uint64_t sourceFrame,
                      uint64_t publishedTotal, uint32_t publishedWriteFrame) {
    const uint32_t inChannels = mapping.header->channels;
    const uint32_t outChannels = mix->nChannels;

    auto sourceForChannel = [&](uint32_t outChannel) -> float {
        if (inChannels == 1) {
            return readSample(mapping, sourceFrame, 0, publishedTotal, publishedWriteFrame);
        }
        if (outChannels == 1) {
            float sum = 0.0f;
            for (uint32_t ch = 0; ch < inChannels; ++ch) {
                sum += readSample(mapping, sourceFrame, ch, publishedTotal, publishedWriteFrame);
            }
            return sum / static_cast<float>(inChannels);
        }
        uint32_t inChannel = std::min(outChannel, inChannels - 1);
        return readSample(mapping, sourceFrame, inChannel, publishedTotal, publishedWriteFrame);
    };

    if (isFloatFormat(mix)) {
        float* out = reinterpret_cast<float*>(dst) + static_cast<size_t>(frameIndex) * outChannels;
        for (uint32_t ch = 0; ch < outChannels; ++ch) {
            out[ch] = sourceForChannel(ch);
        }
    } else {
        int16_t* out = reinterpret_cast<int16_t*>(dst) + static_cast<size_t>(frameIndex) * outChannels;
        for (uint32_t ch = 0; ch < outChannels; ++ch) {
            float sample = std::clamp(sourceForChannel(ch), -1.0f, 1.0f);
            out[ch] = static_cast<int16_t>(sample * 32767.0f);
        }
    }
}

int run(const std::wstring& deviceSubstring) {
    Mapping mapping;
    if (!openMapping(mapping)) {
        return 2;
    }

    IMMDevice* device = findRenderDevice(deviceSubstring);
    if (!device) {
        std::wcerr << L"No active render endpoint contains: " << deviceSubstring << L"\n";
        return 3;
    }

    IAudioClient* client = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&client));
    device->Release();
    if (FAILED(hr)) {
        std::wcerr << L"IAudioClient activation failed: 0x" << std::hex << hr << L"\n";
        return 4;
    }

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) {
        std::wcerr << L"GetMixFormat failed: 0x" << std::hex << hr << L"\n";
        client->Release();
        return 5;
    }

    if (mix->nSamplesPerSec != mapping.header->sampleRate) {
        std::wcerr << L"Sample-rate mismatch: scrcpy=" << mapping.header->sampleRate
                   << L", endpoint=" << mix->nSamplesPerSec
                   << L". Resampling is not implemented yet.\n";
        CoTaskMemFree(mix);
        client->Release();
        return 6;
    }

    if (!isFloatFormat(mix) && !isPcm16Format(mix)) {
        std::wcerr << L"Endpoint mix format is not float32 or PCM16.\n";
        CoTaskMemFree(mix);
        client->Release();
        return 7;
    }

    constexpr REFERENCE_TIME bufferDuration = 1000000; // 100 ms
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, mix, nullptr);
    if (FAILED(hr)) {
        std::wcerr << L"IAudioClient::Initialize failed: 0x" << std::hex << hr << L"\n";
        CoTaskMemFree(mix);
        client->Release();
        return 8;
    }

    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);

    IAudioRenderClient* render = nullptr;
    hr = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(hr)) {
        std::wcerr << L"Could not get IAudioRenderClient: 0x" << std::hex << hr << L"\n";
        CoTaskMemFree(mix);
        client->Release();
        return 9;
    }

    uint64_t readTotal = static_cast<uint64_t>(mapping.header->totalFrames);

    hr = client->Start();
    if (FAILED(hr)) {
        std::wcerr << L"IAudioClient::Start failed: 0x" << std::hex << hr << L"\n";
        render->Release();
        CoTaskMemFree(mix);
        client->Release();
        return 10;
    }

    std::wcout << L"Bridge running. scrcpy PCM -> " << deviceSubstring << L"\n";
    std::wcout << L"Press Ctrl+C to stop.\n";

    for (;;) {
        if (mapping.header->magic != kMagic) {
            std::wcerr << L"scrcpy virtual mic producer stopped.\n";
            break;
        }

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) {
            break;
        }
        UINT32 available = bufferFrames - padding;
        if (!available) {
            Sleep(2);
            continue;
        }

        BYTE* out = nullptr;
        hr = render->GetBuffer(available, &out);
        if (FAILED(hr)) {
            break;
        }

        uint64_t publishedTotal = static_cast<uint64_t>(mapping.header->totalFrames);
        uint32_t publishedWrite = static_cast<uint32_t>(mapping.header->writeFrame);
        uint64_t capacity = mapping.header->capacityFrames;

        if (publishedTotal > readTotal + capacity) {
            readTotal = publishedTotal - capacity;
        }

        uint64_t ready = publishedTotal - readTotal;
        UINT32 copyFrames = static_cast<UINT32>(std::min<uint64_t>(available, ready));

        for (UINT32 i = 0; i < copyFrames; ++i) {
            writeOutputFrame(out, i, mix, mapping, readTotal + i,
                             publishedTotal, publishedWrite);
        }

        size_t frameBytes = mix->nBlockAlign;
        if (copyFrames < available) {
            ZeroMemory(out + static_cast<size_t>(copyFrames) * frameBytes,
                       static_cast<size_t>(available - copyFrames) * frameBytes);
        }

        readTotal += copyFrames;
        hr = render->ReleaseBuffer(available, 0);
        if (FAILED(hr)) {
            break;
        }

        Sleep(2);
    }

    client->Stop();
    render->Release();
    CoTaskMemFree(mix);
    client->Release();
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::wcerr << L"Usage: scrcpy-vmic-bridge.exe \"render endpoint name substring\"\n"
                   << L"Example: scrcpy-vmic-bridge.exe \"scrcpy Virtual Microphone Feed\"\n";
        return 1;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::wcerr << L"CoInitializeEx failed: 0x" << std::hex << hr << L"\n";
        return 1;
    }

    int result = run(argv[1]);
    CoUninitialize();
    return result;
}
