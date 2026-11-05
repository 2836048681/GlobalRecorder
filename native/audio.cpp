#include "audio.h"

#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <objidl.h>
#include <propidl.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <queue>
#include <utility>
#include <vector>

namespace lightrec {
namespace {
constexpr uint32_t rate = 16000;
constexpr uint64_t chunkFrames = 10ULL * rate;
constexpr uint64_t maxDataBytes = UINT32_MAX - 36U;

// MinGW's SDK lacks audioclientactivationparams.h. Match the Windows SDK ABI.
struct ProcessLoopbackParams {
    DWORD targetProcessId;
    int32_t mode; // PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0
};
struct AudioClientActivationParams {
    int32_t activationType; // AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
    ProcessLoopbackParams processLoopback;
};
static_assert(sizeof(AudioClientActivationParams) == 12, "process loopback ABI");
using ActivateAsync = HRESULT (WINAPI*)(
    const WCHAR*, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
    IActivateAudioInterfaceAsyncOperation**);
std::atomic<bool> activationOutstanding{false};

class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler,
                                public IAgileObject {
public:
    ActivationHandler() : completed_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationHandler() { if (completed_) CloseHandle(completed_); }
    HANDLE event() const { return completed_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) ||
            IsEqualIID(iid, __uuidof(IActivateAudioInterfaceCompletionHandler)))
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (IsEqualIID(iid, IID_IAgileObject))
            *out = static_cast<IAgileObject*>(this);
        if (!*out) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(completed_);
        activationOutstanding.store(false);
        Release(); // Explicit in-flight reference survives a caller timeout.
        return S_OK;
    }
private:
    std::atomic<ULONG> refs_{1};
    HANDLE completed_;
};

template <typename T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com() = default;
    T** put() { return &p; }
    T* operator->() const { return p; }
};

std::wstring hexError(HRESULT hr);
bool activateProcessClient(uint32_t pid, IAudioClient** client, std::wstring& error) {
    if (!processLoopbackSupported()) {
        error = L"当前 Windows 版本不支持指定程序声音（需 build 20348+）";
        return false;
    }
    bool expected = false;
    if (!activationOutstanding.compare_exchange_strong(expected, true)) {
        error = L"上一次程序回环激活仍未完成";
        return false;
    }
    static HMODULE mmdevapi = LoadLibraryExW(L"Mmdevapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto activate = mmdevapi ? reinterpret_cast<ActivateAsync>(
        GetProcAddress(mmdevapi, "ActivateAudioInterfaceAsync")) : nullptr;
    if (!activate) {
        activationOutstanding.store(false);
        error = L"系统 Mmdevapi.dll 缺少 ActivateAudioInterfaceAsync";
        return false;
    }
    auto* handler = new (std::nothrow) ActivationHandler();
    if (!handler || !handler->event()) {
        if (handler) handler->Release();
        activationOutstanding.store(false);
        error = L"创建程序回环完成事件失败";
        return false;
    }
    AudioClientActivationParams params{1, {pid, 0}};
    PROPVARIANT prop{};
    prop.vt = VT_BLOB;
    prop.blob.cbSize = sizeof(params);
    prop.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    Com<IActivateAudioInterfaceAsyncOperation> operation;
    handler->AddRef(); // Only ActivateCompleted releases this reference.
    HRESULT hr = activate(L"VAD\\Process_Loopback", __uuidof(IAudioClient), &prop,
                          handler, operation.put());
    if (FAILED(hr)) {
        activationOutstanding.store(false);
        handler->Release(); // No completion is scheduled on synchronous failure.
        handler->Release();
        error = L"启动程序回环激活失败: " + hexError(hr);
        return false;
    }
    if (!operation.p) {
        error = L"程序回环激活未返回异步操作";
        handler->Release(); // The completion reference remains until callback.
        return false;
    }
    DWORD wait = WaitForSingleObject(handler->event(), 5000);
    if (wait != WAIT_OBJECT_0) {
        error = wait == WAIT_TIMEOUT ? L"程序回环激活超时（5 秒）" :
                L"等待程序回环激活失败: " + hexError(HRESULT_FROM_WIN32(GetLastError()));
        handler->Release(); // Completion may still arrive; it owns the other ref.
        return false;
    }
    Com<IUnknown> activated;
    HRESULT result = E_UNEXPECTED;
    hr = operation->GetActivateResult(&result, activated.put());
    if (SUCCEEDED(hr)) hr = result;
    if (SUCCEEDED(hr) && activated.p)
        hr = activated->QueryInterface(__uuidof(IAudioClient),
                                       reinterpret_cast<void**>(client));
    else if (SUCCEEDED(hr)) hr = E_NOINTERFACE;
    handler->Release();
    if (FAILED(hr)) {
        error = L"程序回环激活失败: " + hexError(hr);
        return false;
    }
    return true;
}

