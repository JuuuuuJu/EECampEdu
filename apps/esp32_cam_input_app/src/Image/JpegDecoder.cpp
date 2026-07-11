#include "JpegDecoder.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <cstdio>

static void ReleaseIfNeeded(IUnknown* ptr) {
    if (ptr != nullptr) {
        ptr->Release();
    }
}

static void SetError(std::string* error, const char* message, HRESULT hr = S_OK) {
    if (error == nullptr) {
        return;
    }
    if (hr == S_OK) {
        *error = message;
        return;
    }
    char buffer[160];
    snprintf(buffer, sizeof(buffer), "%s (HRESULT=0x%08lX)", message, static_cast<unsigned long>(hr));
    *error = buffer;
}

bool DecodeJpegToRgba(const uint8_t* data,
                      size_t size,
                      std::vector<uint8_t>* rgba,
                      int* width,
                      int* height,
                      std::string* error) {
    if (data == nullptr || size == 0 || rgba == nullptr || width == nullptr || height == nullptr) {
        SetError(error, "invalid JPEG decode arguments");
        return false;
    }
    if (size > static_cast<size_t>(0xFFFFFFFFu)) {
        SetError(error, "JPEG payload is too large for WIC memory stream");
        return false;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        SetError(error, "CoInitializeEx failed", hr);
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    hr = CoCreateInstance(CLSID_WICImagingFactory,
                          nullptr,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        SetError(error, "CoCreateInstance(CLSID_WICImagingFactory) failed", hr);
        return false;
    }

    hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromMemory(const_cast<BYTE*>(reinterpret_cast<const BYTE*>(data)), static_cast<DWORD>(size));
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(hr)) {
        hr = decoder->GetFrame(0, &frame);
    }

    UINT decoded_width = 0;
    UINT decoded_height = 0;
    if (SUCCEEDED(hr)) {
        hr = frame->GetSize(&decoded_width, &decoded_height);
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateFormatConverter(&converter);
    }
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame,
                                   GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone,
                                   nullptr,
                                   0.0,
                                   WICBitmapPaletteTypeCustom);
    }

    if (SUCCEEDED(hr) && decoded_width > 0 && decoded_height > 0) {
        const size_t pixels = static_cast<size_t>(decoded_width) * static_cast<size_t>(decoded_height);
        rgba->resize(pixels * 4);
        const UINT stride = decoded_width * 4;
        const UINT bytes = static_cast<UINT>(rgba->size());
        hr = converter->CopyPixels(nullptr, stride, bytes, rgba->data());
    }

    ReleaseIfNeeded(converter);
    ReleaseIfNeeded(frame);
    ReleaseIfNeeded(decoder);
    ReleaseIfNeeded(stream);
    ReleaseIfNeeded(factory);

    if (FAILED(hr)) {
        rgba->clear();
        SetError(error, "WIC JPEG decode failed", hr);
        return false;
    }

    *width = static_cast<int>(decoded_width);
    *height = static_cast<int>(decoded_height);
    return true;
}

#else
bool DecodeJpegToRgba(const uint8_t*,
                      size_t,
                      std::vector<uint8_t>*,
                      int*,
                      int*,
                      std::string* error) {
    if (error != nullptr) {
        *error = "JPEG preview is only implemented for the Windows UI build";
    }
    return false;
}
#endif

