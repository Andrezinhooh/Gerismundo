#include "nativecall/audio.hpp"
#include "audio_codec.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <objidl.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <opus/opus.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <utility>

namespace nativecall {
namespace {

using Microsoft::WRL::ComPtr;
using namespace audio_detail;
using Mono = std::array<spx_int16_t, kFrames>;

void check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    std::ostringstream text;
    text << operation << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << ')';
    throw std::runtime_error(text.str());
}

class ComApartment {
    HRESULT result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
public:
    ComApartment() {
        if (result_ != RPC_E_CHANGED_MODE) check(result_, "CoInitializeEx");
    }
    ~ComApartment() { if (SUCCEEDED(result_)) CoUninitialize(); }
};

class Event final {
    HANDLE value_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
public:
    Event() {
        if (!value_) throw std::runtime_error("CreateEventW falhou");
    }
    ~Event() { CloseHandle(value_); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    HANDLE get() const noexcept { return value_; }
};

class AudioPriority final {
    DWORD index_{};
    HANDLE handle_ = AvSetMmThreadCharacteristicsW(L"Audio", &index_);
public:
    AudioPriority() { if (handle_) AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH); }
    ~AudioPriority() { if (handle_) AvRevertMmThreadCharacteristics(handle_); }
};

WAVEFORMATEX format() {
    WAVEFORMATEX value{};
    value.wFormatTag = WAVE_FORMAT_PCM;
    value.nChannels = kChannels;
    value.nSamplesPerSec = kRate;
    value.wBitsPerSample = 16;
    value.nBlockAlign = kChannels * sizeof(opus_int16);
    value.nAvgBytesPerSec = value.nSamplesPerSec * value.nBlockAlign;
    return value;
}

ComPtr<IMMDevice> endpoint(const std::wstring& id, bool input) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           IID_PPV_ARGS(&enumerator)), "MMDeviceEnumerator");
    ComPtr<IMMDevice> device;
    if (id.empty()) {
        check(enumerator->GetDefaultAudioEndpoint(input ? eCapture : eRender,
              eCommunications, &device), "Dispositivo de audio padrao");
    } else {
        check(enumerator->GetDevice(id.c_str(), &device), "Dispositivo de audio selecionado");
    }
    return device;
}

struct Stream final {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioRenderClient> render;
    Event event;
    UINT32 capacity{};
    ~Stream() { if (client) client->Stop(); }

    void initialize(bool input, bool loopback) {
        auto wave = format();
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (loopback) {
            // The virtual process-loopback device converts to our wire PCM
            // format. It must never be replaced with the default output device.
            flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        }
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
              0, 0, &wave, nullptr), "IAudioClient::Initialize");
        check(client->SetEventHandle(event.get()), "IAudioClient::SetEventHandle");
        check(client->GetBufferSize(&capacity), "IAudioClient::GetBufferSize");
        if (input) {
            check(client->GetService(IID_PPV_ARGS(&capture)), "IAudioCaptureClient");
        } else {
            check(client->GetService(IID_PPV_ARGS(&render)), "IAudioRenderClient");
            BYTE* initial{};
            check(render->GetBuffer(capacity, &initial), "Prefill do dispositivo de audio");
            check(render->ReleaseBuffer(capacity, AUDCLNT_BUFFERFLAGS_SILENT), "Prefill silencioso");
        }
        check(client->Start(), "IAudioClient::Start");
    }
};

struct ActivationState final {
    Event completed;
    std::mutex mutex;
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    PROPVARIANT property{};
};

// The callback is genuinely agile: its only shared state is protected by a
// mutex/event, so COM can invoke it from the audio activation MTA directly.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
    std::atomic<ULONG> references_{1};
    std::shared_ptr<ActivationState> state_;