std::wstring hexError(HRESULT hr) {
    wchar_t buf[32];
    swprintf(buf, 32, L"0x%08X", static_cast<unsigned>(hr));
    return buf;
}
std::wstring ioError(const std::wstring& what) {
    return what + L" (errno " + std::to_wstring(errno) + L")";
}
void le16(uint8_t* p, uint16_t n) {
    p[0] = static_cast<uint8_t>(n);
    p[1] = static_cast<uint8_t>(n >> 8);
}
void le32(uint8_t* p, uint32_t n) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(n >> (8 * i));
}
uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
std::array<uint8_t, 44> header(uint32_t dataBytes) {
    std::array<uint8_t, 44> h{};
    memcpy(h.data(), "RIFF", 4);
    le32(h.data() + 4, 36 + dataBytes);
    memcpy(h.data() + 8, "WAVEfmt ", 8);
    le32(h.data() + 16, 16);
    le16(h.data() + 20, 1);
    le16(h.data() + 22, 1);
    le32(h.data() + 24, rate);
    le32(h.data() + 28, rate * 2);
    le16(h.data() + 32, 2);
    le16(h.data() + 34, 16);
    memcpy(h.data() + 36, "data", 4);
    le32(h.data() + 40, dataBytes);
    return h;
}

class Wave {
public:
    ~Wave() {
        if (file_) {
            std::wstring ignored;
            close(ignored);
        }
    }
    bool open(const std::wstring& path, std::wstring& error) {
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            error = L"新建 WAV 失败: " + path + L" (" +
                    hexError(HRESULT_FROM_WIN32(GetLastError())) + L")";
            return false;
        }
        int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
        if (fd < 0) {
            CloseHandle(handle);
        } else {
            file_ = _wfdopen(fd, L"wb+");
            if (!file_) _close(fd);
        }
        if (!file_) {
            error = ioError(L"打开新 WAV 失败: " + path);
            DeleteFileW(path.c_str());
            return false;
        }
        path_ = path;
        return patch(error);
    }
    bool append(const int16_t* samples, size_t count, std::wstring& error) {
        if (count > (maxDataBytes - bytes_) / 2) {
            error = L"WAV 4 GiB RIFF 大小上限: " + path_;
            return false;
        }
        if (count && fwrite(samples, sizeof(int16_t), count, file_) != count) {
            error = ioError(L"写入 WAV 失败: " + path_);
            return false;
        }
        bytes_ += count * 2;
        if (bytes_ / (rate * 2) != checkpoint_) {
            checkpoint_ = bytes_ / (rate * 2);
            return patch(error);
        }
        return true;
    }
    bool close(std::wstring& error) {
        if (!file_) return true;
        bool ok = patch(error);
        if (fclose(file_) != 0 && ok) {
            error = ioError(L"关闭 WAV 失败: " + path_);
            ok = false;
        }
        file_ = nullptr;
        return ok;
    }
    uint64_t frames() const { return bytes_ / 2; }
private:
    bool patch(std::wstring& error) {
        if (_fseeki64(file_, 0, SEEK_SET) != 0) {
            error = ioError(L"定位 WAV 头失败: " + path_); return false;
        }
        auto h = header(static_cast<uint32_t>(bytes_));
        if (fwrite(h.data(), 1, h.size(), file_) != h.size() ||
            fflush(file_) != 0 || _fseeki64(file_, 0, SEEK_END) != 0) {
            error = ioError(L"更新 WAV 头失败: " + path_); return false;
        }
        return true;
    }
    FILE* file_ = nullptr;
    std::wstring path_;
    uint64_t bytes_ = 0;
    uint64_t checkpoint_ = 0;
};

