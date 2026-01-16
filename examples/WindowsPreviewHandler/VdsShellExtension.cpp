/****************************************************************************
** Copyright 2019 The Open Group
** Copyright 2019 Bluware, Inc.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
****************************************************************************/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <propsys.h>

#include "VdsRenderer.h"
#include <memory>
#include <string>

#pragma comment(lib, "shlwapi.lib")

// ============================================================================
// GUIDs
// ============================================================================

// {8F7B3D50-5C4E-4A1F-9E5D-6B2A7F8C4D91} - VDS Thumbnail Provider
static const CLSID CLSID_VdsThumbnailProvider =
{ 0x8f7b3d50, 0x5c4e, 0x4a1f, { 0x9e, 0x5d, 0x6b, 0x2a, 0x7f, 0x8c, 0x4d, 0x91 } };

// {9A2E5F61-7D3B-4C2F-A8E6-1C4B8E9D5F72} - VDS Preview Handler
static const CLSID CLSID_VdsPreviewHandler =
{ 0x9a2e5f61, 0x7d3b, 0x4c2f, { 0xa8, 0xe6, 0x1c, 0x4b, 0x8e, 0x9d, 0x5f, 0x72 } };

// ============================================================================
// Globals
// ============================================================================

HINSTANCE g_hInstance = nullptr;
LONG g_dllRefCount = 0;

// ============================================================================
// Helper macros
// ============================================================================

template<typename T>
void SafeRelease(T** ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = nullptr;
    }
}

// ============================================================================
// VdsThumbnailProvider
// ============================================================================

class VdsThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider
{
public:
    VdsThumbnailProvider() : m_refCount(1), m_stream(nullptr) {}