public:
    explicit ActivationHandler(std::shared_ptr<ActivationState> state) : state_(std::move(state)) {}
    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, __uuidof(IUnknown)) ||
            IsEqualIID(iid, __uuidof(IActivateAudioInterfaceCompletionHandler))) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (IsEqualIID(iid, __uuidof(IAgileObject))) {
            *object = static_cast<IAgileObject*>(this);
        }
        if (!*object) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activation = E_FAIL;
        ComPtr<IUnknown> object;
        const HRESULT query = operation->GetActivateResult(&activation, &object);
        {
            std::lock_guard lock(state_->mutex);
            state_->result = FAILED(query) ? query : activation;
            if (SUCCEEDED(state_->result)) state_->result = object.As(&state_->client);
        }
        SetEvent(state_->completed.get());
        return S_OK;
    }
};

ComPtr<IAudioClient> activateLoopback(const CaptureSource& source, std::stop_token stop) {
    auto state = std::make_shared<ActivationState>();
    state->parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    state->parameters.ProcessLoopbackParams.TargetProcessId = source.kind == SourceKind::Monitor
        ? GetCurrentProcessId() : source.processId;
    state->parameters.ProcessLoopbackParams.ProcessLoopbackMode = source.kind == SourceKind::Monitor
        ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
        : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    state->property.vt = VT_BLOB;
    state->property.blob.cbSize = sizeof(state->parameters);
    state->property.blob.pBlobData = reinterpret_cast<BYTE*>(&state->parameters);
    ComPtr<ActivationHandler> handler;
    handler.Attach(new ActivationHandler(state));
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    check(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
          __uuidof(IAudioClient), &state->property, handler.Get(), &operation),
          "ActivateAudioInterfaceAsync (audio seletivo por processo)");
    for (int attempt = 0; attempt != 100; ++attempt) {
        if (stop.stop_requested()) throw std::runtime_error("Ativacao de audio interrompida");
        const auto wait = WaitForSingleObject(state->completed.get(), 100);
        if (wait == WAIT_OBJECT_0) {
            std::lock_guard lock(state->mutex);
            check(state->result, "Ativacao do Application Loopback");
            return state->client;
        }
        if (wait == WAIT_FAILED) throw std::runtime_error("Falha aguardando ativacao de audio");
    }
    // The completion handler retains the parameters and event if Windows is
    // still activating after this bounded timeout.
    throw std::runtime_error("Tempo limite ao ativar Application Loopback");
}

bool audible(const Stereo& samples) {
    double energy{};
    for (const auto sample : samples) energy += static_cast<double>(sample) * sample;
    return energy / samples.size() > 200.0 * 200.0;
}

std::int64_t performanceClockOffsetUs() {
    LARGE_INTEGER ticks{}, frequency{};
    if (!QueryPerformanceCounter(&ticks) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        throw std::runtime_error("QueryPerformanceCounter indisponivel para sincronizacao de audio");
    const auto tickUs = (ticks.QuadPart / frequency.QuadPart) * 1000000 +
                       (ticks.QuadPart % frequency.QuadPart) * 1000000 / frequency.QuadPart;
    return static_cast<std::int64_t>(mediaNowUs()) - tickUs;
}

} // namespace

struct AudioEngine::Impl final {
    Callbacks callbacks;
    std::jthread microphone, output, sharing;
    std::atomic<bool> micMuted{}, deafened{}, shareMuted{}, running{};
    std::mutex remoteMutex;
    using Key = std::pair<std::string, AudioKind>;
    std::map<Key, std::unique_ptr<Remote>> remotes;
    std::mutex processingMutex;
    SpeexEchoState* echo{};
    SpeexPreprocessState* preprocess{};
    std::uint64_t localVoiceUntil{};
    bool localSpeaking{};