class Dispatcher {
public:
    explicit Dispatcher(std::function<void(const Chunk&)> cb) : callback_(std::move(cb)) {
        if (callback_) thread_ = std::thread([this] { run(); });
    }
    ~Dispatcher() {
        finish();
    }
    void finish() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            done_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    void post(Chunk chunk) {
        if (!callback_) return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (pending_.size() == 64) {
                dropped_ = true; // WAV chunks remain on disk for later processing.
                return;
            }
            pending_.push(std::move(chunk));
        }
        cv_.notify_one();
    }
    bool dropped() const { std::lock_guard<std::mutex> lock(mu_); return dropped_; }
private:
    void run() {
        for (;;) {
            Chunk chunk;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return done_ || !pending_.empty(); });
                if (pending_.empty()) return;
                chunk = std::move(pending_.front());
                pending_.pop();
            }
            try { callback_(chunk); }
            catch (...) { std::lock_guard<std::mutex> lock(mu_); dropped_ = true; }
        }
    }
    std::function<void(const Chunk&)> callback_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::queue<Chunk> pending_;
    std::thread thread_;
    bool done_ = false, dropped_ = false;
};

class Sink {
public:
    Sink(std::wstring folder, std::wstring source, Dispatcher& dispatcher)
        : folder_(std::move(folder)), source_(std::move(source)), dispatcher_(dispatcher) {}
    bool open(std::wstring& error) {
        return raw_.open((std::filesystem::path(folder_) / (source_ + L".wav")).wstring(), error);
    }
    bool append(const int16_t* data, size_t count, std::wstring& error) {
        while (count) {
            if (!part_) {
                std::wstring name = source_ + L"_" + std::to_wstring(cursor_ / chunkFrames + 1) + L".wav";
                part_ = std::make_unique<Wave>();
                partPath_ = (std::filesystem::path(folder_) / L"chunks" / name).wstring();
                if (!part_->open(partPath_, error)) { part_.reset(); return false; }
            }
            size_t n = std::min<uint64_t>(count, chunkFrames - cursor_ % chunkFrames);
            if (!raw_.append(data, n, error) || !part_->append(data, n, error)) return false;
            cursor_ += n;
            data += n;
            count -= n;
            if (cursor_ % chunkFrames == 0 && !finishPart(error)) return false;
        }
        return true;
    }
    bool padTo(uint64_t target, std::wstring& error) {
        static const std::array<int16_t, 4096> zeros{};
        while (cursor_ < target) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(zeros.size(), target - cursor_));
            if (!append(zeros.data(), n, error)) return false;
        }
        return true;
    }
    bool close(std::wstring& error) {
        bool ok = true;
        std::wstring local;
        if (part_ && !finishPart(local)) { error = local; ok = false; }
        if (!raw_.close(local)) { if (ok) error = local; ok = false; }
        return ok;
    }
    uint64_t cursor() const { return cursor_; }
private:
    bool finishPart(std::wstring& error) {
        if (!part_->close(error)) { part_.reset(); return false; }
        dispatcher_.post({partPath_, source_, double(cursor_ - part_->frames()) / rate,
                          double(part_->frames()) / rate});
        part_.reset();
        return true;
    }
    std::wstring folder_, source_, partPath_;
    Dispatcher& dispatcher_;
    Wave raw_;
    std::unique_ptr<Wave> part_;
    uint64_t cursor_ = 0;
};

