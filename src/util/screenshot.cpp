#include "util/screenshot.h"

#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace util
{
bool SaveBackBuffer(IDirect3DDevice9* device, const std::wstring& path, bool opaque)
{
    IDirect3DSurface9* back = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
        return false;
    D3DSURFACE_DESC desc;
    back->GetDesc(&desc);
    IDirect3DSurface9* sys = nullptr;
    bool ok = (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8)
           && SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))
           && SUCCEEDED(device->GetRenderTargetData(back, sys));
    D3DLOCKED_RECT lr;
    if (ok && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
    {
        FILE* f = _wfopen(path.c_str(), L"wb");
        ok = f != nullptr;
        if (f)
        {
            BITMAPINFOHEADER ih = {};
            ih.biSize = sizeof(ih);
            ih.biWidth = (LONG)desc.Width;
            ih.biHeight = -(LONG)desc.Height; // top-down
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;
            BITMAPFILEHEADER fh = {};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + desc.Width * desc.Height * 4;
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            std::vector<unsigned char> row(desc.Width * 4);
            for (UINT y = 0; y < desc.Height; y++)
            {
                memcpy(row.data(), (const unsigned char*)lr.pBits + y * lr.Pitch, row.size()); // already BGRA
                if (opaque)
                    for (UINT x = 0; x < desc.Width; x++)
                        row[x * 4 + 3] = 255;
                fwrite(row.data(), row.size(), 1, f);
            }
            fclose(f);
        }
        sys->UnlockRect();
    }
    if (sys) sys->Release();
    back->Release();
    return ok;
}
}