    explicit Impl(Callbacks value) : callbacks(std::move(value)) {
        echo = speex_echo_state_init(kFrames, kRate / 5);
        preprocess = speex_preprocess_state_init(kFrames, kRate);
        if (!echo || !preprocess) {
            if (echo) speex_echo_state_destroy(echo);
            if (preprocess) speex_preprocess_state_destroy(preprocess);
            throw std::runtime_error("Falha criando processamento de voz SpeexDSP");
        }
        int rate = kRate;
        speex_echo_ctl(echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
        int enabled = 1;
        int suppress = -20;
        speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_DENOISE, &enabled);
        speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppress);
        speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, echo);
    }

    ~Impl() {
        stop();
        speex_preprocess_state_destroy(preprocess);
        speex_echo_state_destroy(echo);
    }

    void report(AudioFailure failure, const std::string& message) noexcept {
        try { if (callbacks.onError) callbacks.onError(failure, message); } catch (...) {}
    }

    void activity(const std::string& peer, bool speaking) noexcept {
        try { if (callbacks.onActivity) callbacks.onActivity(peer, speaking); } catch (...) {}
    }

    void processMicrophone(Stereo& samples) {
        Mono input{}, clean{};
        for (int i = 0; i != kFrames; ++i) {
            input[i] = static_cast<spx_int16_t>((static_cast<int>(samples[i * 2]) + samples[i * 2 + 1]) / 2);
        }
        {
            std::lock_guard lock(processingMutex);
            speex_echo_capture(echo, input.data(), clean.data());
            speex_preprocess_run(preprocess, clean.data());
        }
        const bool muted = micMuted.load();
        for (int i = 0; i != kFrames; ++i) {
            samples[i * 2] = samples[i * 2 + 1] = muted ? 0 : clean[i];
        }
        const auto now = mediaNowUs();
        if (!muted && audible(samples)) localVoiceUntil = now + 180000;
        const bool speaking = !muted && now < localVoiceUntil;
        if (speaking != localSpeaking) {
            localSpeaking = speaking;
            activity("self", speaking);
        }
    }

    Stereo mix() {
        std::array<float, kFrames * kChannels> accumulated{};
        std::vector<std::pair<std::string, bool>> changed;
        {
            std::lock_guard lock(remoteMutex);
            const auto now = mediaNowUs();
            for (auto& [key, remote] : remotes) {
                Stereo decoded{};
                remote->decode(decoded);
                if (key.second == AudioKind::Microphone) {
                    if (audible(decoded)) remote->voiceUntil = now + 180000;
                    const bool speaking = now < remote->voiceUntil;
                    if (remote->speaking != speaking) {
                        remote->speaking = speaking;
                        changed.emplace_back(key.first, speaking);
                    }
                }
                for (std::size_t i = 0; i != accumulated.size(); ++i) {
                    accumulated[i] += decoded[i] * remote->volume;
                }
            }
        }
        for (const auto& [peer, speaking] : changed) activity(peer, speaking);
        Stereo mixed{};
        Mono reference{};
        const bool muted = deafened.load();
        for (std::size_t i = 0; i != mixed.size(); ++i) {
            mixed[i] = muted ? 0 : static_cast<opus_int16>(std::clamp(accumulated[i], -32768.0f, 32767.0f));
        }
        for (int i = 0; i != kFrames; ++i) {
            reference[i] = static_cast<spx_int16_t>((static_cast<int>(mixed[i * 2]) + mixed[i * 2 + 1]) / 2);
        }
        {
            std::lock_guard lock(processingMutex);
            speex_echo_playback(echo, reference.data());
        }
        return mixed;
    }

    void captureLoop(Stream& stream, AudioKind kind, std::stop_token stop) {
        Encoder encoder(kind);
        Stereo frame{};
        std::size_t offset{};
        auto pts = mediaNowUs();
        const auto clockOffsetUs = performanceClockOffsetUs();
        bool haveTimestamp{};
        while (!stop.stop_requested()) {
            const auto wait = WaitForSingleObject(stream.event.get(), 100);
            if (wait == WAIT_FAILED) throw std::runtime_error("Falha aguardando captura de audio");
            if (wait != WAIT_OBJECT_0) continue;
            UINT32 next{};
            check(stream.capture->GetNextPacketSize(&next), "GetNextPacketSize");
            while (next && !stop.stop_requested()) {
                BYTE* data{};
                UINT32 frames{};
                DWORD flags{};
                UINT64 qpcPosition{};
                check(stream.capture->GetBuffer(&data, &frames, &flags, nullptr, &qpcPosition), "Captura de audio");
                // A buffer must be released even if an encoder/network callback throws.
                struct Release final {
                    IAudioCaptureClient* client;
                    UINT32 count;
                    ~Release() { if (client) client->ReleaseBuffer(count); }
                } release{stream.capture.Get(), frames};
                // WASAPI reports the first captured sample's QPC time in 100 ns
                // units. Map it into the same steady timeline as WGC video.
                // Only reset on a real gap; packet-to-packet timestamps must keep
                // their exact 480-sample cadence for RTP and the jitter buffer.
                const auto captured = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
                    ? static_cast<std::int64_t>(mediaNowUs()) - static_cast<std::int64_t>(frames) * 1000000 / kRate
                    : static_cast<std::int64_t>(qpcPosition / 10) + clockOffsetUs;
                const auto expected = static_cast<std::int64_t>(pts) +
                                      static_cast<std::int64_t>(offset) * 1000000 / kRate;
                const bool clockGap = captured > expected + 30000 || captured + 30000 < expected;
                if (!haveTimestamp || clockGap || (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                    offset = 0;
                    pts = static_cast<std::uint64_t>(std::max<std::int64_t>(0, captured));
                    haveTimestamp = true;
                }
                const auto* samples = reinterpret_cast<const opus_int16*>(data);
                for (UINT32 index = 0; index != frames; ++index) {
                    for (int channel = 0; channel != kChannels; ++channel) {
                        frame[offset * kChannels + channel] = (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                            ? 0 : samples[index * kChannels + channel];
                    }
                    if (++offset == kFrames) {
                        if (kind == AudioKind::Microphone) processMicrophone(frame);
                        else if (shareMuted.load()) frame.fill(0);
                        if (callbacks.onPacket) callbacks.onPacket(kind, encoder.encode(frame), pts);
                        pts += kFrameUs;
                        offset = 0;
                    }
                }
                // Release before asking for the next buffer.
                release.client = nullptr;
                check(stream.capture->ReleaseBuffer(frames), "ReleaseBuffer da captura");
                check(stream.capture->GetNextPacketSize(&next), "GetNextPacketSize");
            }
        }
    }

    std::jthread inputWorker(std::wstring id, std::shared_ptr<std::promise<void>> startup) {
        return std::jthread([this, id = std::move(id), startup = std::move(startup)](std::stop_token stop) {
            bool ready{};
            try {
                ComApartment apartment;
                AudioPriority priority;
                Stream stream;
                auto device = endpoint(id, true);
                check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(stream.client.GetAddressOf())), "Ativacao do microfone");
                stream.initialize(true, false);
                startup->set_value(); ready = true;
                captureLoop(stream, AudioKind::Microphone, stop);
            } catch (...) {
                if (!ready) startup->set_exception(std::current_exception());
                else try { throw; } catch (const std::exception& error) { report(AudioFailure::Microphone, error.what()); }
                catch (...) { report(AudioFailure::Microphone, "Falha inesperada no microfone"); }
            }
            localSpeaking = false;
            activity("self", false);
        });
    }

    std::jthread outputWorker(std::wstring id, std::shared_ptr<std::promise<void>> startup) {
        return std::jthread([this, id = std::move(id), startup = std::move(startup)](std::stop_token stop) {
            bool ready{};
            try {
                ComApartment apartment;
                AudioPriority priority;
                Stream stream;
                auto device = endpoint(id, false);
                check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(stream.client.GetAddressOf())), "Ativacao da saida de audio");
                stream.initialize(false, false);
                startup->set_value(); ready = true;
                Stereo mixed{};
                std::size_t offset = kFrames;
                while (!stop.stop_requested()) {
                    const auto wait = WaitForSingleObject(stream.event.get(), 100);
                    if (wait == WAIT_FAILED) throw std::runtime_error("Falha aguardando reproducao de audio");
                    if (wait != WAIT_OBJECT_0) continue;
                    UINT32 padding{};
                    check(stream.client->GetCurrentPadding(&padding), "Padding de audio");
                    const UINT32 count = stream.capacity - std::min(stream.capacity, padding);
                    if (!count) continue;
                    BYTE* data{};
                    check(stream.render->GetBuffer(count, &data), "Buffer de reproducao");
                    auto* target = reinterpret_cast<opus_int16*>(data);
                    for (UINT32 index = 0; index != count; ++index) {
                        if (offset == kFrames) { mixed = mix(); offset = 0; }
                        target[index * 2] = mixed[offset * 2];
                        target[index * 2 + 1] = mixed[offset * 2 + 1];
                        ++offset;
                    }
                    check(stream.render->ReleaseBuffer(count, 0), "ReleaseBuffer da reproducao");
                }
            } catch (...) {
                if (!ready) startup->set_exception(std::current_exception());
                else try { throw; } catch (const std::exception& error) { report(AudioFailure::Output, error.what()); }
                catch (...) { report(AudioFailure::Output, "Falha inesperada na saida de audio"); }
            }
        });
    }

    void stopShare() noexcept {
        sharing.request_stop();
        if (sharing.joinable()) sharing.join();
    }

    void stop() noexcept {
        running.store(false);
        stopShare();
        microphone.request_stop();
        output.request_stop();
        if (microphone.joinable()) microphone.join();
        if (output.joinable()) output.join();
        std::lock_guard lock(remoteMutex);
        remotes.clear();
    }
};

