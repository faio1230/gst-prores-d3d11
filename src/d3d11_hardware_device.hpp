#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <stdexcept>

// GstD3D11Device may wrap an externally supplied WARP device.  Feature level
// and typed UAV support alone do not prove that compute runs on a physical GPU.
inline DXGI_ADAPTER_DESC1 d3d11_adapter_description(ID3D11Device* device) {
    if (!device) throw std::runtime_error("missing native D3D11 device");
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))))
        throw std::runtime_error("cannot identify D3D11 DXGI device");
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter)))
        throw std::runtime_error("cannot identify D3D11 DXGI adapter");
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter1;
    if (FAILED(adapter.As(&adapter1)))
        throw std::runtime_error("cannot inspect D3D11 adapter software flag");
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapter1->GetDesc1(&description)))
        throw std::runtime_error("cannot read D3D11 adapter description");
    return description;
}

inline void require_d3d11_non_software_adapter(ID3D11Device* device) {
    const auto description = d3d11_adapter_description(device);
    if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        throw std::runtime_error("software D3D11 adapter is not a ProRes GPU backend");
}
