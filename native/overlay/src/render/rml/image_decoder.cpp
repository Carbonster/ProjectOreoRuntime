#include "render/rml/image_decoder.h"

#include <windows.h>
#include <wincodec.h>

namespace image_decoder {

namespace {

template <typename T>
void release(T*& object) {
    if (object) object->Release();
    object = nullptr;
}

}  // namespace

bool decode(const std::string& bytes, uint32_t& width, uint32_t& height,
            std::vector<uint8_t>& rgba) {
    if (bytes.empty() || bytes.size() > 0xFFFFFFFFu) return false;
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // the game may own COM here
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    UINT w = 0, h = 0;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size())) &&
              SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr,
                                                         WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) &&
              SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
              SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppPRGBA,
                                              WICBitmapDitherTypeNone, nullptr, 0.0,
                                              WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(converter->GetSize(&w, &h)) && w > 0 && h > 0 && w <= kMaxSide &&
              h <= kMaxSide;
    if (ok) {
        rgba.resize((size_t)w * h * 4);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()));
    }
    release(converter);
    release(frame);
    release(decoder);
    release(stream);
    release(factory);
    if (SUCCEEDED(com)) CoUninitialize();
    width = w;
    height = h;
    return ok;
}

}  // namespace image_decoder
