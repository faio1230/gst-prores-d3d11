#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <chrono>

namespace prores {

struct BoundedMapResult {
    HRESULT result;
    bool timed_out;
};

// Map(DO_NOT_WAIT)の待機判断を分離し、実GPUを停止させずに期限分岐を検査する。
template <typename Attempt, typename DeviceStatus, typename Now, typename Pause>
BoundedMapResult bounded_staging_map(Attempt attempt, DeviceStatus device_status,
                                     Now now, Pause pause,
                                     std::chrono::steady_clock::duration timeout) {
    const auto deadline = now() + timeout;
    for (;;) {
        const HRESULT result = attempt();
        if (result != DXGI_ERROR_WAS_STILL_DRAWING) return {result, false};
        const HRESULT device_result = device_status();
        if (FAILED(device_result)) return {device_result, false};
        if (now() >= deadline) return {DXGI_ERROR_WAS_STILL_DRAWING, true};
        pause();
    }
}

}  // namespace prores
