// 実GPUを失わせずに、staging Mapの成功・待機・device lost・期限切れを検査する。
#include "d3d11_bounded_map.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

using Clock = std::chrono::steady_clock;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() try {
    int attempts = 0;
    int checks = 0;
    int pauses = 0;
    auto current = Clock::time_point{};
    const auto now = [&] { return current; };
    const auto pause = [&] { ++pauses; current += std::chrono::milliseconds(1); };

    auto ready = prores::bounded_staging_map(
        [&] { ++attempts; return S_OK; },
        [&] { ++checks; return S_OK; }, now, pause, std::chrono::milliseconds(3));
    require(ready.result == S_OK && !ready.timed_out && attempts == 1 &&
            checks == 0 && pauses == 0, "immediate Map was not returned");

    attempts = checks = pauses = 0;
    current = Clock::time_point{};
    auto delayed = prores::bounded_staging_map(
        [&] { return ++attempts < 3 ? DXGI_ERROR_WAS_STILL_DRAWING : S_OK; },
        [&] { ++checks; return S_OK; }, now, pause, std::chrono::milliseconds(3));
    require(delayed.result == S_OK && !delayed.timed_out && attempts == 3 &&
            checks == 2 && pauses == 2, "delayed Map was not completed");

    attempts = checks = pauses = 0;
    current = Clock::time_point{};
    auto removed = prores::bounded_staging_map(
        [&] { ++attempts; return DXGI_ERROR_WAS_STILL_DRAWING; },
        [&] { ++checks; return DXGI_ERROR_DEVICE_REMOVED; },
        now, pause, std::chrono::milliseconds(3));
    require(removed.result == DXGI_ERROR_DEVICE_REMOVED && !removed.timed_out &&
            attempts == 1 && checks == 1 && pauses == 0,
            "device removal was not propagated");

    attempts = checks = pauses = 0;
    current = Clock::time_point{};
    auto failed = prores::bounded_staging_map(
        [&] { ++attempts; return E_INVALIDARG; },
        [&] { ++checks; return S_OK; }, now, pause, std::chrono::milliseconds(3));
    require(failed.result == E_INVALIDARG && !failed.timed_out && attempts == 1 &&
            checks == 0 && pauses == 0, "Map error was not propagated");

    attempts = checks = pauses = 0;
    current = Clock::time_point{};
    auto timed = prores::bounded_staging_map(
        [&] { ++attempts; return DXGI_ERROR_WAS_STILL_DRAWING; },
        [&] { ++checks; return S_OK; }, now, pause, std::chrono::milliseconds(3));
    require(timed.timed_out && attempts == 4 && checks == 4 && pauses == 3,
            "busy Map did not stop at the deadline");

    std::cout << "{\"passed\":true,\"cases\":5,\"gpu_device_removed\":false}\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
