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
#include <windowsx.h>  // For GET_X_LPARAM, GET_Y_LPARAM
#include <shlwapi.h>
#include <thumbcache.h>
#include <propsys.h>

#include "VdsRenderer.h"
#include "version.h"
#include <cstdarg>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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
// Log file path helper
// ============================================================================

// Build log file path in %USERPROFILE%\AppData\LocalLow\Temp
static std::string GetLogFilePath(const char* filename)
{
    char expandedPath[MAX_PATH];
    DWORD result = ExpandEnvironmentStringsA("%USERPROFILE%\\AppData\\LocalLow\\Temp\\", expandedPath, MAX_PATH);
    if (result == 0 || result > MAX_PATH)
    {
        // Fallback to temp directory
        return std::string("c:\\temp\\") + filename;
    }
    return std::string(expandedPath) + filename;
}

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
            // Use useProgressiveLOD=false for thumbnails - go straight to target LOD
            int sliceOnDimension = renderer.GetDimensionality() - 1;
            int sliceIndex = renderer.GetDefaultSliceIndex(sliceOnDimension);
            HBITMAP hBitmap = renderer.RenderSlice(sliceOnDimension, sliceIndex, cx, false);

            if (!hBitmap)
                return E_FAIL;

            *phbmp = hBitmap;
            return S_OK;
        }
        catch (const std::exception& e)
        {
            // LogPreview("Exception whilst rendering thumbnail.");
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

// Debug message storage for on-screen display and file logging
static std::vector<std::string> g_debugMessages;
static const size_t MAX_DEBUG_MESSAGES = 100;
static int g_debugScrollOffset = 0;

// Log to both on-screen display and preview log file
static void LogPreview(const char* message)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "[%02d:%02d:%02d] %s",
             st.wHour, st.wMinute, st.wSecond, message);

    // Add to on-screen display
    g_debugMessages.push_back(buffer);
    if (g_debugMessages.size() > MAX_DEBUG_MESSAGES)
        g_debugMessages.erase(g_debugMessages.begin());

    // Also write to log file
    std::string logPath = GetLogFilePath("openvds-previewpane.log");
    std::ofstream logFile(logPath, std::ios::app);
    if (logFile.is_open())
    {
        char timestamp[64];
        snprintf(timestamp, sizeof(timestamp), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        logFile << timestamp << message << "\n";
        logFile.flush();
    }
}

static void LogPreviewFmt(const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    LogPreview(buffer);
}

