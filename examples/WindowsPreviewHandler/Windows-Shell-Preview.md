# Windows Shell Preview and Thumbnail Handlers for .vds Files

This document describes how to implement Windows Explorer preview and thumbnail functionality for OpenVDS (.vds) files.

## Overview

Windows provides two distinct extension points for file previews:

| Handler | Purpose | Interface | Displayed In |
|---------|---------|-----------|--------------|
| Preview Handler | Full interactive preview in Explorer's preview pane | `IPreviewHandler` | Preview pane (Alt+P) |
| Thumbnail Handler | Static thumbnail image | `IThumbnailProvider` | Icons, tiles, content views |

Both handlers are implemented in a single COM DLL with two registered CLSIDs.

## Architecture

```
VdsShellExtension.dll
├── DllMain / DllGetClassObject / DllRegisterServer
├── ClassFactory (routes to correct handler by CLSID)
├── VdsThumbnailProvider : IThumbnailProvider, IInitializeWithStream
└── VdsPreviewHandler : IPreviewHandler, IInitializeWithStream, ...
    ├── Preview Window (child HWND)
    │   ├── Slice View (graphical - inline/crossline/timeslice)
    │   └── Metadata View (scrollable text)
    └── Input handling (hotkeys, mouse)
```

## Thumbnail Handler

The simpler of the two. Generates a bitmap thumbnail on demand.

### Required Interfaces

Your COM class must implement:

- `IInitializeWithStream` — receives the file as an `IStream` (you have this)
- `IThumbnailProvider` — generates the thumbnail bitmap

### IThumbnailProvider Interface

```cpp
class VdsThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider
{
    // IInitializeWithStream
    HRESULT Initialize(IStream* pStream, DWORD grfMode) override
    {
        // Store stream, read VDS header/metadata as needed
        m_stream = pStream;
        return S_OK;
    }

    // IThumbnailProvider
    HRESULT GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override
    {
        // cx = requested thumbnail size (e.g., 256)
        // Create HBITMAP with your VDS preview image
        // Set *pdwAlpha = WTSAT_ARGB if using alpha, WTSAT_RGB otherwise
        
        *phbmp = CreateYourVdsThumbnail(m_stream, cx);
        *pdwAlpha = WTSAT_RGB;
        return S_OK;
    }
};
```

### Registration

```reg
; Register the COM server
HKCR\CLSID\{YOUR-THUMBNAIL-GUID}
    (Default) = "VDS Thumbnail Handler"
HKCR\CLSID\{YOUR-THUMBNAIL-GUID}\InprocServer32
    (Default) = "C:\path\to\VdsShellExtension.dll"
    ThreadingModel = "Apartment"

; Associate with .vds files
HKCR\.vds\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}
    (Default) = "{YOUR-THUMBNAIL-GUID}"
```

The GUID `{e357fccd-a995-4576-b01f-234630154e96}` is the system's thumbnail handler identifier.

## Preview Handler

More complex. Provides a live, potentially interactive preview in Explorer's preview pane.

### Required Interfaces

Your COM class must implement:

- `IInitializeWithStream` — receives the file
- `IPreviewHandler` — core preview functionality
- `IObjectWithSite` — receives the frame's site
- `IOleWindow` — window management

Optional but recommended:

- `IPreviewHandlerVisuals` — receives background/font colors from Explorer

### Preview Modes

The preview handler supports multiple view modes:

| Mode | Content | Interaction |
|------|---------|-------------|
| Slice View | Inline/crossline/timeslice rendering | Mouse scroll to navigate slices |
| Metadata View | Scrollable text (axes, dimensions, brick info, etc.) | Scroll, select/copy |

Users switch modes via hotkeys.

### Keyboard Input in Preview Handlers

Preview handlers **can** receive keyboard input, but it requires explicit focus management. The preview pane doesn't automatically route keys to your window.

Key points:

1. **Focus**: Call `SetFocus(m_hwndPreview)` when the user clicks in your preview window
2. **Message loop**: The surrogate host (`prevhost.exe`) pumps messages; your `WndProc` receives them when focused
3. **Accelerators**: For reliability, handle keys in `WM_KEYDOWN` rather than accelerator tables