struct Endpoint {
    Com<IMMDevice> device;
    Com<IAudioClient> client;
    Com<IAudioCaptureClient> capture;
    WAVEFORMATEX* format = nullptr;
    bool floating = false;
    bool started = false;
    ~Endpoint() {
        if (client.p && started) client->Stop();
        if (format) CoTaskMemFree(format);
    }
    bool open(IMMDeviceEnumerator* devices, EDataFlow flow, std::wstring& error) {
        HRESULT hr = devices->GetDefaultAudioEndpoint(flow, eConsole, device.put());
        if (FAILED(hr)) { error = L"获取默认音频设备失败: " + hexError(hr); return false; }
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client.put()));
        if (FAILED(hr)) { error = L"打开音频设备失败: " + hexError(hr); return false; }
        hr = client->GetMixFormat(&format);
        if (FAILED(hr)) { error = L"读取设备格式失败: " + hexError(hr); return false; }
        WORD tag = format->wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE) {
            if (format->cbSize < 22) { error = L"设备 extensible 格式长度无效"; return false; }
            auto* ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format);
            if (IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
                tag = WAVE_FORMAT_IEEE_FLOAT;
            else if (IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
                tag = WAVE_FORMAT_PCM;
        }
        floating = tag == WAVE_FORMAT_IEEE_FLOAT;
        const WORD bits = format->wBitsPerSample;
        if ((!floating && tag != WAVE_FORMAT_PCM) ||
            (floating && bits != 32 && bits != 64) ||
            (!floating && bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
            !format->nChannels || !format->nSamplesPerSec ||
            format->nBlockAlign < format->nChannels * (bits / 8)) {
            error = L"不支持的设备音频格式"; return false;
        }
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                flow == eRender ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                                0, 0, format, nullptr);
        if (FAILED(hr)) { error = L"WASAPI 初始化失败: " + hexError(hr); return false; }
        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                reinterpret_cast<void**>(capture.put()));
        if (FAILED(hr)) { error = L"取得采集服务失败: " + hexError(hr); return false; }
        return true;
    }
    bool openProcess(uint32_t pid, std::wstring& error) {
        if (!activateProcessClient(pid, client.put(), error)) return false;
        format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        if (!format) { error = L"分配程序回环格式失败"; return false; }
        *format = {};
        format->wFormatTag = WAVE_FORMAT_PCM;
        format->nChannels = 2;
        format->nSamplesPerSec = 48000;
        format->wBitsPerSample = 16;
        format->nBlockAlign = 4;
        format->nAvgBytesPerSec = 48000 * 4;
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
            0, 0, format, nullptr);
        if (FAILED(hr)) {
            error = L"初始化程序回环失败: " + hexError(hr);
            return false;
        }
        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                reinterpret_cast<void**>(capture.put()));
        if (FAILED(hr)) {
            error = L"取得程序回环采集服务失败: " + hexError(hr);
            return false;
        }
        return true;
    }
    float sample(const BYTE* frame) const {
        const int bytes = format->wBitsPerSample / 8;
        double sum = 0;
        for (WORD ch = 0; ch < format->nChannels; ++ch) {
            const BYTE* p = frame + ch * bytes;
            double x = 0;
            if (floating && bytes == 4) {
                float v; memcpy(&v, p, 4); x = v;
            } else if (floating && bytes == 8) {
                double v; memcpy(&v, p, 8); x = v;
            } else if (bytes == 1) x = (int(p[0]) - 128) / 128.0;
            else if (bytes == 2) x = int16_t(get16(p)) / 32768.0;
            else if (bytes == 3) {
                int32_t v = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);
                if (v & 0x800000) v |= ~0xffffff;
                x = v / 8388608.0;
            } else {
                int32_t v = static_cast<int32_t>(get32(p));
                x = v / 2147483648.0;
            }
            sum += std::isfinite(x) ? x : 0.0;
        }
        return static_cast<float>(std::clamp(sum / format->nChannels, -1.0, 1.0));
    }
};