// Draw debug messages on screen (scrollable)
static void DrawDebugMessages(HDC hdc, RECT rc, COLORREF textColor)
{
    ::SetTextColor(hdc, textColor);
    ::SetBkMode(hdc, TRANSPARENT);

    HFONT hFont = CreateFontA(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

    // Header
    RECT headerRect = { rc.left + 8, rc.top + 6, rc.right - 8, rc.top + 20 };
    ::SetTextColor(hdc, RGB(60, 60, 120));
    DrawTextA(hdc, "Debug Log", -1, &headerRect, DT_LEFT | DT_SINGLELINE);
    ::SetTextColor(hdc, textColor);

    if (g_debugMessages.empty())
    {
        RECT emptyRect = { rc.left + 8, rc.top + 24, rc.right - 8, rc.bottom };
        DrawTextA(hdc, "(no messages)", -1, &emptyRect, DT_LEFT | DT_SINGLELINE);
        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);
        return;
    }

    int lineHeight = 13;
    int contentTop = rc.top + 24;
    int contentBottom = rc.bottom - 22;
    int maxLines = (contentBottom - contentTop) / lineHeight;

    int startIdx = g_debugScrollOffset;
    if (startIdx < 0) startIdx = 0;
    if (startIdx >= (int)g_debugMessages.size()) startIdx = (int)g_debugMessages.size() - 1;

    int y = contentTop;
    for (int i = startIdx; i < (int)g_debugMessages.size() && (i - startIdx) < maxLines; i++)
    {
        RECT textRect = { rc.left + 8, y, rc.right - 8, y + lineHeight };
        DrawTextA(hdc, g_debugMessages[i].c_str(), -1, &textRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += lineHeight;
    }

    // Footer with scroll info
    ::SetTextColor(hdc, RGB(100, 100, 100));
    char scrollInfo[64];
    int endIdx = std::min(startIdx + maxLines, (int)g_debugMessages.size());
    snprintf(scrollInfo, sizeof(scrollInfo), "Lines %d-%d of %d",
             startIdx + 1, endIdx, (int)g_debugMessages.size());
    RECT infoRect = { rc.left + 8, rc.bottom - 18, rc.right - 8, rc.bottom - 4 };
    DrawTextA(hdc, scrollInfo, -1, &infoRect, DT_LEFT | DT_SINGLELINE);

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
}

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
        , m_sliceIndex(0)
        , m_dimension(0)
        , m_bgColor(RGB(30, 30, 30))
        , m_textColor(RGB(200, 200, 200))
        , m_cachedBitmap(nullptr)
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
        m_lastError.clear();
        SafeRelease(&m_stream);
        m_stream = pStream;
        if (m_stream)
            m_stream->AddRef();

        if (!m_stream)
        {
            LogPreview("Initialize: no stream");
            return S_OK;
        }

        // Get stream size
        STATSTG stat = {};
        if (SUCCEEDED(m_stream->Stat(&stat, STATFLAG_NONAME)))
        {
            LogPreviewFmt("Stream: %lld bytes", stat.cbSize.QuadPart);
        }

        // Reset stream position
        LARGE_INTEGER seekPos = {};
        if (FAILED(m_stream->Seek(seekPos, STREAM_SEEK_SET, nullptr)))
        {
            LogPreview("Seek failed");
            m_lastError = L"Seek failed";
        }

        // Wait for any pending render on old renderer before destroying it
        // This prevents orphaned VDS requests when switching files
        if (m_renderer)
        {
            m_renderer->WaitForPendingRender();
        }

        // Clear cached bitmap when switching files
        if (m_cachedBitmap)
        {
            DeleteObject(m_cachedBitmap);
            m_cachedBitmap = nullptr;
        }
        m_cachedSliceIndex = -1;

        // Create renderer
        m_renderer = std::make_unique<VdsRenderer>();
        m_renderer->SetLogFile(GetLogFilePath("openvds-previewpane.log").c_str());

        if (!m_renderer->Initialize(m_stream))
        {
            LogPreview("VDS init failed");
            m_lastError = L"VDS init failed";
            m_renderer.reset();
            return E_FAIL;
        }

        // Set default slice
        m_dimension = m_renderer->GetDimensionality() - 1;
        m_sliceIndex = m_renderer->GetDefaultSliceIndex(m_dimension);
        LogPreviewFmt("VDS OK: %dD, slice %d/%d",
                     m_renderer->GetDimensionality(),
                     m_sliceIndex + 1,
                     m_renderer->GetSliceCount(m_dimension));

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
        // Register window class once
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

        if (!m_hwndPreview)
        {
            LogPreviewFmt("CreateWindow failed: %d", GetLastError());
        }

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
        m_cachedSliceIndex = -1;
        // Wait for any pending render before destroying renderer
        if (m_renderer)
        {
            m_renderer->WaitForPendingRender();
        }
        m_renderer.reset();
        SafeRelease(&m_stream);
        m_sliceIndex = 0;
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

        case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;

            // Get mouse position to determine which half
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            ScreenToClient(hwnd, &pt);

            RECT rc;
            GetClientRect(hwnd, &rc);
            int midX = (rc.right - rc.left) / 2;

            if (pt.x > midX && m_renderer)
            {
                // Right side: scroll through slices
                // Scroll by 10 slices normally, 1 slice with SHIFT held
                int scrollAmount = (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 10;
                int maxSlice = m_renderer->GetSliceCount(m_dimension) - 1;
                int newSlice = m_sliceIndex - delta * scrollAmount;
                newSlice = std::max(0, std::min(maxSlice, newSlice));

                // Cancel any pending refinement timer and in-progress render when slice changes
                if (newSlice != m_sliceIndex)
                {
                    KillTimer(hwnd, 1);
                    m_renderer->RequestCancel();  // Signal render to stop if in progress
                }
                m_sliceIndex = newSlice;
            }
            else
            {
                // Left side: scroll through debug messages
                g_debugScrollOffset -= delta;
                if (g_debugScrollOffset < 0) g_debugScrollOffset = 0;
                if (g_debugScrollOffset >= (int)g_debugMessages.size())
                    g_debugScrollOffset = (int)g_debugMessages.size() - 1;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_SIZE:
            // Cancel any pending refinement timer
            KillTimer(hwnd, 1);
            // Invalidate cached bitmap on resize so it re-renders at new size
            if (m_cachedBitmap)
            {
                DeleteObject(m_cachedBitmap);
                m_cachedBitmap = nullptr;
                m_cachedSliceIndex = -1;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;  // We handle erase in WM_PAINT

        case WM_TIMER:
            if (wParam == 1)  // Refinement timer
            {
                KillTimer(hwnd, 1);
                // Force re-render by invalidating cache
                m_cachedSliceIndex = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, 1);  // Cancel any pending refinement timer
            if (m_cachedBitmap)
            {
                DeleteObject(m_cachedBitmap);
                m_cachedBitmap = nullptr;
                m_cachedSliceIndex = -1;
            }
            return 0;
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

private:
    LONG m_refCount;
    IStream* m_stream;
    IUnknown* m_site;
    HWND m_hwndParent;
    HWND m_hwndPreview;
    RECT m_rect;
    int m_sliceIndex;
    int m_dimension;
    COLORREF m_bgColor;
    COLORREF m_textColor;
    HBITMAP m_cachedBitmap;
    int m_cachedSliceIndex = -1;  // Track which slice the cached bitmap represents
    std::unique_ptr<VdsRenderer> m_renderer;
    std::wstring m_lastError;  // For diagnostic display
    bool m_isRendering = false;  // Re-entrancy guard for COM message pumping

    // Create a test pattern bitmap (for debugging)
    HBITMAP CreateTestBitmap(int width, int height, int sliceIndex)
    {
        HDC hdcScreen = GetDC(nullptr);
        if (!hdcScreen) return nullptr;

        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        if (!hdcMem)
        {
            ReleaseDC(nullptr, hdcScreen);
            return nullptr;
        }

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pBits = nullptr;
        HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);

        if (!hBitmap || !pBits) return nullptr;

        // Fill with gradient test pattern
        uint32_t* pixels = static_cast<uint32_t*>(pBits);
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                uint8_t r = (uint8_t)((x * 255 / width + sliceIndex * 7) % 256);
                uint8_t g = (uint8_t)((y * 255 / height + sliceIndex * 13) % 256);
                uint8_t b = (uint8_t)(((x + y) * 255 / (width + height) + sliceIndex * 23) % 256);
                pixels[y * width + x] = (b) | (g << 8) | (r << 16);
            }
        }
        return hBitmap;
    }

    void UpdateCachedBitmap()
    {
        // Re-entrancy guard: COM may pump messages during IStream::Read(),
        // causing WM_PAINT to fire while we're mid-render. Skip nested calls.
        if (m_isRendering)
        {
            LogPreview("Skipping re-entrant render");
            return;
        }

        if (!m_renderer)
        {
            m_lastError = L"No renderer";
            return;
        }

        // Check if we need to re-render (slice changed or no bitmap)
        if (m_cachedBitmap && m_cachedSliceIndex == m_sliceIndex)
        {
            return;  // Already have the right slice cached
        }

        RECT rc;
        GetClientRect(m_hwndPreview, &rc);

        // Calculate actual bitmap display area (right half of window, minus info/status areas)
        // The window is split: left half = info panel, right half = bitmap display
        int totalWidth = rc.right - rc.left;
        int totalHeight = rc.bottom - rc.top;
        int bitmapAreaWidth = totalWidth / 2 - 20;  // Right half minus margins
        int bitmapAreaHeight = totalHeight - 80;     // Minus info and status bars
        int maxSize = std::max(bitmapAreaWidth, bitmapAreaHeight);

        // Ensure reasonable bounds
        // Cap at 500px to ensure higher LODs are used for large VDS files
        // For a 1176px slice: LOD1 gives 588px effective res, which is > 500
        // This provides good quality while being ~30x faster than full resolution
        if (maxSize < 100) maxSize = 100;
        if (maxSize > 500) maxSize = 500;

        LogPreviewFmt("Render: slice=%d, maxSize=%d (window: %dx%d)", m_sliceIndex, maxSize, totalWidth, totalHeight);

        // Use RAII pattern to ensure flag is always reset
        m_isRendering = true;
        struct RenderGuard {
            bool& flag;
            ~RenderGuard() { flag = false; }
        } guard{m_isRendering};

        HBITMAP newBitmap = m_renderer->RenderSlice(m_dimension, m_sliceIndex, maxSize);

        // Copy VdsRenderer debug messages to the global debug display
        const auto& vdsDebug = m_renderer->GetLastRenderDebugMessages();
        for (const auto& msg : vdsDebug)
        {
            char narrowBuf[256];
            WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, narrowBuf, sizeof(narrowBuf), nullptr, nullptr);
            LogPreviewFmt("[VDS] %s", narrowBuf);
        }

        if (newBitmap)
        {
            // Only delete old bitmap after new one is ready (reduces flicker)
            if (m_cachedBitmap)
            {
                DeleteObject(m_cachedBitmap);
            }
            m_cachedBitmap = newBitmap;
            m_cachedSliceIndex = m_sliceIndex;

            BITMAP bm;
            GetObject(m_cachedBitmap, sizeof(bm), &bm);
            wchar_t buf[64];
            swprintf_s(buf, L"OK: %dx%d", bm.bmWidth, bm.bmHeight);
            m_lastError = buf;

            // Progressive LOD: if refinement is pending, trigger another render
            if (m_renderer->IsRefinementPending())
            {
                // Invalidate to trigger another render cycle for better quality
                // Small delay allows the current frame to display first
                SetTimer(m_hwndPreview, 1, 10, nullptr);  // Timer ID 1, 10ms delay
            }
        }
        else
        {
            m_lastError = L"Render failed";
            // Keep old bitmap visible even if new render fails
        }
    }

    void DrawVdsInfoPanel(HDC hdc, RECT rc)
    {
        if (!m_renderer)
            return;

        ::SetBkMode(hdc, TRANSPARENT);

        HFONT hFont = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

        // Header
        RECT headerRect = { rc.left + 8, rc.top + 6, rc.right - 8, rc.top + 20 };
        ::SetTextColor(hdc, RGB(60, 120, 180));
        DrawTextA(hdc, "VDS Information", -1, &headerRect, DT_LEFT | DT_SINGLELINE);

        // Content
        ::SetTextColor(hdc, RGB(50, 50, 50));
        auto lines = m_renderer->GetMetadataLines();

        int lineHeight = 14;
        int y = rc.top + 26;
        int maxLines = (rc.bottom - rc.top - 32) / lineHeight;

        for (size_t i = 0; i < lines.size() && (int)i < maxLines; i++)
        {
            RECT textRect = { rc.left + 8, y, rc.right - 8, y + lineHeight };
            DrawTextW(hdc, lines[i].c_str(), -1, &textRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += lineHeight;
        }

        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);
    }

    void OnPaint()
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_hwndPreview, &ps);

        RECT rc;
        GetClientRect(m_hwndPreview, &rc);

        int totalWidth = rc.right - rc.left;
        int midX = totalWidth / 2;

        // Left panel: Debug log (white background)
        RECT leftRect = { rc.left, rc.top, midX - 2, rc.bottom };
        HBRUSH hbrWhite = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &leftRect, hbrWhite);
        DeleteObject(hbrWhite);

        // Right panel bounds (don't clear - bitmap will be drawn over it)
        RECT rightRect = { midX + 2, rc.top, rc.right, rc.bottom };

        // Divider
        RECT divider = { midX - 2, rc.top, midX + 2, rc.bottom };
        HBRUSH hbrDiv = CreateSolidBrush(RGB(180, 180, 180));
        FillRect(hdc, &divider, hbrDiv);
        DeleteObject(hbrDiv);

        // Draw debug messages on left (black text on white)
        DrawDebugMessages(hdc, leftRect, RGB(0, 0, 0));

        // Right panel layout: VDS info on top, bitmap in middle, status at bottom
        if (m_renderer)
        {
            // Update cached bitmap if needed (checks slice index internally)
            UpdateCachedBitmap();

            int infoHeight = 220;  // Height for VDS info section
            int statusHeight = 28;
            int bitmapAreaTop = rightRect.top + infoHeight;
            int bitmapAreaBottom = rightRect.bottom - statusHeight;

            // Top: VDS info panel (fill just this area)
            RECT infoRect = { rightRect.left, rightRect.top, rightRect.right, rightRect.top + infoHeight };
            HBRUSH hbrInfo = CreateSolidBrush(RGB(245, 245, 245));
            FillRect(hdc, &infoRect, hbrInfo);
            DeleteObject(hbrInfo);
            DrawVdsInfoPanel(hdc, infoRect);

            // Separator line
            RECT sepLine = { rightRect.left + 8, infoRect.bottom - 1, rightRect.right - 8, infoRect.bottom + 1 };
            HBRUSH hbrSep = CreateSolidBrush(RGB(200, 200, 200));
            FillRect(hdc, &sepLine, hbrSep);
            DeleteObject(hbrSep);

            // Middle: bitmap
            if (m_cachedBitmap)
            {
                BITMAP bm;
                GetObject(m_cachedBitmap, sizeof(bm), &bm);

                int availW = rightRect.right - rightRect.left - 20;
                int availH = bitmapAreaBottom - bitmapAreaTop - 10;
                if (availW > 0 && availH > 0)
                {
                    float scale = std::min((float)availW / bm.bmWidth, (float)availH / bm.bmHeight);
                    int destW = (int)(bm.bmWidth * scale);
                    int destH = (int)(bm.bmHeight * scale);
                    int destX = rightRect.left + (rightRect.right - rightRect.left - destW) / 2;
                    int destY = bitmapAreaTop + (bitmapAreaBottom - bitmapAreaTop - destH) / 2;

                    HDC hdcMem = CreateCompatibleDC(hdc);
                    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, m_cachedBitmap);
                    SetStretchBltMode(hdc, HALFTONE);
                    StretchBlt(hdc, destX, destY, destW, destH,
                              hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                    SelectObject(hdcMem, hOldBitmap);
                    DeleteDC(hdcMem);
                }
            }

            // Bottom: slice navigation status
            RECT statusRect = { rightRect.left, rightRect.bottom - statusHeight, rightRect.right, rightRect.bottom };
            HBRUSH hbrStatus = CreateSolidBrush(RGB(60, 60, 60));
            FillRect(hdc, &statusRect, hbrStatus);
            DeleteObject(hbrStatus);

            ::SetTextColor(hdc, RGB(220, 220, 220));
            ::SetBkMode(hdc, TRANSPARENT);
            wchar_t buf[128];
            if (m_renderer->GetDimensionality() >= 3)
            {
                const wchar_t* dimName = m_renderer->GetDimensionName(m_dimension);
                swprintf_s(buf, L"%s Slice %d / %d  |  Scroll to navigate",
                          dimName, m_sliceIndex + 1, m_renderer->GetSliceCount(m_dimension));
            }
            else
            {
                swprintf_s(buf, L"2D Data");
            }
            DrawTextW(hdc, buf, -1, &statusRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        EndPaint(m_hwndPreview, &ps);
    }

    void RenderSliceView(HDC hdc, RECT rc)
    {
        if (!m_cachedBitmap)
            UpdateCachedBitmap();

        ::SetTextColor(hdc, m_textColor);
        ::SetBkMode(hdc, TRANSPARENT);

        if (m_cachedBitmap)
        {
            BITMAP bm;
            GetObject(m_cachedBitmap, sizeof(bm), &bm);

            int statusHeight = 24;
            int availableHeight = (rc.bottom - rc.top) - statusHeight;
            int availableWidth = rc.right - rc.left;

            RECT destRect = CalculateAspectFitRect(bm.bmWidth, bm.bmHeight,
                                                   availableWidth, availableHeight);

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
        else
        {
            RECT msgRect = rc;
            msgRect.bottom -= 24;
            DrawTextW(hdc, m_lastError.c_str(), -1, &msgRect,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_WORD_ELLIPSIS);
        }

        // Status text at bottom
        wchar_t buf[128];
        if (m_renderer->GetDimensionality() == 2)
        {
            swprintf_s(buf, L"2D Data");
        }
        else
        {
            const wchar_t* dimName = m_renderer->GetDimensionName(m_dimension);
            swprintf_s(buf, L"%s Slice %d / %d  (scroll to navigate)",
                      dimName, m_sliceIndex + 1, m_renderer->GetSliceCount(m_dimension));
        }

        RECT textRect = rc;
        textRect.top = textRect.bottom - 22;
        DrawTextW(hdc, buf, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
