#include "image/image_writer.h"

#include "common/log.h"
#include "common/util.h"

#include <wincodec.h>

namespace syna {
namespace {

HRESULT encode(IWICImagingFactory* factory, const std::wstring& file, const uint8_t* bgra, int width, int height, int stride,
               bool jpeg, float quality) {
    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) return hr;
    if (jpeg && props) {
        PROPBAG2 option = {};
        option.pstrName = const_cast<wchar_t*>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = quality;
        props->Write(1, &option, &v);
    }
    hr = frame->Initialize(props.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    WICPixelFormatGUID fmt = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (FAILED(hr)) return hr;

    if (fmt != GUID_WICPixelFormat32bppBGRA) {
        // The encoder negotiated another layout (always the case for JPEG): convert.
        ComPtr<IWICBitmap> bitmap;
        hr = factory->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height), GUID_WICPixelFormat32bppBGRA,
                                             static_cast<UINT>(stride), static_cast<UINT>(stride) * height, const_cast<BYTE*>(bgra), &bitmap);
        ComPtr<IWICFormatConverter> conv;
        if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&conv);
        if (SUCCEEDED(hr)) hr = conv->Initialize(bitmap.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) hr = frame->WriteSource(conv.Get(), nullptr);
    } else {
        hr = frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(stride), static_cast<UINT>(stride) * height,
                                const_cast<BYTE*>(bgra));
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return hr;  // all COM objects (and the file handle) are released on return
}

}  // namespace

syna_result write_image(const std::wstring& path, const uint8_t* bgra, int width, int height, int stride, bool jpeg, float quality) {
    if (!bgra || width <= 0 || height <= 0 || path.empty()) return SYNA_E_INVALID_ARG;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ScopeExit uninit([co] {
        if (SUCCEEDED(co)) CoUninitialize();
    });
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        set_last_error("WIC unavailable");
        return SYNA_E_FAIL;
    }
    const std::wstring temp = path + L".partial";
    const HRESULT hr = encode(factory.Get(), temp, bgra, width, height, stride, jpeg, quality);
    if (FAILED(hr)) {
        DeleteFileW(temp.c_str());
        set_last_error(std::format("image encode failed (0x{:08X})", static_cast<uint32_t>(hr)));
        log::error("image", "encode failed hr=0x{:08X} path=\"{}\"", static_cast<uint32_t>(hr), to_utf8(path));
        if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) return SYNA_E_ACCESS_DENIED;
        if (hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL)) return SYNA_E_DISK_FULL;
        return SYNA_E_IO;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD err = GetLastError();
        DeleteFileW(temp.c_str());
        set_last_error(std::format("cannot write {} (error {})", to_utf8(path), err));
        return err == ERROR_ACCESS_DENIED ? SYNA_E_ACCESS_DENIED : SYNA_E_IO;
    }
    return SYNA_OK;
}

}  // namespace syna