```cpp
// In your preview window's WndProc
case WM_LBUTTONDOWN:
    SetFocus(hWnd);  // Grab focus on click
    break;

case WM_KEYDOWN:
    switch (wParam)
    {
    case VK_TAB:
    case 'V':       // Switch view mode
        ToggleViewMode();
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    
    case VK_UP:
    case VK_DOWN:
        // Scroll metadata or navigate slices
        HandleScroll(wParam == VK_UP ? -1 : 1);
        return 0;
    
    case VK_PRIOR:  // Page Up
    case VK_NEXT:   // Page Down
        HandleScroll(wParam == VK_PRIOR ? -10 : 10);
        return 0;
    }
    break;

case WM_MOUSEWHEEL:
    {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        HandleScroll(-delta);
    }
    return 0;
```

### IPreviewHandler Interface

```cpp
class VdsPreviewHandler : public IInitializeWithStream,
                          public IPreviewHandler,
                          public IObjectWithSite,
                          public IOleWindow,
                          public IPreviewHandlerVisuals
{
    HWND m_hwndParent = nullptr;
    RECT m_rect = {};
    IStream* m_stream = nullptr;
    HWND m_hwndPreview = nullptr;

    // View state
    enum class ViewMode { Slice, Metadata };
    ViewMode m_viewMode = ViewMode::Slice;
    int m_sliceIndex = 0;
    int m_scrollOffset = 0;
    
    // Colors from Explorer
    COLORREF m_bgColor = RGB(255, 255, 255);
    COLORREF m_textColor = RGB(0, 0, 0);

    // IPreviewHandler
    HRESULT SetWindow(HWND hwnd, const RECT* prc) override
    {
        m_hwndParent = hwnd;
        m_rect = *prc;
        if (m_hwndPreview)
            MoveWindow(m_hwndPreview, 
                m_rect.left, m_rect.top,
                m_rect.right - m_rect.left,
                m_rect.bottom - m_rect.top, TRUE);
        return S_OK;
    }

    HRESULT SetRect(const RECT* prc) override
    {
        m_rect = *prc;
        if (m_hwndPreview)
            MoveWindow(m_hwndPreview,
                m_rect.left, m_rect.top,
                m_rect.right - m_rect.left,
                m_rect.bottom - m_rect.top, TRUE);
        return S_OK;
    }

    HRESULT DoPreview() override
    {
        // Register window class (once)
        static bool registered = false;
        if (!registered)
        {
            WNDCLASS wc = {};
            wc.lpfnWndProc = PreviewWndProc;
            wc.hInstance = g_hInstance;
            wc.lpszClassName = L"VdsPreviewWindow";
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClass(&wc);
            registered = true;
        }

        // Create preview window
        m_hwndPreview = CreateWindowEx(0,
            L"VdsPreviewWindow", nullptr,
            WS_CHILD | WS_VISIBLE,
            m_rect.left, m_rect.top,
            m_rect.right - m_rect.left,
            m_rect.bottom - m_rect.top,
            m_hwndParent, nullptr, g_hInstance, this);

        return m_hwndPreview ? S_OK : E_FAIL;
    }

    HRESULT Unload() override
    {
        if (m_hwndPreview)
        {
            DestroyWindow(m_hwndPreview);
            m_hwndPreview = nullptr;
        }
        SafeRelease(&m_stream);
        m_sliceIndex = 0;
        m_scrollOffset = 0;
        return S_OK;
    }

    // IPreviewHandlerVisuals
    HRESULT SetBackgroundColor(COLORREF color) override
    {
        m_bgColor = color;
        if (m_hwndPreview)
            InvalidateRect(m_hwndPreview, nullptr, TRUE);
        return S_OK;
    }

    HRESULT SetTextColor(COLORREF color) override
    {
        m_textColor = color;
        if (m_hwndPreview)
            InvalidateRect(m_hwndPreview, nullptr, TRUE);
        return S_OK;
    }

    HRESULT SetFont(const LOGFONTW* plf) override
    {
        // Optionally create font from LOGFONT
        return S_OK;
    }

    // IOleWindow
    HRESULT GetWindow(HWND* phwnd) override
    {
        *phwnd = m_hwndParent;
        return S_OK;
    }

    HRESULT ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
};
```

### Preview Window Rendering

