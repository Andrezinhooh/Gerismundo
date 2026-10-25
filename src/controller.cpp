#include "nativecall/controller.hpp"
#include "nativecall/audio.hpp"
#include "nativecall/capture.hpp"
#include "nativecall/transport.hpp"
#include "nativecall/video.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace nativecall {
namespace {
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}
struct Apartment {
    HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~Apartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
}

struct CallController::Impl {
    Callbacks callbacks;
    mutable std::mutex stateMutex;
    AppSnapshot state;
    std::atomic<bool> accepting{true};
    std::wstring inputDevice, outputDevice;
    std::unique_ptr<AudioEngine> audio;
    std::unique_ptr<RtcTransport> transport;
    std::unique_ptr<ScreenCapture> capture;
    std::unique_ptr<VideoEncoder> encoder;

    std::mutex commandMutex;
    std::condition_variable commandWake;
    std::deque<std::function<void()>> commands;
    bool commandsClosed{};
    std::thread commandThread;

    struct ReceiveQueue {
        std::deque<EncodedVideo> frames;
        bool needsKeyframe{true};
    };
    std::mutex videoMutex;
    std::condition_variable videoWake;
    std::map<std::string, ReceiveQueue> incomingVideo;
    std::set<std::string> resetDecoders;
    std::jthread videoThread;
    std::atomic<std::uint32_t> requestedBitrate{2500000};
    std::atomic<bool> videoFailureReported{};
    std::atomic<std::uint64_t> sharingGeneration{};
    std::optional<CaptureSource> activeSource;
    bool shareAudioStarted{};
    std::atomic<bool> recoveringAudio{};
    std::atomic<std::uint64_t> callGeneration{};

    explicit Impl(Callbacks cb) : callbacks(std::move(cb)) {
        audio = std::make_unique<AudioEngine>(AudioEngine::Callbacks{
            [this](AudioKind kind, std::vector<std::uint8_t> bytes, std::uint64_t pts) {
                if (accepting) transport->sendAudio(kind, bytes, pts);
            },
            [this](std::string id, bool speaking) {
                if (!accepting) return;
                update([&](auto& value) {
                    for (auto& peer : value.participants)
                        if (peer.id == id) peer.speaking = speaking;
                });
            },
            [this](AudioFailure failure, std::string error) {
                if (!accepting) return;
                report(std::move(error));
                if (failure == AudioFailure::Shared) {
                    const auto generation = sharingGeneration.load();
                    post([this, generation] {
                        if (generation == sharingGeneration && snapshot().sharing) stopSharingNow();
                    });
                } else if (!recoveringAudio.exchange(true)) {
                    post([this] { recoverAudio(); });
                }
            }
        });
        transport = std::make_unique<RtcTransport>(RtcTransport::Callbacks{
            [this](std::string id, std::string name) {
                if (!accepting) return;
                update([&](auto& value) {
                    auto found = std::find_if(value.participants.begin(), value.participants.end(),
                        [&](const auto& item) { return item.id == id; });
                    if (found == value.participants.end() && value.participants.size() < 4) {
                        ParticipantSnapshot peer; peer.id = id; peer.name = name;
                        value.participants.push_back(std::move(peer));
                    }
                });
                encoder->requestKeyframe();
                const auto current = snapshot();
                if (current.sharing) transport->announceShare(utf8(current.sourceName), true);
            },
            [this](std::string id) {
                if (!accepting) return;
                audio->removePeer(id);
                {
                    std::lock_guard lock(videoMutex);
                    incomingVideo.erase(id); resetDecoders.insert(id);
                }
                videoWake.notify_one();
                update([&](auto& value) {
                    std::erase_if(value.participants, [&](const auto& item) { return item.id == id; });
                });
            },
            [this](std::string id, AudioKind kind, std::vector<std::uint8_t> bytes, std::uint64_t pts) {
                if (accepting) audio->receive(std::move(id), kind, bytes, pts);
            },
            [this](std::string id, EncodedVideo frame) {
                if (!accepting || frame.bytes.size() > 8 * 1024 * 1024) return;
                bool requestKeyframe{}, queued{};
                {
                    std::lock_guard lock(videoMutex);
                    if (incomingVideo.size() >= 3 && !incomingVideo.contains(id)) return;
                    auto& queue = incomingVideo[id];
                    if (queue.frames.size() >= 8) {
                        queue.frames.clear(); queue.needsKeyframe = true;
                        resetDecoders.insert(id);
                        requestKeyframe = true;
                    }
                    if (!queue.needsKeyframe || frame.keyframe) {
                        if (frame.keyframe) queue.needsKeyframe = false;
                        queue.frames.push_back(std::move(frame)); queued = true;
                    }
                }
                if (requestKeyframe) transport->requestRemoteKeyframe(id);
                if (queued) videoWake.notify_one();
            },
            [this](std::string status) {
                if (accepting) update([&](auto& value) { value.connectionState = std::move(status); });
            },
            [this](std::string id, std::string source, bool active) {
                if (!accepting) return;
                if (!active) {
                    std::lock_guard lock(videoMutex);
                    incomingVideo.erase(id); resetDecoders.insert(id);
                }
                update([&](auto& value) {
                    for (auto& peer : value.participants) if (peer.id == id) {
                        peer.sharing = active; peer.sharedSource = std::move(source);
                        if (!active) peer.video.reset();
                    }
                });
            },
            [this] { if (accepting) encoder->requestKeyframe(); },
            [this](std::uint32_t bitrate) {
                if (!accepting) return;
                const auto current = snapshot();
                const auto next = std::clamp(bitrate, 350000u,
                    current.quality.width >= 1920 ? 8000000u : 4500000u);
                requestedBitrate = next;
                encoder->setBitrate(next);
                update([&](auto& value) { value.quality.bitrate = next; });
            },
            [this](std::string reason) {
                if (!accepting) return;
                const auto generation = callGeneration.load();
                post([this, reason = std::move(reason), generation] {
                    if (generation == callGeneration) { leaveNow(); report(reason); }
                });
            }
        });
        capture = std::make_unique<ScreenCapture>();
        encoder = std::make_unique<VideoEncoder>();
        commandThread = std::thread([this] { runCommands(); });
        try { videoThread = std::jthread([this](std::stop_token stop) { runVideo(stop); }); }
        catch (...) {
            { std::lock_guard lock(commandMutex); commandsClosed = true; }
            commandWake.notify_one(); commandThread.join(); throw;
        }
    }

