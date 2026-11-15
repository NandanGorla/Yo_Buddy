#include "platform/screen_capture.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <d3d11.h>
#include <mutex>
#include <wincodec.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include "platform/capture_target.h"

namespace buddy::capture {
namespace {

using winrt::com_ptr;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

struct CaptureState {
    std::mutex mutex;
    std::condition_variable ready;
    com_ptr<ID3D11Texture2D> texture;
    int width = 0;
    int height = 0;
    bool done = false;
    std::string error;
};

template <typename T>
winrt::com_ptr<T> query_dxgi(const auto& surface) {
    auto dxgi_access =
        surface.template as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<T> result;
    winrt::check_hresult(dxgi_access->GetInterface(__uuidof(T), result.put_void()));
    return result;
}

winrt::com_ptr<ID3D11Device> make_device(winrt::com_ptr<ID3D11DeviceContext>& context) {
    com_ptr<ID3D11Device> device;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level{};
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                            D3D11_SDK_VERSION, device.put(), &level, context.put()));
    return device;
}

std::string base64_encode(const std::uint8_t* data, std::size_t size) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t value = static_cast<std::uint32_t>(data[i]) << 16 |
                                    (i + 1 < size ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0) |
                                    (i + 2 < size ? data[i + 2] : 0);
        result.push_back(alphabet[(value >> 18) & 63]);
        result.push_back(alphabet[(value >> 12) & 63]);
        result.push_back(i + 1 < size ? alphabet[(value >> 6) & 63] : '=');
        result.push_back(i + 2 < size ? alphabet[value & 63] : '=');
    }
    return result;
}

}  // namespace

FrameResult capture_one(const TargetResult& target, int max_dimension) {
    if (!target.ok()) return {{}, target.error.empty() ? "Capture target is invalid." : target.error};
    if (max_dimension < 64 || max_dimension > 8192) return {{}, "Capture size limit is invalid."};
    if (target.window && is_buddy_window(target.window)) return {{}, "Buddy windows cannot be captured."};

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        com_ptr<ID3D11DeviceContext> context;
        const com_ptr<ID3D11Device> device = make_device(context);
        auto dxgi_device = device.as<IDXGIDevice>();
        com_ptr<::IInspectable> inspectable_device;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable_device.put()));
        const IDirect3DDevice capture_device = inspectable_device.as<IDirect3DDevice>();

        RECT bounds{};
        if (target.monitor) bounds = target.monitor->bounds;
        else if (!GetWindowRect(target.window, &bounds)) return {{}, "Could not read the capture window bounds."};
        const int source_width = bounds.right - bounds.left;
        const int source_height = bounds.bottom - bounds.top;
        if (source_width <= 0 || source_height <= 0) return {{}, "The capture target has no drawable area."};

        const auto item_interop =
            winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                          IGraphicsCaptureItemInterop>();
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
        if (target.monitor) {
            winrt::check_hresult(item_interop->CreateForMonitor(
                target.monitor->monitor,
                winrt::guid_of<winrt::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(item)));
        } else {
            winrt::check_hresult(item_interop->CreateForWindow(
                target.window,
                winrt::guid_of<winrt::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(item)));
        }

        const auto size = item.Size();
        const auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            capture_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size);
        auto session = pool.CreateCaptureSession(item);
        CaptureState state;
        auto token = pool.FrameArrived([&](Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;
            auto texture = query_dxgi<ID3D11Texture2D>(frame.Surface());
            {
                std::lock_guard lock(state.mutex);
                state.texture = std::move(texture);
                state.width = frame.ContentSize().Width;
                state.height = frame.ContentSize().Height;
                state.done = true;
            }
            state.ready.notify_one();
        });
        session.StartCapture();
        {
            std::unique_lock lock(state.mutex);
            if (!state.ready.wait_for(lock, std::chrono::seconds(3), [&] { return state.done; })) {
                pool.FrameArrived(token);
                return {{}, "Timed out waiting for a screen frame."};
            }
        }
        session = nullptr;
        pool.FrameArrived(token);

        const int scale = std::max(1, (std::max(state.width, state.height) + max_dimension - 1) / max_dimension);
        const int width = state.width / scale;
        const int height = state.height / scale;
        Frame result{width, height, width * 4, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
        D3D11_TEXTURE2D_DESC desc{};
        state.texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, staging.put()));
        context->CopyResource(staging.get(), state.texture.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        for (int y = 0; y < height; ++y) {
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y * scale) * mapped.RowPitch;
            auto* destination = result.bgra.data() + static_cast<std::size_t>(y) * result.stride;
            for (int x = 0; x < width; ++x) {
                const auto* pixel = source + static_cast<std::size_t>(x * scale) * 4;
                std::copy_n(pixel, 4, destination + x * 4);
            }
        }
        context->Unmap(staging.get(), 0);
        return {std::move(result), {}};
    } catch (const winrt::hresult_error& error) {
        return {{}, "Screen capture failed: " + std::to_string(static_cast<long>(error.code().value))};
    }
}

EncodedFrameResult encode_png(const Frame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.stride < frame.width * 4 ||
        frame.bgra.size() < static_cast<std::size_t>(frame.stride) * frame.height) {
        return {{}, "The captured frame is invalid."};
    }
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        winrt::com_ptr<IWICImagingFactory> factory;
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(factory.put())));
        winrt::com_ptr<IWICBitmap> bitmap;
        winrt::check_hresult(factory->CreateBitmapFromMemory(
            static_cast<UINT>(frame.width), static_cast<UINT>(frame.height), GUID_WICPixelFormat32bppBGRA,
            static_cast<UINT>(frame.stride), static_cast<UINT>(frame.bgra.size()),
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(frame.bgra.data())), bitmap.put()));

        winrt::com_ptr<IStream> stream;
        winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
        winrt::com_ptr<IWICStream> wic_stream;
        winrt::check_hresult(factory->CreateStream(wic_stream.put()));
        winrt::check_hresult(wic_stream->InitializeFromIStream(stream.get()));
        winrt::com_ptr<IWICBitmapEncoder> encoder;
        winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
        winrt::check_hresult(encoder->Initialize(wic_stream.get(), WICBitmapEncoderNoCache));
        winrt::com_ptr<IWICBitmapFrameEncode> frame_encoder;
        winrt::com_ptr<IPropertyBag2> properties;
        winrt::check_hresult(encoder->CreateNewFrame(frame_encoder.put(), properties.put()));
        winrt::check_hresult(frame_encoder->Initialize(properties.get()));
        winrt::check_hresult(frame_encoder->SetSize(static_cast<UINT>(frame.width), static_cast<UINT>(frame.height)));
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        winrt::check_hresult(frame_encoder->SetPixelFormat(&format));
        winrt::check_hresult(frame_encoder->WriteSource(bitmap.get(), nullptr));
        winrt::check_hresult(frame_encoder->Commit());
        winrt::check_hresult(encoder->Commit());

        STATSTG stat{};
        winrt::check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
        LARGE_INTEGER zero{};
        winrt::check_hresult(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
        ULONG read = 0;
        winrt::check_hresult(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read));
        return {base64_encode(bytes.data(), read), {}};
    } catch (const winrt::hresult_error& error) {
        return {{}, "PNG encoding failed: " + std::to_string(static_cast<long>(error.code().value))};
    }
}

}  // namespace buddy::capture