```cpp
// WM_PAINT handler (simplified)
void OnPaint(HWND hWnd, VdsPreviewHandler* pHandler)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    
    RECT rc;
    GetClientRect(hWnd, &rc);

    // Fill background with Explorer's color
    HBRUSH hbr = CreateSolidBrush(pHandler->m_bgColor);
    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);

    if (pHandler->m_viewMode == ViewMode::Slice)
    {
        // Render slice image from VDS
        // Use your existing IStream-based VDS reader
        RenderSlice(hdc, rc, pHandler->m_stream, pHandler->m_sliceIndex);
        
        // Draw slice indicator
        wchar_t buf[64];
        swprintf_s(buf, L"Slice %d  [V]=Metadata  [↑↓]=Navigate", 
            pHandler->m_sliceIndex);
        SetTextColor(hdc, pHandler->m_textColor);
        SetBkMode(hdc, TRANSPARENT);
        DrawText(hdc, buf, -1, &rc, DT_BOTTOM | DT_CENTER | DT_SINGLELINE);
    }
    else // Metadata view
    {
        // Render scrollable metadata text
        RenderMetadata(hdc, rc, pHandler->m_stream, 
            pHandler->m_scrollOffset, pHandler->m_textColor);
        
        // Draw mode indicator
        wchar_t buf[64] = L"[V]=Slice View  [↑↓/PgUp/PgDn]=Scroll";
        DrawText(hdc, buf, -1, &rc, DT_BOTTOM | DT_CENTER | DT_SINGLELINE);
    }

    EndPaint(hWnd, &ps);
}
```

### Scrollable Metadata View

For the metadata view, track scroll position and render visible lines:

```cpp
void RenderMetadata(HDC hdc, RECT rc, IStream* stream, int scrollOffset, COLORREF textColor)
{
    // Get metadata strings from VDS (cache these after first read)
    std::vector<std::wstring> lines = GetVdsMetadataLines(stream);
    // Example lines:
    //   "Dimensions: 1000 x 500 x 200"
    //   "Inline range: 100 - 1100"
    //   "Crossline range: 200 - 700"
    //   "Sample interval: 4ms"
    //   "Brick size: 64 x 64 x 64"
    //   ...

    HFONT hFont = CreateFont(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    
    SetTextColor(hdc, textColor);
    SetBkMode(hdc, TRANSPARENT);

    int lineHeight = 18;
    int y = 10 - (scrollOffset * lineHeight);
    int margin = 10;

    for (const auto& line : lines)
    {
        if (y + lineHeight > 0 && y < rc.bottom - 30)  // visible
        {
            RECT lineRect = { margin, y, rc.right - margin, y + lineHeight };
            DrawText(hdc, line.c_str(), -1, &lineRect, DT_LEFT | DT_SINGLELINE);
        }
        y += lineHeight;
    }

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
}
```

### Registration

```reg
; Register the COM server
HKCR\CLSID\{YOUR-PREVIEW-GUID}
    (Default) = "VDS Preview Handler"
    AppID = "{YOUR-PREVIEW-GUID}"
HKCR\CLSID\{YOUR-PREVIEW-GUID}\InprocServer32
    (Default) = "C:\path\to\VdsShellExtension.dll"
    ThreadingModel = "Apartment"

; Mark as safe for low-integrity (required for previewer isolation)
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\PreviewHandlers
    {YOUR-PREVIEW-GUID} = "VDS Preview Handler"

; Associate with .vds files
HKCR\.vds\ShellEx\{8895b1c6-b41f-4c1c-a562-0d564250836f}
    (Default) = "{YOUR-PREVIEW-GUID}"
```

The GUID `{8895b1c6-b41f-4c1c-a562-0d564250836f}` is the system's preview handler identifier.

### AppID for Isolation

Preview handlers run in a low-integrity surrogate process for security. Register an AppID:

```reg
HKCR\AppID\{YOUR-PREVIEW-GUID}
    DllSurrogate = ""
```

## Single DLL Implementation

Both handlers share one DLL with a routing class factory.

### GUIDs

```cpp
// {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} - generate your own
static const CLSID CLSID_VdsThumbnailProvider = { /* ... */ };
static const CLSID CLSID_VdsPreviewHandler = { /* ... */ };
```

### Class Factory