    ~Impl() {
        accepting = false;
        {
            std::lock_guard lock(commandMutex);
            commands.clear();
            commands.push_back([this] { leaveNow(); });
            commandsClosed = true;
        }
        commandWake.notify_one();
        if (commandThread.joinable()) commandThread.join();
        videoThread.request_stop(); videoWake.notify_one();
        if (videoThread.joinable()) videoThread.join();
        // All objects that can invoke callbacks are gone before state/mutexes.
        transport.reset(); capture.reset(); encoder.reset(); audio.reset();
    }

    AppSnapshot snapshot() const {
        std::lock_guard lock(stateMutex);
        return state;
    }
    void changed() noexcept {
        try { if (accepting && callbacks.changed) callbacks.changed(); } catch (...) {}
    }
    template<class F> void update(F&& action) {
        { std::lock_guard lock(stateMutex); action(state); }
        changed();
    }
    void report(std::string message) noexcept {
        try { if (accepting && callbacks.error) callbacks.error(std::move(message)); } catch (...) {}
    }
    void post(std::function<void()> action) {
        bool full{};
        {
            std::lock_guard lock(commandMutex);
            if (commandsClosed || !accepting) return;
            full = commands.size() >= 256;
            if (!full) commands.push_back(std::move(action));
        }
        if (full) report("Muitas operações pendentes; aguarde a operação atual.");
        else commandWake.notify_one();
    }
    void runCommands() {
        Apartment apartment;
        for (;;) {
            std::function<void()> action;
            {
                std::unique_lock lock(commandMutex);
                commandWake.wait(lock, [&] { return commandsClosed || !commands.empty(); });
                if (commands.empty()) return;
                action = std::move(commands.front()); commands.pop_front();
            }
            try { action(); }
            catch (const std::exception& e) { report(e.what()); }
            catch (...) { report("Falha inesperada na operação nativa."); }
        }
    }