double qpcSeconds() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return double(now.QuadPart) / double(freq.QuadPart);
}
int16_t toPcm(double x) {
    return static_cast<int16_t>(std::clamp(std::lround(x * 32768.0), -32768L, 32767L));
}
bool drain(Endpoint& ep, Sink& sink, double origin, float& level, std::wstring& error) {
    UINT32 available = 0;
    HRESULT hr = ep.capture->GetNextPacketSize(&available);
    if (FAILED(hr)) { error = L"WASAPI 查询缓冲失败: " + hexError(hr); return false; }
    while (available) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 devicePosition = 0, packetQpc = 0;
        hr = ep.capture->GetBuffer(&data, &frames, &flags, &devicePosition, &packetQpc);
        if (FAILED(hr)) { error = L"WASAPI 读取缓冲失败: " + hexError(hr); return false; }
        bool ok = true;
        if (frames > 1000000 || (frames && !(flags & AUDCLNT_BUFFERFLAGS_SILENT) && !data)) {
            error = L"WASAPI 缓冲长度或地址无效"; ok = false;
        } else if (frames) {
            double duration = double(frames) / ep.format->nSamplesPerSec;
            double begin = double(packetQpc) / 10000000.0 - origin;
            double elapsed = qpcSeconds() - origin;
            if (packetQpc == 0 || begin < -1 || begin > elapsed + 1)
                begin = elapsed - duration;
            uint64_t first = static_cast<uint64_t>(std::max(0.0, std::ceil(begin * rate - 0.00001)));
            uint64_t last = static_cast<uint64_t>(
                std::max(0.0, std::ceil((begin + duration) * rate - 0.00001)));
            if (last > first + rate * 70ULL) {
                error = L"WASAPI 时间戳异常"; ok = false;
            } else {
                ok = sink.padTo(first, error);
                std::array<int16_t, 4096> out{};
                size_t used = 0;
                float peak = 0;
                for (uint64_t at = std::max(first, sink.cursor()); ok && at < last; ++at) {
                    double pos = (double(at) / rate - begin) * ep.format->nSamplesPerSec;
                    auto i = static_cast<UINT32>(std::clamp(pos, 0.0, double(frames - 1)));
                    double frac = std::clamp(pos - i, 0.0, 1.0);
                    double x = 0;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        x = ep.sample(data + size_t(i) * ep.format->nBlockAlign);
                        if (i + 1 < frames)
                            x += frac * (ep.sample(data + size_t(i + 1) * ep.format->nBlockAlign) - x);
                    }
                    peak = std::max(peak, static_cast<float>(std::abs(x)));
                    out[used++] = toPcm(x);
                    if (used == out.size()) { ok = sink.append(out.data(), used, error); used = 0; }
                }
                if (ok && used) ok = sink.append(out.data(), used, error);
                level = peak;
            }
        }
        HRESULT release = ep.capture->ReleaseBuffer(frames);
        if (!ok) return false;
        if (FAILED(release)) { error = L"WASAPI 释放缓冲失败: " + hexError(release); return false; }
        hr = ep.capture->GetNextPacketSize(&available);
        if (FAILED(hr)) { error = L"WASAPI 查询缓冲失败: " + hexError(hr); return false; }
    }
    return true;
}

bool readSource(const std::wstring& path, FILE*& file, uint64_t& frames, std::wstring& error) {
    file = _wfopen(path.c_str(), L"rb");
    if (!file) { error = ioError(L"打开原始 WAV 失败: " + path); return false; }
    uint8_t h[44];
    if (fread(h, 1, 44, file) != 44 ||
        memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVEfmt ", 8) ||
        get32(h + 16) != 16 || get16(h + 20) != 1 || get16(h + 22) != 1 ||
        get32(h + 24) != rate || get16(h + 32) != 2 || get16(h + 34) != 16 ||
        memcmp(h + 36, "data", 4) || get32(h + 4) != get32(h + 40) + 36U ||
        get32(h + 40) % 2) {
        error = L"原始 WAV 格式无效: " + path; fclose(file); file = nullptr; return false;
    }
    frames = get32(h + 40) / 2;
    return true;
}
} // namespace

