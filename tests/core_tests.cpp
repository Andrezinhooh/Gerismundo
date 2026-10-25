#include "nativecall/bounded_queue.hpp"
#include "nativecall/types.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace nativecall;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int main() try {
    bool rejected = false;
    try { BoundedQueue<int> invalid(0); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "zero-capacity queue accepted");
    BoundedQueue<int> frames(2);
    frames.push(1); frames.push(2); frames.push(3);
    require(frames.dropped() == 1, "overload did not count dropped frame");
    require(frames.pop() == 2 && frames.pop() == 3, "overload retained stale frames");
    auto reader = std::async(std::launch::async, [&] { return frames.pop(); });
    frames.close();
    require(reader.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
            "closing did not wake waiting consumer");
    require(!reader.get(), "closed empty queue returned a value");
    require(!frames.push(4), "closed queue accepted new work");

    BoundedQueue<int> paced(16);
    std::atomic<int> consumed{};
    std::jthread consumer([&] { while (auto n = paced.pop()) { (void)n; ++consumed; } });
    constexpr int count = 100000;
    for (int i=0; i<count; ++i) paced.push(i);
    paced.close(); consumer.join();
    require(consumed + paced.dropped() == count, "concurrent queue lost accounting");
    require(mediaNowUs() <= mediaNowUs(), "media clock moved backwards");
    std::cout << "PASS: bounded overload, close wakeup, drain, concurrent accounting, media clock\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n'; return 1;
}