    void stopSharingNow() {
        ++sharingGeneration;
        activeSource.reset(); shareAudioStarted = false;
        capture->stop(); encoder->stop(); audio->stopShareAudio();
        transport->announceShare({}, false);
        update([](auto& value) {
            value.sharing = false; value.sourceName.clear(); value.shareMuted = false;
            for (auto& peer : value.participants) if (peer.id == "self") {
                peer.sharing = false; peer.sharedSource.clear();
            }
        });
    }
    void restoreAudioControls(const AppSnapshot& current) {
        audio->setMicrophoneMuted(current.micMuted);
        audio->setDeafened(current.deafened);
        for (const auto& peer : current.participants) if (peer.id != "self") {
            audio->setVolume(peer.id, AudioKind::Microphone, peer.voiceVolume);
            audio->setVolume(peer.id, AudioKind::Shared, peer.shareVolume);
        }
    }
    void recoverAudio() {
        const auto current = snapshot();
        if (!current.joined) { recoveringAudio = false; return; }
        const auto source = activeSource;
        const bool restartShareAudio = shareAudioStarted;
        audio->stop();
        for (int attempt = 0; attempt != 3 && accepting; ++attempt) {
            try {
                audio->start(inputDevice, outputDevice);
                restoreAudioControls(current);
                if (source && restartShareAudio) {
                    audio->setShareMuted(current.shareMuted);
                    audio->startShareAudio(*source);
                }
                recoveringAudio = false;
                update([](auto& value) { value.connectionState = "Dispositivos de áudio reiniciados"; });
                return;
            } catch (const std::exception&) {
                audio->stop();
                if (attempt != 2) {
                    std::unique_lock lock(commandMutex);
                    commandWake.wait_for(lock, std::chrono::milliseconds(250 * (attempt + 1)),
                                         [&] { return commandsClosed; });
                }
            }
        }
        recoveringAudio = false;
        leaveNow();
        report("Não foi possível recuperar os dispositivos de áudio. Verifique as permissões e selecione um dispositivo disponível antes de entrar novamente.");
    }
    void leaveNow() {
        ++callGeneration;
        stopSharingNow(); transport->leave(); audio->stop();
        {
            std::lock_guard lock(videoMutex);
            for (const auto& [id, unused] : incomingVideo) resetDecoders.insert(id);
            incomingVideo.clear();
        }
        update([](auto& value) {
            value.joined = false; value.joining = false; value.participants.clear();
            value.connectionState = "Desconectado";
        });
    }