    ~VdsThumbnailProvider()
    {
        SafeRelease(&m_stream);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] =
        {
            QITABENT(VdsThumbnailProvider, IInitializeWithStream),
            QITABENT(VdsThumbnailProvider, IThumbnailProvider),
            { 0 }
        };
        return QISearch(this, qit, riid, ppv);
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        LONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0)
            delete this;
        return ref;
    }

    // IInitializeWithStream
    STDMETHODIMP Initialize(IStream* pStream, DWORD grfMode) override
    {
        SafeRelease(&m_stream);
        m_stream = pStream;
        if (m_stream)
            m_stream->AddRef();
        return S_OK;
    }

    // IThumbnailProvider
    STDMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override
    {
        if (!m_stream || !phbmp || !pdwAlpha)
            return E_INVALIDARG;

        *phbmp = nullptr;
        *pdwAlpha = WTSAT_RGB;

        try
        {
            // Create VDS renderer
            VdsRenderer renderer;
            if (!renderer.Initialize(m_stream))
                return E_FAIL;

            // Render middle slice of the first dimension
            int dimension = (renderer.GetDimensionality() >= 3) ? 2 : 0;  // Prefer depth/time slice for 3D
            int sliceIndex = renderer.GetDefaultSliceIndex(dimension);
            HBITMAP hBitmap = renderer.RenderSlice(dimension, sliceIndex, cx);

            if (!hBitmap)
                return E_FAIL;

            *phbmp = hBitmap;
            return S_OK;
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

private:
    LONG m_refCount;
    IStream* m_stream;
};

// ============================================================================
// VdsPreviewHandler
// ============================================================================

class VdsPreviewHandler :
    public IInitializeWithStream,
    public IPreviewHandler,
    public IPreviewHandlerVisuals,
    public IObjectWithSite,
    public IOleWindow
{
public:
    VdsPreviewHandler()
        : m_refCount(1)
        , m_stream(nullptr)
        , m_site(nullptr)
        , m_hwndParent(nullptr)
        , m_hwndPreview(nullptr)
        , m_viewMode(ViewMode::Slice)
        , m_sliceIndex(0)
        , m_scrollOffset(0)
        , m_dimension(0)
        , m_bgColor(RGB(255, 255, 255))
        , m_textColor(RGB(0, 0, 0))
        , m_cachedBitmap(nullptr)
        , m_hasFocus(false)
    {
        ZeroMemory(&m_rect, sizeof(m_rect));
    }

    ~VdsPreviewHandler()
    {
        SafeRelease(&m_stream);
        SafeRelease(&m_site);
        if (m_cachedBitmap)
            DeleteObject(m_cachedBitmap);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] =
        {
            QITABENT(VdsPreviewHandler, IInitializeWithStream),
            QITABENT(VdsPreviewHandler, IPreviewHandler),
            QITABENT(VdsPreviewHandler, IPreviewHandlerVisuals),
            QITABENT(VdsPreviewHandler, IObjectWithSite),
            QITABENT(VdsPreviewHandler, IOleWindow),
            { 0 }
        };
        return QISearch(this, qit, riid, ppv);
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        InterlockedIncrement(&g_dllRefCount);
        return InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        InterlockedDecrement(&g_dllRefCount);
        LONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0)
            delete this;
        return ref;
    }

    // IInitializeWithStream
    STDMETHODIMP Initialize(IStream* pStream, DWORD grfMode) override
    {
        SafeRelease(&m_stream);
        m_stream = pStream;
        if (m_stream)
            m_stream->AddRef();

        // Initialize renderer
        if (m_stream)
        {
            m_renderer = std::make_unique<VdsRenderer>();
            if (!m_renderer->Initialize(m_stream))
            {
                m_renderer.reset();
                return E_FAIL;
            }

            // Set default dimension (prefer depth/time for 3D data)
            m_dimension = (m_renderer->GetDimensionality() >= 3) ? 2 : 0;
            m_sliceIndex = m_renderer->GetDefaultSliceIndex(m_dimension);
        }

        return S_OK;
    }

    // IPreviewHandler
    STDMETHODIMP SetWindow(HWND hwnd, const RECT* prc) override
    {
        m_hwndParent = hwnd;
        m_rect = *prc;

        if (m_hwndPreview)
        {
            SetWindowPos(m_hwndPreview, nullptr,
                        m_rect.left, m_rect.top,
                        m_rect.right - m_rect.left,
                        m_rect.bottom - m_rect.top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
        }

        return S_OK;
    }

    STDMETHODIMP SetRect(const RECT* prc) override
    {
        m_rect = *prc;

        if (m_hwndPreview)
        {
            SetWindowPos(m_hwndPreview, nullptr,
                        m_rect.left, m_rect.top,
                        m_rect.right - m_rect.left,
                        m_rect.bottom - m_rect.top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
        }

        return S_OK;
    }

    STDMETHODIMP DoPreview() override
    {
        // Register window class
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = StaticWndProc;
            wc.hInstance = g_hInstance;
            wc.lpszClassName = L"VdsPreviewWindow";
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            RegisterClassW(&wc);
            registered = true;
        }

        // Create preview window
        m_hwndPreview = CreateWindowExW(
            0,
            L"VdsPreviewWindow",
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            m_rect.left, m_rect.top,
            m_rect.right - m_rect.left,
            m_rect.bottom - m_rect.top,
            m_hwndParent,
            nullptr,
            g_hInstance,
            this);

        return m_hwndPreview ? S_OK : E_FAIL;
    }

    STDMETHODIMP Unload() override
    {
        if (m_hwndPreview)
        {
            DestroyWindow(m_hwndPreview);
            m_hwndPreview = nullptr;
        }

        if (m_cachedBitmap)
        {
            DeleteObject(m_cachedBitmap);
            m_cachedBitmap = nullptr;
        }

        m_renderer.reset();
        SafeRelease(&m_stream);
        m_sliceIndex = 0;
        m_scrollOffset = 0;
        m_hasFocus = false;

        return S_OK;
    }

    STDMETHODIMP SetFocus() override
    {
        if (m_hwndPreview)
        {
            ::SetFocus(m_hwndPreview);
            return S_OK;
        }
        return E_FAIL;
    }

    STDMETHODIMP QueryFocus(HWND* phwnd) override
    {
        if (!phwnd)
            return E_INVALIDARG;
        *phwnd = ::GetFocus();
        return S_OK;
    }

    STDMETHODIMP TranslateAccelerator(MSG* pmsg) override
    {
        // Let the system handle accelerators
        return S_FALSE;
    }

    // IPreviewHandlerVisuals
    STDMETHODIMP SetBackgroundColor(COLORREF color) override
    {
        m_bgColor = color;
        if (m_hwndPreview)
            InvalidateRect(m_hwndPreview, nullptr, TRUE);
        return S_OK;
    }

    STDMETHODIMP SetTextColor(COLORREF color) override
    {
        m_textColor = color;
        if (m_hwndPreview)
            InvalidateRect(m_hwndPreview, nullptr, TRUE);
        return S_OK;
    }

    STDMETHODIMP SetFont(const LOGFONTW* plf) override
    {
        // Could create custom font here
        return S_OK;
    }

    // IObjectWithSite
    STDMETHODIMP SetSite(IUnknown* pUnkSite) override
    {
        SafeRelease(&m_site);
        m_site = pUnkSite;
        if (m_site)
            m_site->AddRef();
        return S_OK;
    }

    STDMETHODIMP GetSite(REFIID riid, void** ppv) override
    {
        if (!m_site)
            return E_FAIL;
        return m_site->QueryInterface(riid, ppv);
    }

    // IOleWindow
    STDMETHODIMP GetWindow(HWND* phwnd) override
    {
        if (!phwnd)
            return E_INVALIDARG;
        *phwnd = m_hwndParent;
        return S_OK;
    }

    STDMETHODIMP ContextSensitiveHelp(BOOL) override
    {
        return E_NOTIMPL;
    }

    // Window procedure
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        VdsPreviewHandler* pThis = nullptr;

        if (msg == WM_CREATE)
        {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            pThis = reinterpret_cast<VdsPreviewHandler*>(pCreate->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        }
        else
        {
            pThis = reinterpret_cast<VdsPreviewHandler*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        }

        if (pThis)
            return pThis->WndProc(hwnd, msg, wParam, lParam);

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_LBUTTONDOWN:
            ::SetFocus(hwnd);
            m_hasFocus = true;
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;

        case WM_KILLFOCUS:
            m_hasFocus = false;
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;

        case WM_KEYDOWN:
            HandleKeyDown(wParam);
            return 0;

        case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            HandleScroll(-delta);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;  // We handle erase in WM_PAINT

        case WM_DESTROY:
            if (m_cachedBitmap)
            {
                DeleteObject(m_cachedBitmap);
                m_cachedBitmap = nullptr;
            }
            return 0;
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

private:
    enum class ViewMode { Slice, Metadata };

    LONG m_refCount;
    IStream* m_stream;
    IUnknown* m_site;
    HWND m_hwndParent;
    HWND m_hwndPreview;
    RECT m_rect;
    ViewMode m_viewMode;
    int m_sliceIndex;
    int m_scrollOffset;
    int m_dimension;
    COLORREF m_bgColor;
    COLORREF m_textColor;
    HBITMAP m_cachedBitmap;
    bool m_hasFocus;
    std::unique_ptr<VdsRenderer> m_renderer;

    void HandleKeyDown(WPARAM key)
    {
        switch (key)
        {
        case 'V':
        case VK_TAB:
            // Toggle view mode
            m_viewMode = (m_viewMode == ViewMode::Slice) ? ViewMode::Metadata : ViewMode::Slice;
            m_scrollOffset = 0;
            InvalidateRect(m_hwndPreview, nullptr, TRUE);
            break;

        case VK_UP:
            HandleScroll(-1);
            break;

        case VK_DOWN:
            HandleScroll(1);
            break;

        case VK_PRIOR:  // Page Up
            HandleScroll(-10);
            break;

        case VK_NEXT:  // Page Down
            HandleScroll(10);
            break;

        case VK_LEFT:
        case VK_RIGHT:
            // Cycle through dimensions (for 3D data)
            if (m_viewMode == ViewMode::Slice && m_renderer)
            {
                int dimCount = m_renderer->GetDimensionality();
                if (dimCount > 2)
                {
                    m_dimension = (key == VK_RIGHT) ?
                        ((m_dimension + 1) % dimCount) :
                        ((m_dimension + dimCount - 1) % dimCount);
                    m_sliceIndex = m_renderer->GetDefaultSliceIndex(m_dimension);
                    UpdateCachedBitmap();
                    InvalidateRect(m_hwndPreview, nullptr, TRUE);
                }
            }
            break;
        }
    }

    void HandleScroll(int delta)
    {
        if (m_viewMode == ViewMode::Slice && m_renderer)
        {
            // No slice navigation for 2D data
            if (m_renderer->GetDimensionality() <= 2)
                return;

            int maxSlice = m_renderer->GetSliceCount(m_dimension) - 1;
            m_sliceIndex = std::max(0, std::min(maxSlice, m_sliceIndex + delta));
            UpdateCachedBitmap();
        }
        else
        {
            // Metadata scroll
            m_scrollOffset = std::max(0, m_scrollOffset + delta);
        }

        InvalidateRect(m_hwndPreview, nullptr, TRUE);
    }

    void UpdateCachedBitmap()
    {
        if (m_cachedBitmap)
        {
            DeleteObject(m_cachedBitmap);
            m_cachedBitmap = nullptr;
        }

        if (m_renderer && m_viewMode == ViewMode::Slice)
        {
            RECT rc;
            GetClientRect(m_hwndPreview, &rc);
            int maxSize = std::max(rc.right - rc.left, rc.bottom - rc.top);
            m_cachedBitmap = m_renderer->RenderSlice(m_dimension, m_sliceIndex, maxSize);
        }
    }

    void OnPaint()
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_hwndPreview, &ps);

        RECT rc;
        GetClientRect(m_hwndPreview, &rc);

        // Fill background
        HBRUSH hbr = CreateSolidBrush(m_bgColor);
        FillRect(hdc, &rc, hbr);
        DeleteObject(hbr);

        if (!m_renderer)
        {
            DrawErrorMessage(hdc, rc, L"Failed to open VDS file");
        }
        else if (m_viewMode == ViewMode::Slice)
        {
            RenderSliceView(hdc, rc);
        }
        else
        {
            RenderMetadataView(hdc, rc);
        }

        EndPaint(m_hwndPreview, &ps);
    }

    void DrawErrorMessage(HDC hdc, const RECT& rc, const wchar_t* message)
    {
        ::SetTextColor(hdc, m_textColor);
        ::SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, message, -1, const_cast<RECT*>(&rc),
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void RenderSliceView(HDC hdc, RECT rc)
    {
        // Ensure bitmap is cached
        if (!m_cachedBitmap)
            UpdateCachedBitmap();

        if (m_cachedBitmap)
        {
            // Get bitmap dimensions
            BITMAP bm;
            GetObject(m_cachedBitmap, sizeof(bm), &bm);

            // Calculate display rect (preserve aspect ratio, leave room for text)
            int availableHeight = (rc.bottom - rc.top) - 40;
            int availableWidth = rc.right - rc.left;

            RECT destRect = CalculateAspectFitRect(bm.bmWidth, bm.bmHeight,
                                                   availableWidth, availableHeight);

            // Center in available space
            OffsetRect(&destRect, 0, 10);

            // Draw bitmap
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, m_cachedBitmap);

            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, nullptr);

            StretchBlt(hdc,
                      destRect.left, destRect.top,
                      destRect.right - destRect.left,
                      destRect.bottom - destRect.top,
                      hdcMem,
                      0, 0, bm.bmWidth, bm.bmHeight,
                      SRCCOPY);

            SelectObject(hdcMem, hOldBitmap);
            DeleteDC(hdcMem);
        }

        // Draw status text
        ::SetTextColor(hdc, m_textColor);
        ::SetBkMode(hdc, TRANSPARENT);

        wchar_t buf[256];
        int dimensionality = m_renderer->GetDimensionality();

        if (dimensionality == 2)
        {
            // For 2D data, no slice navigation
            swprintf_s(buf, L"2D Data View  |  [V]=Metadata%s",
                      m_hasFocus ? L"" : L"  |  Click to enable keyboard");
        }
        else
        {
            const wchar_t* dimName = m_renderer->GetDimensionName(m_dimension);
            swprintf_s(buf, L"%s Slice %d/%d  |  [V]=Metadata  [↑↓]=Navigate  [←→]=Dimension%s",
                      dimName,
                      m_sliceIndex + 1,
                      m_renderer->GetSliceCount(m_dimension),
                      m_hasFocus ? L"" : L"  |  Click to enable keyboard");
        }

        RECT textRect = rc;
        textRect.top = textRect.bottom - 30;
        DrawTextW(hdc, buf, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void RenderMetadataView(HDC hdc, RECT rc)
    {
        auto lines = m_renderer->GetMetadataLines();

        HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

        ::SetTextColor(hdc, m_textColor);
        ::SetBkMode(hdc, TRANSPARENT);

        int lineHeight = 20;
        int y = 10 - (m_scrollOffset * lineHeight);
        int margin = 20;

        for (const auto& line : lines)
        {
            if (y + lineHeight > 0 && y < rc.bottom - 40)
            {
                RECT lineRect = { margin, y, rc.right - margin, y + lineHeight };
                DrawTextW(hdc, line.c_str(), -1, &lineRect, DT_LEFT | DT_SINGLELINE);
            }
            y += lineHeight;
        }

        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);

        // Draw status
        wchar_t buf[128];
        swprintf_s(buf, L"[V]=Slice View  |  [↑↓/PgUp/PgDn]=Scroll%s",
                  m_hasFocus ? L"" : L"  |  Click to enable keyboard");

        RECT textRect = rc;
        textRect.top = textRect.bottom - 30;

        HFONT hStatusFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hOldStatusFont = (HFONT)SelectObject(hdc, hStatusFont);

        DrawTextW(hdc, buf, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, hOldStatusFont);
        DeleteObject(hStatusFont);
    }

    RECT CalculateAspectFitRect(int srcWidth, int srcHeight, int targetWidth, int targetHeight)
    {
        float srcAspect = (float)srcWidth / srcHeight;
        float targetAspect = (float)targetWidth / targetHeight;

        int destWidth, destHeight;

        if (srcAspect > targetAspect)
        {
            destWidth = targetWidth;
            destHeight = (int)(targetWidth / srcAspect);
        }
        else
        {
            destHeight = targetHeight;
            destWidth = (int)(targetHeight * srcAspect);
        }

        int offsetX = (targetWidth - destWidth) / 2;
        int offsetY = (targetHeight - destHeight) / 2;

        RECT result;
        result.left = offsetX;
        result.top = offsetY;
        result.right = offsetX + destWidth;
        result.bottom = offsetY + destHeight;
        return result;
    }
};

// ============================================================================
// ClassFactory
// ============================================================================

class ClassFactory : public IClassFactory
{
public:
    ClassFactory(REFCLSID clsid) : m_refCount(1), m_clsid(clsid) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
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

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        LONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0)
            delete this;
        return ref;
    }

    // IClassFactory
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override
    {
        if (pUnkOuter)
            return CLASS_E_NOAGGREGATION;

        IUnknown* pObj = nullptr;

        if (m_clsid == CLSID_VdsThumbnailProvider)
            pObj = static_cast<IThumbnailProvider*>(new (std::nothrow) VdsThumbnailProvider());
        else if (m_clsid == CLSID_VdsPreviewHandler)
            pObj = static_cast<IPreviewHandler*>(new (std::nothrow) VdsPreviewHandler());
        else
            return CLASS_E_CLASSNOTAVAILABLE;

        if (!pObj)
            return E_OUTOFMEMORY;

        HRESULT hr = pObj->QueryInterface(riid, ppv);
        pObj->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL) override
    {
        return S_OK;
    }

private:
    LONG m_refCount;
    CLSID m_clsid;
};

// ============================================================================
// DLL Exports
// ============================================================================

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
        if (!pFactory)
            return E_OUTOFMEMORY;

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