AudioEngine::AudioEngine(Callbacks callbacks) : impl_(std::make_unique<Impl>(std::move(callbacks))) {}
AudioEngine::~AudioEngine() = default;

void AudioEngine::start(std::wstring input, std::wstring output) {
    impl_->stop();
    {
        std::lock_guard lock(impl_->processingMutex);
        speex_echo_state_reset(impl_->echo);
    }
    auto inputReady = std::make_shared<std::promise<void>>();
    auto outputReady = std::make_shared<std::promise<void>>();
    auto inputFuture = inputReady->get_future();
    auto outputFuture = outputReady->get_future();
    impl_->running.store(true);
    try {
        impl_->output = impl_->outputWorker(std::move(output), outputReady);
        impl_->microphone = impl_->inputWorker(std::move(input), inputReady);
        outputFuture.get();
        inputFuture.get();
    } catch (...) {
        impl_->stop();
        throw;
    }
}

void AudioEngine::stop() noexcept { impl_->stop(); }
void AudioEngine::setMicrophoneMuted(bool muted) noexcept { impl_->micMuted.store(muted); }
void AudioEngine::setDeafened(bool deafened) noexcept { impl_->deafened.store(deafened); }
void AudioEngine::setShareMuted(bool muted) noexcept { impl_->shareMuted.store(muted); }