bool mixWaveFiles(const std::wstring& folder, std::wstring& error) {
    error.clear();
    const auto base = std::filesystem::path(folder);
    FILE *mic = nullptr, *system = nullptr;
    uint64_t a = 0, b = 0;
    if (!readSource((base / L"microphone.wav").wstring(), mic, a, error)) return false;
    if (!readSource((base / L"system.wav").wstring(), system, b, error)) {
        fclose(mic); return false;
    }
    const auto temp = (base / L"mix.wav.tmp").wstring();
    Wave output;
    bool ok = output.open(temp, error);
    bool created = ok;
    std::array<int16_t, 4096> m{}, s{}, result{};
    uint64_t cursor = 0, total = std::max(a, b);
    while (ok && cursor < total) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(result.size(), total - cursor));
        size_t nm = static_cast<size_t>(std::min<uint64_t>(n, a > cursor ? a - cursor : 0));
        size_t ns = static_cast<size_t>(std::min<uint64_t>(n, b > cursor ? b - cursor : 0));
        if (fread(m.data(), sizeof(int16_t), nm, mic) != nm ||
            fread(s.data(), sizeof(int16_t), ns, system) != ns) {
            error = L"读取原始 WAV 失败或数据截断"; ok = false; break;
        }
        for (size_t i = 0; i < n; ++i)
            result[i] = static_cast<int16_t>(std::clamp(
                (i < nm ? int(m[i]) : 0) + (i < ns ? int(s[i]) : 0), -32768, 32767));
        ok = output.append(result.data(), n, error);
        cursor += n;
    }
    fclose(mic);
    fclose(system);
    std::wstring closing;
    if (!output.close(closing)) { if (ok) error = closing; ok = false; }
    if (ok && !MoveFileExW(temp.c_str(), (base / L"mix.wav").c_str(),
                           MOVEFILE_WRITE_THROUGH)) {
        error = L"完成混音 WAV 失败: " + hexError(HRESULT_FROM_WIN32(GetLastError()));
        ok = false;
    }
    if (!ok && created) DeleteFileW(temp.c_str());
    return ok;
}

struct Recorder::Impl {
    std::mutex lifecycle, meta;
    std::thread worker;
    std::atomic<CaptureState> state{CaptureState::Idle};
    std::atomic<bool> stopRequested{false};
    std::atomic<double> seconds{0};
    std::atomic<float> micPeak{0}, systemPeak{0};
    std::wstring path, issue;

