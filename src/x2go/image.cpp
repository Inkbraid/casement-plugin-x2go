#include "x2go/image.h"

#include <windows.h>
#include <wincodec.h>

#include <format>

#include "casement/text.h"

namespace x2go {

namespace {

/// Holds one COM reference.
template <class T>
class Ref {
public:
    Ref() = default;
    ~Ref() {
        if (p_) p_->Release();
    }
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    T** put() { return &p_; }
    T* operator->() const { return p_; }
    T* get() const { return p_; }

private:
    T* p_ = nullptr;
};

}  // namespace

struct WicDecoder::Impl {
    bool com = false;
    IWICImagingFactory* factory = nullptr;
};

WicDecoder::WicDecoder(std::string& error) : impl_(std::make_unique<Impl>()) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    impl_->com = SUCCEEDED(hr);
    hr = CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                          reinterpret_cast<void**>(&impl_->factory));
    if (FAILED(hr)) {
        impl_->factory = nullptr;
        error = std::format("Windows Imaging Component unavailable: {}", casement::system_message(static_cast<unsigned long>(hr)));
    }
}

WicDecoder::~WicDecoder() {
    if (impl_->factory) impl_->factory->Release();
    if (impl_->com) CoUninitialize();
}

bool WicDecoder::decode(std::span<const std::uint8_t> data, Image& out) {
    IWICImagingFactory* f = impl_->factory;
    if (!f || data.empty() || data.size() > 0x7FFFFFFF) return false;
    Ref<IWICStream> stream;
    Ref<IWICBitmapDecoder> decoder;
    Ref<IWICBitmapFrameDecode> frame;
    Ref<IWICFormatConverter> converter;
    if (FAILED(f->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data.data()), static_cast<DWORD>(data.size()))) ||
        FAILED(f->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())))
        return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > 16384 || h > 16384) return false;
    if (FAILED(f->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom)))
        return false;
    out.width = w;
    out.height = h;
    out.pixels.resize(std::size_t{w} * h * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.pixels.size()), out.pixels.data()));
}

}  // namespace x2go