bool AudioEngine::selectiveLoopbackSupported() noexcept {
    using VersionFunction = LONG (WINAPI*)(OSVERSIONINFOW*);
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto function = ntdll ? std::bit_cast<VersionFunction>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (!function) return false;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return function(&version) == 0 && (version.dwMajorVersion > 10 ||
        (version.dwMajorVersion == 10 && version.dwBuildNumber >= 20348));
}

void AudioEngine::startShareAudio(const CaptureSource& source) {
    if (!impl_->running.load()) throw std::runtime_error("Entre na chamada antes de compartilhar audio");
    if (!selectiveLoopbackSupported()) {
        throw std::runtime_error("Audio seletivo requer Windows build 20348 ou superior. "
                                 "Desative Compartilhar audio para transmitir somente a imagem.");
    }
    if (source.kind == SourceKind::Window) {
        if (!source.processId || source.processId == GetCurrentProcessId()) {
            throw std::runtime_error("Nao e permitido retransmitir o audio deste aplicativo de chamadas");
        }
        DWORD actualPid{};
        if (!IsWindow(reinterpret_cast<HWND>(source.handle)) ||
            !GetWindowThreadProcessId(reinterpret_cast<HWND>(source.handle), &actualPid) ||
            actualPid != source.processId) {
            throw std::runtime_error("A janela selecionada foi fechada ou mudou de processo");
        }
    }
    impl_->stopShare();
    auto startup = std::make_shared<std::promise<void>>();
    auto readyFuture = startup->get_future();
    impl_->sharing = std::jthread([this, source, startup](std::stop_token stop) {
        bool ready{};
        try {
            ComApartment apartment;
            AudioPriority priority;
            Stream stream;
            stream.client = activateLoopback(source, stop);
            stream.initialize(true, true);
            startup->set_value(); ready = true;
            impl_->captureLoop(stream, AudioKind::Shared, stop);
        } catch (...) {
            if (!ready) startup->set_exception(std::current_exception());
            else try { throw; } catch (const std::exception& error) { impl_->report(AudioFailure::Shared, error.what()); }
            catch (...) { impl_->report(AudioFailure::Shared, "Falha inesperada no audio compartilhado"); }
        }
    });
    try { readyFuture.get(); }
    catch (...) { impl_->stopShare(); throw; }
}