    void run(std::wstring folder, std::function<void(const Chunk&)> callback,
             uint32_t processId) {
        std::wstring error;
        bool callbackOnlyIssue = false;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        struct CoGuard {
            bool initialized;
            ~CoGuard() { if (initialized) CoUninitialize(); }
        } com{SUCCEEDED(hr)};
        if (FAILED(hr)) {
            error = L"录音线程 COM 初始化失败: " + hexError(hr);
        } else {
            {
                Com<IMMDeviceEnumerator> devices;
                Endpoint mic, system;
                struct ProcessHandle {
                    HANDLE value = nullptr;
                    ~ProcessHandle() { if (value) CloseHandle(value); }
                } selected;
                if (processId && !processLoopbackSupported())
                    error = L"当前 Windows 版本不支持指定程序声音（需 build 20348+）";
                if (processId && error.empty()) {
                    selected.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                                 FALSE, processId);
                    if (!selected.value) error = L"无法访问目标进程: " +
                        hexError(HRESULT_FROM_WIN32(GetLastError()));
                    else if (WaitForSingleObject(selected.value, 0) == WAIT_OBJECT_0)
                        error = L"目标进程已退出";
                }
                if (error.empty()) {
                    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          __uuidof(IMMDeviceEnumerator),
                                          reinterpret_cast<void**>(devices.put()));
                    if (FAILED(hr)) error = L"创建音频设备枚举器失败: " + hexError(hr);
                }
                if (error.empty() && !mic.open(devices.p, eCapture, error)) {}
                if (error.empty() && processId &&
                    !system.openProcess(processId, error)) {}
                if (error.empty() && !processId &&
                    !system.open(devices.p, eRender, error)) {}
                if (!error.empty()) {}
                else if (stopRequested.load()) {}
                else {
                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::path(folder) / L"chunks", ec);
                    if (ec) error = L"创建录音目录失败: " + std::to_wstring(ec.value());
                    else {
                        Dispatcher dispatcher(std::move(callback));
                        Sink micSink(folder, L"microphone", dispatcher);
                        Sink systemSink(folder, L"system", dispatcher);
                        if (micSink.open(error) && systemSink.open(error)) {
                            double origin = qpcSeconds();
                            hr = stopRequested.load() ? S_FALSE : mic.client->Start();
                            if (FAILED(hr)) error = L"启动麦克风失败: " + hexError(hr);
                            else if (hr == S_OK) {
                                mic.started = true;
                                hr = system.client->Start();
                                if (FAILED(hr)) error = L"启动回环采集失败: " + hexError(hr);
                                else {
                                    system.started = true;
                                    CaptureState expected = CaptureState::Starting;
                                    state.compare_exchange_strong(expected, CaptureState::Recording);
                                    while (error.empty() && !stopRequested.load()) {
                                        if (selected.value &&
                                            WaitForSingleObject(selected.value, 0) == WAIT_OBJECT_0) {
                                            error = L"目标进程已退出；已保存此前录音";
                                            break;
                                        }
                                        float ml = 0, sl = 0;
                                        if (!drain(mic, micSink, origin, ml, error) ||
                                            !drain(system, systemSink, origin, sl, error)) break;
                                        micPeak.store(ml);
                                        systemPeak.store(sl);
                                        double elapsed = std::max(0.0, qpcSeconds() - origin);
                                        seconds.store(elapsed);
                                        // Leave room for packets delivered late by the audio engine.
                                        uint64_t watermark = static_cast<uint64_t>(
                                            std::max(0.0, elapsed - 0.3) * rate);
                                        if (!micSink.padTo(watermark, error) ||
                                            !systemSink.padTo(watermark, error)) break;
                                        Sleep(10);
                                    }
                                    if (error.empty()) {
                                        float ml = 0, sl = 0;
                                        drain(mic, micSink, origin, ml, error);
                                        if (error.empty()) drain(system, systemSink, origin, sl, error);
                                    }
                                    double elapsed = std::max(0.0, qpcSeconds() - origin);
                                    seconds.store(elapsed);
                                    if (error.empty()) {
                                        uint64_t tail = static_cast<uint64_t>(std::ceil(elapsed * rate));
                                        if (!micSink.padTo(tail, error)) {}
                                        else systemSink.padTo(tail, error);
                                    }
                                }
                            }
                            if (system.started) {
                                hr = system.client->Stop();
                                if (FAILED(hr) && error.empty())
                                    error = L"停止回环采集失败: " + hexError(hr);
                                if (SUCCEEDED(hr)) system.started = false;
                            }
                            if (mic.started) {
                                hr = mic.client->Stop();
                                if (FAILED(hr) && error.empty())
                                    error = L"停止麦克风失败: " + hexError(hr);
                                if (SUCCEEDED(hr)) mic.started = false;
                            }
                        }
                        std::wstring closing;
                        if (!micSink.close(closing) && error.empty()) error = closing;
                        if (!systemSink.close(closing) && error.empty()) error = closing;
                        if (error.empty() && !mixWaveFiles(folder, error)) {}
                        dispatcher.finish();
                        if (error.empty() && dispatcher.dropped()) {
                            error = L"部分分段回调失败或队列已满；WAV 分段已保存在磁盘";
                            callbackOnlyIssue = true;
                        }
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(meta);
            issue = error;
        }
        micPeak.store(0);
        systemPeak.store(0);
        state.store(error.empty() || callbackOnlyIssue ? CaptureState::Saved : CaptureState::Failed);
    }
};