```cpp
class ClassFactory : public IClassFactory
{
    CLSID m_clsid;
    LONG m_refCount = 1;

public:
    ClassFactory(REFCLSID clsid) : m_clsid(clsid) {}

    // IUnknown
    HRESULT QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG Release() override
    {
        LONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }

    // IClassFactory
    HRESULT CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override
    {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;

        IUnknown* pObj = nullptr;

        if (m_clsid == CLSID_VdsThumbnailProvider)
            pObj = new (std::nothrow) VdsThumbnailProvider();
        else if (m_clsid == CLSID_VdsPreviewHandler)
            pObj = new (std::nothrow) VdsPreviewHandler();
        else
            return CLASS_E_CLASSNOTAVAILABLE;

        if (!pObj) return E_OUTOFMEMORY;

        HRESULT hr = pObj->QueryInterface(riid, ppv);
        pObj->Release();
        return hr;
    }

    HRESULT LockServer(BOOL) override { return S_OK; }
};
```

### DLL Exports

```cpp
HINSTANCE g_hInstance = nullptr;
LONG g_dllRefCount = 0;

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        g_hInstance = hInstance;
        DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (rclsid == CLSID_VdsThumbnailProvider || rclsid == CLSID_VdsPreviewHandler)
    {
        ClassFactory* pFactory = new (std::nothrow) ClassFactory(rclsid);
        if (!pFactory) return E_OUTOFMEMORY;
        
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release();
        return hr;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow()
{
    return (g_dllRefCount == 0) ? S_OK : S_FALSE;
}
```

### Self-Registration (Optional)

```cpp
STDAPI DllRegisterServer()
{
    // Write registry keys programmatically
    // See Registration sections above for required keys
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    // Remove registry keys
    return S_OK;
}
```

### Module Definition File (VdsShellExtension.def)

```
LIBRARY VdsShellExtension
EXPORTS
    DllGetClassObject   PRIVATE
    DllCanUnloadNow     PRIVATE
    DllRegisterServer   PRIVATE
    DllUnregisterServer PRIVATE
```

## Implementation Notes

### Threading

Both handlers use apartment threading. Explorer may call from different threads, so avoid storing state that assumes single-threaded access to shared resources.

### IStream Usage

Since you already have IStream-based VDS reading code, both handlers integrate cleanly. The shell provides the IStream in `IInitializeWithStream::Initialize`. Consider caching parsed metadata after first read to avoid re-parsing on every paint.

### Focus Caveats

Explorer doesn't automatically focus the preview window. Users must click in the preview area before hotkeys work. Display a hint like `"Click to enable keyboard navigation"` until first focus.

### Slice Caching

For the slice view, rendering a full VDS slice on every paint may be slow. Consider:

1. Rendering the current slice to an off-screen bitmap on slice change
2. Blitting the cached bitmap in `WM_PAINT`
3. Invalidating the cache when slice index changes

### Aspect-Aware Scaling

VDS slices often have non-square dimensions (e.g., 1000 inlines × 500 samples). Scale to fit the preview pane while preserving aspect ratio:

```cpp
// Calculate scaled dimensions that fit within target while preserving aspect ratio
void CalcAspectFitRect(int srcWidth, int srcHeight, 
                       int targetWidth, int targetHeight,
                       RECT* pOutRect)
{
    float srcAspect = (float)srcWidth / srcHeight;
    float targetAspect = (float)targetWidth / targetHeight;

    int destWidth, destHeight;

    if (srcAspect > targetAspect)
    {
        // Source is wider than target - fit to width
        destWidth = targetWidth;
        destHeight = (int)(targetWidth / srcAspect);
    }
    else
    {
        // Source is taller than target - fit to height
        destHeight = targetHeight;
        destWidth = (int)(targetHeight * srcAspect);
    }

    // Center in target area
    int offsetX = (targetWidth - destWidth) / 2;
    int offsetY = (targetHeight - destHeight) / 2;

    pOutRect->left = offsetX;
    pOutRect->top = offsetY;
    pOutRect->right = offsetX + destWidth;
    pOutRect->bottom = offsetY + destHeight;
}

void RenderSlice(HDC hdc, RECT clientRect, HBITMAP hSliceBitmap, 
                 int sliceWidth, int sliceHeight)
{
    // Reserve space for status text at bottom
    int availableHeight = (clientRect.bottom - clientRect.top) - 30;
    int availableWidth = clientRect.right - clientRect.left;

    RECT destRect;
    CalcAspectFitRect(sliceWidth, sliceHeight, 
                      availableWidth, availableHeight, &destRect);

    // Offset for client area
    destRect.left += clientRect.left;
    destRect.right += clientRect.left;
    destRect.top += clientRect.top;
    destRect.bottom += clientRect.top;

    // Use StretchBlt or higher-quality GDI+ for scaling
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hSliceBitmap);

    // SetStretchBltMode for better quality when downscaling
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, nullptr);

    StretchBlt(hdc,
        destRect.left, destRect.top,
        destRect.right - destRect.left,
        destRect.bottom - destRect.top,
        hdcMem,
        0, 0, sliceWidth, sliceHeight,
        SRCCOPY);

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
}
```