void AudioEngine::stopShareAudio() noexcept { impl_->stopShare(); }

void AudioEngine::setVolume(std::string peer, AudioKind kind, float gain) {
    if (!std::isfinite(gain)) return;
    std::lock_guard lock(impl_->remoteMutex);
    auto& remote = impl_->remotes[{std::move(peer), kind}];
    if (!remote) remote = std::make_unique<Remote>();
    remote->volume = std::clamp(gain, 0.0f, 2.0f);
}

void AudioEngine::receive(std::string peer, AudioKind kind,
                          std::span<const std::uint8_t> packet, std::uint64_t pts) {
    if (!impl_->running.load() || peer.empty() || packet.empty() || packet.size() > 4000) return;
    std::lock_guard lock(impl_->remoteMutex);
    auto& remote = impl_->remotes[{std::move(peer), kind}];
    if (!remote) remote = std::make_unique<Remote>();
    remote->push(packet, pts);
}

void AudioEngine::removePeer(std::string peer) {
    {
        std::lock_guard lock(impl_->remoteMutex);
        impl_->remotes.erase({peer, AudioKind::Microphone});
        impl_->remotes.erase({peer, AudioKind::Shared});
    }
    impl_->activity(peer, false);
}

std::optional<std::uint64_t> AudioEngine::playoutPosition(const std::string& peer, AudioKind kind) const {
    std::lock_guard lock(impl_->remoteMutex);
    const auto found = impl_->remotes.find({peer, kind});
    return found == impl_->remotes.end() ? std::nullopt : found->second->lastPlayed;
}

std::vector<AudioDevice> AudioEngine::enumerateDevices(bool input) {
    ComApartment apartment;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
          IID_PPV_ARGS(&enumerator)), "Enumeracao de dispositivos de audio");
    ComPtr<IMMDeviceCollection> devices;
    check(enumerator->EnumAudioEndpoints(input ? eCapture : eRender, DEVICE_STATE_ACTIVE, &devices),
          "EnumAudioEndpoints");
    UINT count{};
    check(devices->GetCount(&count), "Contagem de dispositivos de audio");
    std::vector<AudioDevice> result;
    for (UINT index = 0; index != count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(index, &device))) continue;
        LPWSTR id{};
        if (FAILED(device->GetId(&id))) continue;
        std::wstring deviceId(id);
        CoTaskMemFree(id);
        ComPtr<IPropertyStore> properties;
        std::wstring name = deviceId;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT value{};
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal) name = value.pwszVal;
            PropVariantClear(&value);
        }
        result.push_back({std::move(deviceId), std::move(name)});
    }
    return result;
}

} // namespace nativecall