Recorder::Recorder() : impl_(new Impl) {}
Recorder::~Recorder() {
    impl_->stopRequested.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
    delete impl_;
}
bool Recorder::start(const std::wstring& folder, std::function<void(const Chunk&)> onChunk,
                     uint32_t processId) {
    std::lock_guard<std::mutex> lock(impl_->lifecycle);
    auto current = impl_->state.load();
    if (current == CaptureState::Starting || current == CaptureState::Recording ||
        current == CaptureState::Stopping) return false;
    if (impl_->worker.joinable()) impl_->worker.join();
    {
        std::lock_guard<std::mutex> meta(impl_->meta);
        impl_->path = folder;
        impl_->issue.clear();
    }
    impl_->seconds.store(0);
    impl_->micPeak.store(0);
    impl_->systemPeak.store(0);
    impl_->stopRequested.store(false);
    if (folder.empty()) {
        std::lock_guard<std::mutex> meta(impl_->meta);
        impl_->issue = L"录音目录为空";
        impl_->state.store(CaptureState::Failed);
        return false;
    }
    impl_->state.store(CaptureState::Starting);
    try {
        impl_->worker = std::thread([p = impl_, folder, cb = std::move(onChunk), processId]() mutable {
            try { p->run(folder, std::move(cb), processId); }
            catch (const std::exception& ex) {
                std::lock_guard<std::mutex> meta(p->meta);
                p->issue = L"录音线程异常: " + std::wstring(ex.what(), ex.what() + strlen(ex.what()));
                p->state.store(CaptureState::Failed);
            }
            catch (...) {
                std::lock_guard<std::mutex> meta(p->meta);
                p->issue = L"录音线程未知异常";
                p->state.store(CaptureState::Failed);
            }
        });
    } catch (...) {
        std::lock_guard<std::mutex> meta(impl_->meta);
        impl_->issue = L"无法启动录音线程";
        impl_->state.store(CaptureState::Failed);
        return false;
    }
    return true;
}
void Recorder::stop() {
    impl_->stopRequested.store(true);
    auto current = impl_->state.load();
    while ((current == CaptureState::Starting || current == CaptureState::Recording) &&
           !impl_->state.compare_exchange_weak(current, CaptureState::Stopping)) {}
}
bool Recorder::finished() const {
    auto current = impl_->state.load();
    return current == CaptureState::Idle || current == CaptureState::Saved ||
           current == CaptureState::Failed;
}
CaptureState Recorder::state() const { return impl_->state.load(); }
std::wstring Recorder::error() const {
    std::lock_guard<std::mutex> lock(impl_->meta);
    return impl_->issue;
}
std::wstring Recorder::folder() const {
    std::lock_guard<std::mutex> lock(impl_->meta);
    return impl_->path;
}
double Recorder::elapsed() const { return impl_->seconds.load(); }
float Recorder::micLevel() const { return impl_->micPeak.load(); }
float Recorder::systemLevel() const { return impl_->systemPeak.load(); }

bool processLoopbackSupported() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    using RtlGetVersionFn = LONG (WINAPI*)(OSVERSIONINFOW*);
    auto versionFn = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (!versionFn) return false;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return versionFn(&version) == 0 && version.dwBuildNumber >= 20348;
}

bool audioSelfTest(const std::wstring& folder, std::wstring& error) {
    error.clear();
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) { error = L"创建合成测试目录失败: " + std::to_wstring(ec.value()); return false; }
    std::filesystem::path base;
    bool made = false;
    for (int i = 0; i < 16 && !made; ++i) {
        base = std::filesystem::path(folder) /
            (L"audio-self-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(i));
        made = std::filesystem::create_directory(base, ec);
        if (ec && ec != std::errc::file_exists) break;
        ec.clear();
    }
    if (!made) { error = L"无法新建独立合成测试目录"; return false; }
    auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove(base / L"microphone.wav", ignored);
        std::filesystem::remove(base / L"system.wav", ignored);
        std::filesystem::remove(base / L"mix.wav", ignored);
        std::filesystem::remove(base / L"mix.wav.tmp", ignored);
        std::filesystem::remove(base, ignored);
    };
    const int16_t mic[] = {100, 32767, -32768, 7, 9};
    const int16_t system[] = {200, 2, -2};
    Wave a, b;
    if (!a.open((base / L"microphone.wav").wstring(), error) ||
        !a.append(mic, 5, error) || !a.close(error) ||
        !b.open((base / L"system.wav").wstring(), error) ||
        !b.append(system, 3, error) || !b.close(error) ||
        !mixWaveFiles(base.wstring(), error)) { cleanup(); return false; }
    FILE* f = nullptr;
    uint64_t frames = 0;
    if (!readSource((base / L"mix.wav").wstring(), f, frames, error)) {
        cleanup(); return false;
    }
    int16_t actual[5]{};
    bool ok = frames == 5 && fread(actual, sizeof(int16_t), 5, f) == 5 &&
        actual[0] == 300 && actual[1] == 32767 && actual[2] == -32768 &&
        actual[3] == 7 && actual[4] == 9;
    fclose(f);
    if (!ok) error = L"合成混音长度、短轨尾部或饱和边界校验失败";
    cleanup();
    return ok;
}
} // namespace lightrec