    void runVideo(std::stop_token stop) {
        Apartment apartment;
        struct Display {
            std::shared_ptr<VideoFrame> frame;
            std::chrono::steady_clock::time_point received;
        };
        std::map<std::string, std::unique_ptr<VideoDecoder>> decoders;
        std::map<std::string, std::deque<Display>> pending;
        struct AudioClock {
            std::uint64_t position{};
            std::chrono::steady_clock::time_point changed{};
        };
        std::map<std::string, AudioClock> audioClocks;
        while (!stop.stop_requested()) {
            std::vector<std::pair<std::string, EncodedVideo>> batch;
            {
                std::unique_lock lock(videoMutex);
                videoWake.wait_for(lock, std::chrono::milliseconds(8));
                for (const auto& id : resetDecoders) {
                    decoders.erase(id); pending.erase(id); audioClocks.erase(id);
                }
                resetDecoders.clear();
                for (auto& [id, queue] : incomingVideo) {
                    if (!queue.frames.empty()) {
                        batch.emplace_back(id, std::move(queue.frames.front()));
                        queue.frames.pop_front();
                    }
                }
            }
            for (auto& [id, packet] : batch) {
                try {
                    auto& decoder = decoders[id];
                    if (!decoder) decoder = std::make_unique<VideoDecoder>();
                    if (auto frame = decoder->decode(packet.bytes, packet.ptsUs)) {
                        auto& pictures = pending[id];
                        if (pictures.size() == 4) pictures.pop_front();
                        pictures.push_back({std::make_shared<VideoFrame>(std::move(*frame)),
                                            std::chrono::steady_clock::now()});
                    }
                } catch (const std::exception& e) {
                    decoders.erase(id); pending.erase(id);
                    {
                        std::lock_guard lock(videoMutex);
                        if (auto found = incomingVideo.find(id); found != incomingVideo.end()) {
                            found->second.frames.clear(); found->second.needsKeyframe = true;
                        }
                    }
                    transport->requestRemoteKeyframe(id);
                    if (!videoFailureReported.exchange(true)) report(e.what());
                }
            }
            for (auto item = pending.begin(); item != pending.end();) {
                auto position = audio->playoutPosition(item->first, AudioKind::Shared);
                const auto now = std::chrono::steady_clock::now();
                auto& clock = audioClocks[item->first];
                if (position && *position != clock.position) {
                    clock.position = *position; clock.changed = now;
                }
                // A source may stop producing loopback packets while silent.
                if (position && now - clock.changed > std::chrono::milliseconds(120))
                    position.reset();
                auto& pictures = item->second;
                std::shared_ptr<VideoFrame> due;
                while (!pictures.empty()) {
                    const auto& picture = pictures.front();
                    const auto age = now - picture.received;
                    const auto pts = picture.frame->ptsUs;
                    const bool ready = (position && pts <= *position + 40000) ||
                        (!position && age >= std::chrono::milliseconds(35)) ||
                        age >= std::chrono::milliseconds(120);
                    if (!ready) break;
                    if (!position || pts + 200000 >= *position) due = picture.frame;
                    pictures.pop_front();
                }
                if (due) {
                    update([&](auto& value) {
                        for (auto& peer : value.participants)
                            if (peer.id == item->first) peer.video = due;
                    });
                }
                if (pictures.empty()) item = pending.erase(item);
                else ++item;
            }
        }
    }
};