For higher quality scaling (especially when downscaling significantly), consider using GDI+ `Graphics::DrawImage` with `InterpolationModeHighQualityBicubic`, or Direct2D.

### Debugging

Preview handlers run out-of-process in `prevhost.exe`. To debug:

1. Build debug, register the DLL
2. Attach debugger to `prevhost.exe` when it spawns
3. Or temporarily add `DisableLowILProcessIsolation` (DWORD=1) under your preview handler's CLSID to run in-process (debug only, remove for release)

Thumbnail handlers run in Explorer's process directly—attach to `explorer.exe`.

### Restarting Explorer After Registration

After registering or updating the DLL, you may need to restart Explorer:

```cmd
taskkill /f /im explorer.exe && start explorer.exe
```

Or log off/on to refresh the shell extension cache.

## Complete Registration Script

Save as `register.reg` (replace GUIDs with your own):

```reg
Windows Registry Editor Version 5.00

; === File Association ===
[HKEY_CLASSES_ROOT\.vds]
@="OpenVDS.Document"

[HKEY_CLASSES_ROOT\OpenVDS.Document]
@="OpenVDS Volume Data"

[HKEY_CLASSES_ROOT\OpenVDS.Document\DefaultIcon]
@="C:\\Path\\To\\VdsShellExtension.dll,0"

; === Thumbnail Handler ===
[HKEY_CLASSES_ROOT\CLSID\{11111111-1111-1111-1111-111111111111}]
@="VDS Thumbnail Handler"

[HKEY_CLASSES_ROOT\CLSID\{11111111-1111-1111-1111-111111111111}\InprocServer32]
@="C:\\Path\\To\\VdsShellExtension.dll"
"ThreadingModel"="Apartment"

[HKEY_CLASSES_ROOT\.vds\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}]
@="{11111111-1111-1111-1111-111111111111}"

; === Preview Handler ===
[HKEY_CLASSES_ROOT\CLSID\{22222222-2222-2222-2222-222222222222}]
@="VDS Preview Handler"
"AppID"="{22222222-2222-2222-2222-222222222222}"

[HKEY_CLASSES_ROOT\CLSID\{22222222-2222-2222-2222-222222222222}\InprocServer32]
@="C:\\Path\\To\\VdsShellExtension.dll"
"ThreadingModel"="Apartment"

[HKEY_CLASSES_ROOT\AppID\{22222222-2222-2222-2222-222222222222}]
"DllSurrogate"=""

[HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\PreviewHandlers]
"{22222222-2222-2222-2222-222222222222}"="VDS Preview Handler"

[HKEY_CLASSES_ROOT\.vds\ShellEx\{8895b1c6-b41f-4c1c-a562-0d564250836f}]
@="{22222222-2222-2222-2222-222222222222}"
```

## Summary

| Component | Key Interface | System GUID | Your CLSID |
|-----------|---------------|-------------|------------|
| Thumbnail | `IThumbnailProvider` | `{e357fccd-a995-4576-b01f-234630154e96}` | Generate unique |
| Preview | `IPreviewHandler` | `{8895b1c6-b41f-4c1c-a562-0d564250836f}` | Generate unique |

**Preview Handler Hotkeys:**

| Key | Action |
|-----|--------|
| V / Tab | Toggle between Slice and Metadata view |
| ↑ / ↓ | Scroll metadata or navigate slices |
| PgUp / PgDn | Fast scroll / jump slices |
| Mouse wheel | Scroll / navigate |

Both handlers receive the file via `IInitializeWithStream`, making your existing VDS/IStream code directly applicable.