CallController::CallController(Callbacks cb) : impl_(std::make_unique<Impl>(std::move(cb))) {}
CallController::~CallController() = default;
AppSnapshot CallController::snapshot() const { return impl_->snapshot(); }
void CallController::join(ConnectionSettings settings) {
    impl_->post([p = impl_.get(), settings = std::move(settings)]() mutable {
        p->leaveNow();
        ++p->callGeneration;
        p->update([](auto& value) {
            value.joining = true; value.connectionState = "Entrando na chamada…";
        });
        try {
            p->audio->start(p->inputDevice, p->outputDevice);
            const auto previous = p->snapshot();
            p->audio->setMicrophoneMuted(previous.micMuted);
            p->audio->setDeafened(previous.deafened);
            p->update([&](auto& value) {
                value.joined = true; value.joining = false;
                ParticipantSnapshot self; self.id = "self"; self.name = settings.displayName + " (você)";
                value.participants.push_back(std::move(self));
            });
            p->transport->join(std::move(settings));
        } catch (...) { p->leaveNow(); throw; }
    });
}
void CallController::leave() { impl_->post([p = impl_.get()] { p->leaveNow(); }); }
void CallController::setMicrophoneMuted(bool muted) {
    impl_->post([p = impl_.get(), muted] {
        p->audio->setMicrophoneMuted(muted);
        p->update([&](auto& value) { value.micMuted = muted; });
    });
}
void CallController::setDeafened(bool deafened) {
    impl_->post([p = impl_.get(), deafened] {
        p->audio->setDeafened(deafened);
        p->update([&](auto& value) { value.deafened = deafened; });
    });
}
void CallController::selectDevices(std::wstring input, std::wstring output) {
    impl_->post([p = impl_.get(), input = std::move(input), output = std::move(output)] {
        const auto current = p->snapshot();
        if (current.sharing) throw std::runtime_error("Interrompa o compartilhamento antes de trocar dispositivos.");
        const auto oldInput = p->inputDevice, oldOutput = p->outputDevice;
        p->inputDevice = input; p->outputDevice = output;
        if (current.joined) {
            p->audio->stop();
            try { p->audio->start(input, output); }
            catch (...) {
                p->inputDevice = oldInput; p->outputDevice = oldOutput;
                p->audio->start(oldInput, oldOutput); throw;
            }
            p->audio->setMicrophoneMuted(current.micMuted);
            p->audio->setDeafened(current.deafened);
            for (const auto& peer : current.participants) if (peer.id != "self") {
                p->audio->setVolume(peer.id, AudioKind::Microphone, peer.voiceVolume);
                p->audio->setVolume(peer.id, AudioKind::Shared, peer.shareVolume);
            }
        }
        p->changed();
    });
}
void CallController::setVolume(std::string peer, AudioKind kind, float gain) {
    if (!std::isfinite(gain)) return;
    gain = std::clamp(gain, 0.0f, 2.0f);
    impl_->post([p = impl_.get(), peer = std::move(peer), kind, gain] {
        p->audio->setVolume(peer, kind, gain);
        p->update([&](auto& value) { for (auto& item : value.participants) if (item.id == peer) {
            (kind == AudioKind::Microphone ? item.voiceVolume : item.shareVolume) = gain;
        } });
    });
}
void CallController::startSharing(CaptureSource source, VideoQuality quality, bool withAudio) {
    impl_->post([p = impl_.get(), source = std::move(source), quality, withAudio] {
        if (!p->snapshot().joined) throw std::runtime_error("Entre em uma chamada antes de compartilhar.");
        if (!((quality.width == 1280 && quality.height == 720) ||
              (quality.width == 1920 && quality.height == 1080)) ||
              !(quality.fps == 30 || quality.fps == 60))
            throw std::invalid_argument("Qualidade inválida; selecione 720p/1080p e 30/60 FPS.");
        if (withAudio && !AudioEngine::selectiveLoopbackSupported())
            throw std::runtime_error("Áudio seletivo requer Windows build 20348 ou superior. Desative Compartilhar áudio para transmitir somente imagem.");
        p->stopSharingNow();
        p->videoFailureReported = false;
        const auto generation = ++p->sharingGeneration;
        try {
            p->encoder->start(quality,
                [p, generation](EncodedVideo frame) {
                    if (p->accepting && generation == p->sharingGeneration) p->transport->sendVideo(frame);
                },
                [p, generation](std::string error) {
                    if (generation != p->sharingGeneration) return;
                    p->report(std::move(error));
                    p->post([p, generation] {
                        if (generation == p->sharingGeneration) p->stopSharingNow();
                    });
                });
            p->activeSource = source;
            p->audio->setShareMuted(!withAudio);
            if (withAudio) { p->audio->startShareAudio(source); p->shareAudioStarted = true; }
            p->update([&](auto& value) {
                value.sharing = true; value.shareMuted = !withAudio;
                value.sourceName = source.name; value.quality = quality;
                for (auto& peer : value.participants) if (peer.id == "self") {
                    peer.sharing = true; peer.sharedSource = utf8(source.name);
                }
            });
            p->transport->announceShare(utf8(source.name), true);
            p->capture->start(source, quality,
                [p, generation](GpuFrame frame) {
                    if (p->accepting && generation == p->sharingGeneration) p->encoder->submit(std::move(frame));
                },
                [p, generation](std::string error) {
                    if (generation != p->sharingGeneration) return;
                    p->report(std::move(error));
                    p->post([p, generation] {
                        if (generation == p->sharingGeneration) p->stopSharingNow();
                    });
                });
        } catch (...) { p->stopSharingNow(); throw; }
    });
}
void CallController::stopSharing() { impl_->post([p = impl_.get()] { p->stopSharingNow(); }); }
void CallController::setShareMuted(bool muted) {
    impl_->post([p = impl_.get(), muted] {
        if (!p->snapshot().sharing) return;
        if (!muted && !p->shareAudioStarted) {
            if (!p->activeSource) return;
            p->audio->startShareAudio(*p->activeSource);
            p->shareAudioStarted = true;
        }
        p->audio->setShareMuted(muted);
        p->update([&](auto& value) { value.shareMuted = muted; });
    });
}

} // namespace nativecall
