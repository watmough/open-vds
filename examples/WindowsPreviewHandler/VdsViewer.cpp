/****************************************************************************
** VDS Viewer - Standalone VDS file viewer application
**
** A Win32 application for viewing VDS files with slice navigation.
** This serves as both a UI prototype and a standalone VDS viewer application.
**
** Usage:
**   VdsViewer.exe [input.vds]
**
** If no file is specified, a File Open dialog will be shown.
****************************************************************************/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objidl.h>
#include <shlwapi.h>

#include "VdsRenderer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "msimg32.lib")  // For AlphaBlend

// ============================================================================
// Logging
// ============================================================================

static FILE* g_logFile = nullptr;

static void InitLogging()
{
    // Get the path to the source directory's logs folder
    // First try the exe directory, then fall back to a known path
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Find the directory containing the exe
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *lastSlash = L'\0';

    // Create timestamp for filename (ISO8601: YYYYMMDD-HHMMSS)
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t timestamp[32];
    swprintf_s(timestamp, L"%04d%02d%02d-%02d%02d%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // Try to find the logs directory relative to common build output locations
    wchar_t logPath[MAX_PATH];

    // Try: exe_dir\..\..\..\examples\WindowsPreviewHandler\logs
    swprintf_s(logPath, L"%s\\..\\..\\..\\examples\\WindowsPreviewHandler\\logs\\vdsviewer-%s.log", exePath, timestamp);
    g_logFile = _wfopen(logPath, L"w");

    if (!g_logFile)
    {
        // Try: exe_dir\..\..\..\..\examples\WindowsPreviewHandler\logs
        swprintf_s(logPath, L"%s\\..\\..\\..\\..\\examples\\WindowsPreviewHandler\\logs\\vdsviewer-%s.log", exePath, timestamp);
        g_logFile = _wfopen(logPath, L"w");
    }

    if (!g_logFile)
    {
        // Fallback to temp directory
        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        swprintf_s(logPath, L"%svdsviewer-%s.log", tempPath, timestamp);
        g_logFile = _wfopen(logPath, L"w");
    }

    if (g_logFile)
    {
        // Get current time
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_logFile, "=== VdsViewer Log Started %04d-%02d-%02d %02d:%02d:%02d ===\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(g_logFile, "Log file: %ls\n", logPath);
        fflush(g_logFile);
    }
}

static void CloseLogging()
{
    if (g_logFile)
    {
        fprintf(g_logFile, "=== VdsViewer Log Ended ===\n");
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

static void Log(const char* format, ...)
{
    if (!g_logFile) return;

    // Timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%02d:%02d:%02d.%03d] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    // Message
    va_list args;
    va_start(args, format);
    vfprintf(g_logFile, format, args);
    va_end(args);

    fprintf(g_logFile, "\n");
    fflush(g_logFile);
}

static void LogW(const wchar_t* format, ...)
{
    if (!g_logFile) return;

    // Timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%02d:%02d:%02d.%03d] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    // Message
    va_list args;
    va_start(args, format);
    vfwprintf(g_logFile, format, args);
    va_end(args);

    fprintf(g_logFile, "\n");
    fflush(g_logFile);
}

// ============================================================================
// Application state
// ============================================================================

struct AppState
{
    // Window
    HWND hwnd = nullptr;
    int windowWidth = 1024;
    int windowHeight = 768;

    // Splitter
    float splitterRatio = 0.65f;  // Ratio of top pane (0.0 to 1.0)
    bool draggingSplitter = false;
    static constexpr int SPLITTER_HEIGHT = 6;
    static constexpr int MIN_PANE_HEIGHT = 100;

    // VDS renderer (handles all VDS operations)
    std::unique_ptr<VdsRenderer> renderer;
    std::wstring filePath;
    std::wstring fileName;

    // Current view state
    int currentDimension = 2;  // Default to depth/time slices
    int currentSlice = 0;
    int sliceCount = 1;

    // Rendered bitmap
    HBITMAP hBitmap = nullptr;
    int bitmapWidth = 0;
    int bitmapHeight = 0;

    // LOD refinement timer
    static constexpr UINT_PTR LOD_REFINE_TIMER_ID = 1;
    static constexpr UINT LOD_REFINE_DELAY_MS = 50;

    // UI interaction debounce timer
    static constexpr UINT_PTR UI_DEBOUNCE_TIMER_ID = 2;
    static constexpr UINT UI_DEBOUNCE_DELAY_MS = 200;
    bool uiPendingReopen = false;  // Need to reopen file after debounce

    // Temporary overlay text (shown while adjusting controls)
    std::wstring overlayText;
    bool showOverlay = false;

    // VDS info for bottom pane (two columns)
    std::vector<std::wstring> metadataLines;
    std::vector<std::wstring> technicalLines;

    // Status
    std::wstring statusText;
    bool needsRedraw = true;

    // Options
    bool preferLODs = false;  // --prefer-lods command line option

    // Wavelet adaptive compression options
    OpenVDS::WaveletAdaptiveMode waveletAdaptiveMode = OpenVDS::WaveletAdaptiveMode::BestQuality;
    float waveletAdaptiveTolerance = 0.1f;   // --tolerance <value>
    float waveletAdaptiveRatio = 10.0f;      // --ratio <value>

    // UI control regions (computed during paint)
    RECT uiShowRegion = {};      // "Show" control region
    RECT uiQualityRegion = {};   // "Quality" control region
    RECT uiLodRegion = {};       // "Target LOD" control region
    bool uiIsVertical = false;   // True if UI is drawn vertically (left/right whitespace)

    // Target LOD (0 = auto/best for display size)
    int targetLOD = 0;
    int maxAvailableLOD = 0;

    // LOD refinement control
    bool lodRefinementEnabled = true;  // Disabled when manually adjusting LOD or quality
};

static AppState g_app;

// ============================================================================
// Forward declarations
// ============================================================================

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool OpenVdsFile(const std::wstring& path);
void CloseVdsFile();
bool RenderCurrentSlice();
void UpdateWindowTitle();
void UpdateStatusText();
void UpdateVdsInfoText();
bool ShowOpenFileDialog(std::wstring& outPath);

// ============================================================================
// Rendering (uses VdsRenderer)
// ============================================================================

bool RenderCurrentSlice()
{
    Log("RenderCurrentSlice: entry, renderer=%p", g_app.renderer.get());

    if (!g_app.renderer)
    {
        Log("RenderCurrentSlice: no renderer");
        return false;
    }

    Log("RenderCurrentSlice: rendering slice=%d/%d, dim=%d",
        g_app.currentSlice, g_app.sliceCount, g_app.currentDimension);

    // Keep old bitmap until we successfully create a new one
    HBITMAP oldBitmap = g_app.hBitmap;

    // Use VdsRenderer to render the slice
    // maxSize of 2048 is plenty for typical window sizes
    // LOD refinement is disabled when manually adjusting LOD or quality settings
    HBITMAP newBitmap = g_app.renderer->RenderSlice(
        g_app.currentDimension,
        g_app.currentSlice,
        2048,   // maxSize
        g_app.lodRefinementEnabled  // useProgressiveLOD
    );

    if (newBitmap)
    {
        // Get bitmap dimensions
        BITMAP bm;
        GetObject(newBitmap, sizeof(bm), &bm);

        // Success - delete old bitmap and use new one
        if (oldBitmap)
            DeleteObject(oldBitmap);
        g_app.hBitmap = newBitmap;
        g_app.bitmapWidth = bm.bmWidth;
        g_app.bitmapHeight = bm.bmHeight;
        UpdateStatusText();
        Log("RenderCurrentSlice: SUCCESS - bitmap %dx%d", bm.bmWidth, bm.bmHeight);

        // If refinement is pending, set timer to trigger another render
        if (g_app.renderer->IsRefinementPending())
        {
            Log("RenderCurrentSlice: refinement pending, setting timer");
            SetTimer(g_app.hwnd, g_app.LOD_REFINE_TIMER_ID, g_app.LOD_REFINE_DELAY_MS, nullptr);
        }
        else
        {
            // Kill any existing refinement timer
            KillTimer(g_app.hwnd, g_app.LOD_REFINE_TIMER_ID);
        }

        return true;
    }

    // Failed - keep old bitmap
    Log("RenderCurrentSlice: FAILED - RenderSlice returned null");
    g_app.statusText = L"Failed to render slice";
    return false;
}

// ============================================================================
// VDS file operations
// ============================================================================

bool OpenVdsFile(const std::wstring& path)
{
    LogW(L"OpenVdsFile: entry, path=%s", path.c_str());
    Log("OpenVdsFile: calling CloseVdsFile first...");
    CloseVdsFile();

    // Convert to narrow string for OpenVDS
    char narrowPath[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrowPath, MAX_PATH, nullptr, nullptr);

    // Create VdsRenderer and configure settings BEFORE Initialize
    Log("OpenVdsFile: creating VdsRenderer...");
    g_app.renderer = std::make_unique<VdsRenderer>();

    // Apply wavelet adaptive compression settings (must be set before Initialize)
    g_app.renderer->SetWaveletAdaptiveMode(g_app.waveletAdaptiveMode);
    g_app.renderer->SetWaveletAdaptiveTolerance(g_app.waveletAdaptiveTolerance);
    g_app.renderer->SetWaveletAdaptiveRatio(g_app.waveletAdaptiveRatio);
    Log("OpenVdsFile: adaptiveMode=%d, tolerance=%.3f, ratio=%.1f",
        (int)g_app.waveletAdaptiveMode, g_app.waveletAdaptiveTolerance, g_app.waveletAdaptiveRatio);

    // Apply prefer-LODs setting (used during rendering, not opening)
    g_app.renderer->SetPreferLODs(g_app.preferLODs);
    Log("OpenVdsFile: preferLODs=%d", g_app.preferLODs ? 1 : 0);

    Log("OpenVdsFile: calling VdsRenderer::Initialize(%s)...", narrowPath);
    if (!g_app.renderer->Initialize(narrowPath))
    {
        Log("OpenVdsFile: FAILED - VdsRenderer::Initialize returned false");
        MessageBoxW(g_app.hwnd, L"Failed to open VDS file", L"Error", MB_OK | MB_ICONERROR);
        g_app.renderer.reset();
        return false;
    }
    Log("OpenVdsFile: VdsRenderer::Initialize succeeded");

    g_app.filePath = path;

    // Extract filename
    size_t lastSlash = path.find_last_of(L"\\/");
    g_app.fileName = (lastSlash != std::wstring::npos) ? path.substr(lastSlash + 1) : path;

    // Set up initial view
    int dimensionality = g_app.renderer->GetDimensionality();
    g_app.currentDimension = (dimensionality >= 3) ? 2 : 0;
    g_app.sliceCount = g_app.renderer->GetSliceCount(g_app.currentDimension);
    g_app.currentSlice = g_app.renderer->GetDefaultSliceIndex(g_app.currentDimension);

    Log("OpenVdsFile: dimensionality=%d, currentDim=%d, sliceCount=%d, currentSlice=%d",
        dimensionality, g_app.currentDimension, g_app.sliceCount, g_app.currentSlice);

    // Apply user's target LOD if LOD refinement was disabled (manual LOD control)
    // This must happen BEFORE RenderCurrentSlice so the renderer uses the correct LOD
    if (!g_app.lodRefinementEnabled)
    {
        g_app.renderer->SetUserTargetLOD(g_app.targetLOD);
        Log("OpenVdsFile: applied user target LOD=%d (refinement disabled)", g_app.targetLOD);
    }

    UpdateWindowTitle();
    Log("OpenVdsFile: calling UpdateVdsInfoText...");
    UpdateVdsInfoText();
    Log("OpenVdsFile: calling RenderCurrentSlice...");
    RenderCurrentSlice();
    g_app.needsRedraw = true;

    Log("OpenVdsFile: SUCCESS");
    return true;
}

void CloseVdsFile()
{
    Log("CloseVdsFile: entry, renderer=%p", g_app.renderer.get());

    // Wait for any in-progress rendering to complete
    if (g_app.renderer)
    {
        Log("CloseVdsFile: waiting for pending render...");
        g_app.renderer->WaitForPendingRender();
        Log("CloseVdsFile: destroying renderer");
        g_app.renderer.reset();
    }

    if (g_app.hBitmap)
    {
        Log("CloseVdsFile: deleting bitmap");
        DeleteObject(g_app.hBitmap);
        g_app.hBitmap = nullptr;
    }

    g_app.filePath.clear();
    g_app.fileName.clear();
    g_app.bitmapWidth = 0;
    g_app.bitmapHeight = 0;
    g_app.currentSlice = 0;
    g_app.sliceCount = 1;
    g_app.metadataLines.clear();
    g_app.technicalLines.clear();
    Log("CloseVdsFile: done");
}

// ============================================================================
// UI helpers
// ============================================================================

void UpdateWindowTitle()
{
    std::wstring title = L"VDS Viewer";
    if (!g_app.fileName.empty())
    {
        title += L" - " + g_app.fileName;
    }
    SetWindowTextW(g_app.hwnd, title.c_str());
}

void UpdateStatusText()
{
    if (!g_app.renderer)
    {
        g_app.statusText = L"No file loaded. Press Ctrl+O to open a VDS file.";
        return;
    }

    const wchar_t* dimName = g_app.renderer->GetDimensionName(g_app.currentDimension);
    std::wstring dimNameW = dimName ? dimName : L"Dimension";

    wchar_t buf[512];

    // Get dimension group and adaptive mode info from renderer
    std::wstring extraInfo;
    if (g_app.renderer)
    {
        const std::wstring& groupName = g_app.renderer->GetLastDimensionGroup();
        int lodCount = g_app.renderer->GetLastLODCount();
        int selectedLOD = g_app.renderer->GetLastSelectedLOD();

        wchar_t infoBuf[128];
        if (!groupName.empty())
        {
            swprintf_s(infoBuf, L"  |  %s LOD%d/%d", groupName.c_str(), selectedLOD, lodCount);
            extraInfo = infoBuf;
        }

        // Show adaptive mode
        auto mode = g_app.renderer->GetWaveletAdaptiveMode();
        if (mode == OpenVDS::WaveletAdaptiveMode::Tolerance)
        {
            swprintf_s(infoBuf, L"  |  Tol:%.2f", g_app.renderer->GetWaveletAdaptiveTolerance());
            extraInfo += infoBuf;
        }
        else if (mode == OpenVDS::WaveletAdaptiveMode::Ratio)
        {
            swprintf_s(infoBuf, L"  |  Ratio:%.0f", g_app.renderer->GetWaveletAdaptiveRatio());
            extraInfo += infoBuf;
        }
    }

    swprintf_s(buf, L"%s: %d / %d  |  %dx%d%s  |  Wheel: ±10 (Shift: ±1)  |  1/2/3: dim",
        dimNameW.c_str(),
        g_app.currentSlice + 1,
        g_app.sliceCount,
        g_app.bitmapWidth,
        g_app.bitmapHeight,
        extraInfo.c_str());
    g_app.statusText = buf;
}

void UpdateVdsInfoText()
{
    g_app.metadataLines.clear();
    g_app.technicalLines.clear();

    if (!g_app.renderer)
    {
        g_app.metadataLines.push_back(L"No VDS file loaded.");
        g_app.metadataLines.push_back(L"Press Ctrl+O to open a file,");
        g_app.metadataLines.push_back(L"or drag and drop a .vds file.");
        return;
    }

    // Add filename first
    g_app.metadataLines.push_back(L"File: " + g_app.fileName);

    // Get metadata lines from renderer
    auto metadata = g_app.renderer->GetMetadataLines();
    for (const auto& line : metadata)
        g_app.metadataLines.push_back(line);

    // Get technical info lines from renderer
    g_app.technicalLines = g_app.renderer->GetTechnicalInfoLines();
}

bool ShowOpenFileDialog(std::wstring& outPath)
{
    wchar_t filename[MAX_PATH] = L"";

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_app.hwnd;
    ofn.lpstrFilter = L"VDS Files (*.vds)\0*.vds\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open VDS File";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&ofn))
    {
        outPath = filename;
        return true;
    }
    return false;
}

// ============================================================================
// UI Controls Drawing and Interaction
// ============================================================================

// Draw a simple text area with no background, white text with shadow
static void DrawTextArea(HDC hdc, HFONT hFont, const RECT& rect, const wchar_t* text)
{
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);

    // Draw shadow
    RECT shadowRect = rect;
    OffsetRect(&shadowRect, 2, 2);
    SetTextColor(hdc, RGB(0, 0, 0));
    DrawTextW(hdc, text, -1, &shadowRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Draw text in white
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, text, -1, (LPRECT)&rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, hOldFont);
}

// Get current show dimension text - uses actual dimension name from VDS
static const wchar_t* GetShowText()
{
    if (g_app.renderer)
        return g_app.renderer->GetDimensionName(g_app.currentDimension);
    return L"--";
}

// Get current quality mode text
static std::wstring GetQualityText()
{
    wchar_t buf[32];
    switch (g_app.waveletAdaptiveMode)
    {
        case OpenVDS::WaveletAdaptiveMode::BestQuality:
            return L"Best";
        case OpenVDS::WaveletAdaptiveMode::Tolerance:
            swprintf_s(buf, L"Tol %.2f", g_app.waveletAdaptiveTolerance);
            return buf;
        case OpenVDS::WaveletAdaptiveMode::Ratio:
            swprintf_s(buf, L"Ratio %.0f", g_app.waveletAdaptiveRatio);
            return buf;
        default:
            return L"Best";
    }
}

// Get current LOD text
static std::wstring GetLodText()
{
    wchar_t buf[16];
    swprintf_s(buf, L"LOD %d", g_app.targetLOD);
    return buf;
}

// Measure text width with given font
static int MeasureTextWidth(HDC hdc, HFONT hFont, const wchar_t* text)
{
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    SIZE sz;
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
    SelectObject(hdc, hOldFont);
    return sz.cx;
}

// Draw UI controls - simple text areas showing current selection
// Always draws at bottom-left or left side, overlaying seismic if needed
static void DrawUIControls(HDC hdc, HDC hdcScreen, int x, int y, int availableHeight, bool vertical)
{
    if (!g_app.renderer)
        return;

    // Create font (32pt = 43 pixels at 96 DPI, bold)
    HFONT hFont = CreateFontW(-43, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    int textHeight = 54;
    int labelHeight = 54;
    int sectionGap = 12;
    int padding = 20;  // Horizontal padding for text area
    int bgPadding = 10; // Padding around all controls for background
    int inset = 40;     // Inset about a letter's width

    g_app.maxAvailableLOD = g_app.renderer->GetLastLODCount() - 1;

    // Get current text values
    const wchar_t* showText = GetShowText();
    std::wstring qualText = GetQualityText();
    std::wstring lodText = GetLodText();

    // Measure text widths
    int showWidth = MeasureTextWidth(hdc, hFont, showText) + padding;
    int qualWidth = MeasureTextWidth(hdc, hFont, qualText.c_str()) + padding;
    int lodWidth = MeasureTextWidth(hdc, hFont, lodText.c_str()) + padding;

    // Measure label widths
    int showLabelWidth = MeasureTextWidth(hdc, hFont, L"Show") + 4;
    int qualLabelWidth = MeasureTextWidth(hdc, hFont, L"Quality") + 4;
    int lodLabelWidth = MeasureTextWidth(hdc, hFont, L"Target LOD") + 4;

    // Calculate max width for uniform hitboxes
    int maxControlWidth = std::max({showWidth, qualWidth, lodWidth, showLabelWidth, qualLabelWidth, lodLabelWidth});

    // Calculate total bounds for background and draw it first
    RECT bgRect;
    if (vertical)
    {
        int totalHeight = 3 * (labelHeight + textHeight) + 2 * sectionGap;
        int bgY = availableHeight - totalHeight - 12 - bgPadding;
        bgRect = { x + inset - bgPadding, bgY, x + inset + maxControlWidth + bgPadding * 2, availableHeight - 12 + bgPadding };
    }
    else
    {
        int totalWidth = 3 * maxControlWidth + 2 * sectionGap;
        bgRect = { x + inset - bgPadding, y - bgPadding, x + inset + totalWidth + bgPadding, y + labelHeight + textHeight + bgPadding };
    }

    // Draw semi-transparent background
    {
        int bgWidth = bgRect.right - bgRect.left;
        int bgHeight = bgRect.bottom - bgRect.top;

        HDC hdcAlpha = CreateCompatibleDC(hdcScreen);
        HBITMAP hbmAlpha = CreateCompatibleBitmap(hdcScreen, bgWidth, bgHeight);
        HBITMAP hbmOldAlpha = (HBITMAP)SelectObject(hdcAlpha, hbmAlpha);

        RECT fillRect = { 0, 0, bgWidth, bgHeight };
        HBRUSH hBlackBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdcAlpha, &fillRect, hBlackBrush);
        DeleteObject(hBlackBrush);

        BLENDFUNCTION bf = {};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 25;  // 10% opacity
        AlphaBlend(hdc, bgRect.left, bgRect.top, bgWidth, bgHeight,
                   hdcAlpha, 0, 0, bgWidth, bgHeight, bf);

        SelectObject(hdcAlpha, hbmOldAlpha);
        DeleteObject(hbmAlpha);
        DeleteDC(hdcAlpha);
    }

    if (vertical)
    {
        // Vertical layout (left side, bottom-aligned)
        int curX = x + inset;
        int totalHeight = 3 * (labelHeight + textHeight) + 2 * sectionGap;
        int curY = availableHeight - totalHeight - 12;

        // === Show Section ===
        SetBkMode(hdc, TRANSPARENT);
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

        // Draw label with shadow (white like controls)
        RECT labelRect = { curX, curY, curX + maxControlWidth, curY + labelHeight };
        RECT shadowRect = labelRect;
        OffsetRect(&shadowRect, 2, 2);
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, L"Show", -1, &shadowRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, L"Show", -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        g_app.uiShowRegion = { curX, curY, curX + maxControlWidth, curY + labelHeight + textHeight };
        RECT showRect = { curX, curY + labelHeight, curX + maxControlWidth, curY + labelHeight + textHeight };
        DrawTextArea(hdc, hFont, showRect, showText);
        curY += labelHeight + textHeight + sectionGap;

        // === Quality Section ===
        labelRect = { curX, curY, curX + maxControlWidth, curY + labelHeight };
        shadowRect = labelRect;
        OffsetRect(&shadowRect, 2, 2);
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, L"Quality", -1, &shadowRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, L"Quality", -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        g_app.uiQualityRegion = { curX, curY, curX + maxControlWidth, curY + labelHeight + textHeight };
        RECT qualRect = { curX, curY + labelHeight, curX + maxControlWidth, curY + labelHeight + textHeight };
        DrawTextArea(hdc, hFont, qualRect, qualText.c_str());
        curY += labelHeight + textHeight + sectionGap;

        // === Target LOD Section ===
        labelRect = { curX, curY, curX + maxControlWidth, curY + labelHeight };
        shadowRect = labelRect;
        OffsetRect(&shadowRect, 2, 2);
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, L"Target LOD", -1, &shadowRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, L"Target LOD", -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        g_app.uiLodRegion = { curX, curY, curX + maxControlWidth, curY + labelHeight + textHeight };
        RECT lodRect = { curX, curY + labelHeight, curX + maxControlWidth, curY + labelHeight + textHeight };
        DrawTextArea(hdc, hFont, lodRect, lodText.c_str());

        SelectObject(hdc, hOldFont);
    }
    else
    {
        // Horizontal layout (bottom)
        int curX = x + inset;
        int labelY = y;
        int textY = y + labelHeight;

        // === Show Section ===
        SetBkMode(hdc, TRANSPARENT);
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

        // Draw label with shadow (white like controls)
        RECT labelRect = { curX, labelY, curX + maxControlWidth, labelY + labelHeight };
        RECT shadowRect = labelRect;
        OffsetRect(&shadowRect, 2, 2);
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, L"Show", -1, &shadowRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, L"Show", -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        g_app.uiShowRegion = { curX, labelY, curX + maxControlWidth, textY + textHeight };
        RECT showRect = { curX, textY, curX + maxControlWidth, textY + textHeight };
        DrawTextArea(hdc, hFont, showRect, showText);
        curX += maxControlWidth + sectionGap;

        // === Quality Section ===
        labelRect = { curX, labelY, curX + maxControlWidth, labelY + labelHeight };
        shadowRect = labelRect;
        OffsetRect(&shadowRect, 2, 2);
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, L"Quality", -1, &shadowRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, L"Quality", -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        g_app.uiQualityRegion = { curX, labelY, curX + maxControlWidth, textY + textHeight };
        RECT qualRect = { curX, textY, curX + maxControlWidth, textY + textHeight };
        DrawTextArea(hdc, hFont, qualRect, qualText.c_str());
        curX += maxControlWidth + sectionGap;

        // === Target LOD Section ===
        labelRect = { curX, labelY, curX + maxControlWidth, labelY + labelHeight };
        shadowRect = labelRect;
        OffsetRect(&shadowRect, 2, 2);
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, L"Target LOD", -1, &shadowRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, L"Target LOD", -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        g_app.uiLodRegion = { curX, labelY, curX + maxControlWidth, textY + textHeight };
        RECT lodRect = { curX, textY, curX + maxControlWidth, textY + textHeight };
        DrawTextArea(hdc, hFont, lodRect, lodText.c_str());

        SelectObject(hdc, hOldFont);
    }

    g_app.uiIsVertical = vertical;
    DeleteObject(hFont);
}

// Handle click on UI controls - cycle to next option, returns true if handled
static bool HandleUIClick(int x, int y)
{
    if (!g_app.renderer)
        return false;

    POINT pt = { x, y };

    // Check Show text area - cycle through dimensions (only for 3D+ data)
    if (PtInRect(&g_app.uiShowRegion, pt))
    {
        int dimensionality = g_app.renderer->GetDimensionality();
        // Only allow dimension cycling for 3D data
        if (dimensionality < 3)
            return true;  // Consume click but don't change anything

        // Cycle: 2 -> 1 -> 0 -> 2 (Inline -> Xline -> Z-slice -> Inline)
        int newDim = g_app.currentDimension - 1;
        if (newDim < 0) newDim = dimensionality - 1;

        // Re-enable LOD refinement and reset to auto LOD for normal navigation
        g_app.lodRefinementEnabled = true;
        g_app.targetLOD = 0;
        g_app.renderer->SetUserTargetLOD(-1);  // Auto LOD
        g_app.currentDimension = newDim;
        g_app.sliceCount = g_app.renderer->GetSliceCount(g_app.currentDimension);
        g_app.currentSlice = g_app.sliceCount / 2;
        RenderCurrentSlice();
        return true;
    }

    // Check Quality text area - cycle through modes
    if (PtInRect(&g_app.uiQualityRegion, pt))
    {
        // Cycle: Best -> Tolerance -> Ratio -> Best
        switch (g_app.waveletAdaptiveMode)
        {
            case OpenVDS::WaveletAdaptiveMode::BestQuality:
                g_app.waveletAdaptiveMode = OpenVDS::WaveletAdaptiveMode::Tolerance;
                break;
            case OpenVDS::WaveletAdaptiveMode::Tolerance:
                g_app.waveletAdaptiveMode = OpenVDS::WaveletAdaptiveMode::Ratio;
                break;
            case OpenVDS::WaveletAdaptiveMode::Ratio:
                g_app.waveletAdaptiveMode = OpenVDS::WaveletAdaptiveMode::BestQuality;
                break;
        }
        // Show overlay with new value
        g_app.overlayText = GetQualityText();
        g_app.showOverlay = true;
        // Disable LOD refinement when adjusting quality - makes effect clear
        g_app.lodRefinementEnabled = false;
        // Set debounce timer to reopen file
        g_app.uiPendingReopen = true;
        KillTimer(g_app.hwnd, g_app.UI_DEBOUNCE_TIMER_ID);
        SetTimer(g_app.hwnd, g_app.UI_DEBOUNCE_TIMER_ID, g_app.UI_DEBOUNCE_DELAY_MS, nullptr);
        return true;
    }

    // Check LOD text area - cycle through LODs
    if (PtInRect(&g_app.uiLodRegion, pt))
    {
        // Disable LOD refinement when manually adjusting - makes effect clear
        g_app.lodRefinementEnabled = false;
        g_app.targetLOD = (g_app.targetLOD + 1) % (g_app.maxAvailableLOD + 1);
        // Show overlay with new value
        g_app.overlayText = GetLodText();
        g_app.showOverlay = true;
        // Tell renderer to use user's target LOD
        if (g_app.renderer)
            g_app.renderer->SetUserTargetLOD(g_app.targetLOD);
        RenderCurrentSlice();
        // Set debounce timer to clear overlay after delay
        KillTimer(g_app.hwnd, g_app.UI_DEBOUNCE_TIMER_ID);
        SetTimer(g_app.hwnd, g_app.UI_DEBOUNCE_TIMER_ID, g_app.UI_DEBOUNCE_DELAY_MS, nullptr);
        return true;
    }

    return false;
}

// Handle scroll wheel on UI controls, returns true if handled
static bool HandleUIScroll(int x, int y, int delta)
{
    if (!g_app.renderer)
        return false;

    POINT pt = { x, y };

    // Scroll on Show region changes dimension (only for 3D+ data)
    if (PtInRect(&g_app.uiShowRegion, pt))
    {
        int dimensionality = g_app.renderer->GetDimensionality();
        // Only allow dimension cycling for 3D data
        if (dimensionality < 3)
            return true;  // Consume scroll but don't change anything

        int newDim = g_app.currentDimension + ((delta > 0) ? 1 : -1);
        // Wrap around
        if (newDim < 0) newDim = dimensionality - 1;
        if (newDim >= dimensionality) newDim = 0;

        if (newDim != g_app.currentDimension)
        {
            // Re-enable LOD refinement and reset to auto LOD for normal navigation
            g_app.lodRefinementEnabled = true;
            g_app.targetLOD = 0;
            g_app.renderer->SetUserTargetLOD(-1);  // Auto LOD
            g_app.currentDimension = newDim;
            g_app.sliceCount = g_app.renderer->GetSliceCount(g_app.currentDimension);
            g_app.currentSlice = g_app.sliceCount / 2;
            RenderCurrentSlice();
        }
        return true;  // Always consume scroll when over this control
    }

    // Scroll on Quality region adjusts tolerance/ratio values or cycles mode
    if (PtInRect(&g_app.uiQualityRegion, pt))
    {
        bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool changed = false;

        if (g_app.waveletAdaptiveMode == OpenVDS::WaveletAdaptiveMode::Tolerance)
        {
            // Tolerance: 0.1 step, or 0.01 with shift
            float step = shiftPressed ? 0.01f : 0.1f;
            float newVal = g_app.waveletAdaptiveTolerance + ((delta > 0) ? -step : step);
            newVal = std::max(0.01f, std::min(1.0f, newVal));
            if (newVal != g_app.waveletAdaptiveTolerance)
            {
                g_app.waveletAdaptiveTolerance = newVal;
                changed = true;
            }
        }
        else if (g_app.waveletAdaptiveMode == OpenVDS::WaveletAdaptiveMode::Ratio)
        {
            // Ratio: 10 step, or 1 with shift
            float step = shiftPressed ? 1.0f : 10.0f;
            float newVal = g_app.waveletAdaptiveRatio + ((delta > 0) ? -step : step);
            newVal = std::max(1.0f, std::min(100.0f, newVal));
            if (newVal != g_app.waveletAdaptiveRatio)
            {
                g_app.waveletAdaptiveRatio = newVal;
                changed = true;
            }
        }
        else
        {
            // In Best mode, scroll cycles to next mode
            g_app.waveletAdaptiveMode = (delta > 0) ? OpenVDS::WaveletAdaptiveMode::Ratio : OpenVDS::WaveletAdaptiveMode::Tolerance;
            changed = true;
        }

        if (changed)
        {
            // Show overlay with new value
            g_app.overlayText = GetQualityText();
            g_app.showOverlay = true;
            // Disable LOD refinement when adjusting quality - makes effect clear
            g_app.lodRefinementEnabled = false;
            // Set debounce timer to reopen file and re-render
            g_app.uiPendingReopen = true;
            KillTimer(g_app.hwnd, g_app.UI_DEBOUNCE_TIMER_ID);
            SetTimer(g_app.hwnd, g_app.UI_DEBOUNCE_TIMER_ID, g_app.UI_DEBOUNCE_DELAY_MS, nullptr);
        }
        return true;  // Always consume scroll when over this control
    }

    // Scroll on LOD region changes target LOD
    if (PtInRect(&g_app.uiLodRegion, pt))
    {
        int newLOD = g_app.targetLOD + ((delta > 0) ? -1 : 1);
        newLOD = std::max(0, std::min(g_app.maxAvailableLOD, newLOD));
        if (newLOD != g_app.targetLOD)
        {
            // Disable LOD refinement when manually adjusting - makes effect clear
            g_app.lodRefinementEnabled = false;
            g_app.targetLOD = newLOD;
            // Show overlay with new value
            g_app.overlayText = GetLodText();
            g_app.showOverlay = true;
            // Tell renderer to use user's target LOD
            if (g_app.renderer)
                g_app.renderer->SetUserTargetLOD(g_app.targetLOD);
            RenderCurrentSlice();
            // Set debounce timer to clear overlay after delay
            KillTimer(g_app.hwnd, g_app.UI_DEBOUNCE_TIMER_ID);
            SetTimer(g_app.hwnd, g_app.UI_DEBOUNCE_TIMER_ID, g_app.UI_DEBOUNCE_DELAY_MS, nullptr);
        }
        return true;  // Always consume scroll when over this control
    }

    return false;
}

// ============================================================================
// Window procedure
// ============================================================================

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        return 0;

    case WM_DESTROY:
        CloseVdsFile();
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
        g_app.windowWidth = LOWORD(lParam);
        g_app.windowHeight = HIWORD(lParam);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_ERASEBKGND:
        return 1;  // We handle erasing in WM_PAINT

    case WM_PAINT:
    {
        Log("WM_PAINT: hBitmap=%p, bitmapSize=%dx%d, windowSize=%dx%d",
            g_app.hBitmap, g_app.bitmapWidth, g_app.bitmapHeight,
            g_app.windowWidth, g_app.windowHeight);

        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // Create back buffer
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdc, g_app.windowWidth, g_app.windowHeight);
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

        // Calculate layout
        int statusBarHeight = 30;
        int contentHeight = g_app.windowHeight - statusBarHeight;
        int topPaneHeight = (int)(contentHeight * g_app.splitterRatio) - g_app.SPLITTER_HEIGHT / 2;
        int splitterY = topPaneHeight;
        int bottomPaneY = splitterY + g_app.SPLITTER_HEIGHT;
        int bottomPaneHeight = g_app.windowHeight - statusBarHeight - bottomPaneY;

        // Fill entire background with white
        HBRUSH hBrush = CreateSolidBrush(RGB(255, 255, 255));
        RECT clientRect = { 0, 0, g_app.windowWidth, g_app.windowHeight };
        FillRect(hdcMem, &clientRect, hBrush);
        DeleteObject(hBrush);

        // === TOP PANE: Seismic bitmap ===
        int bitmapX = 0, bitmapY = 0, displayWidth = 0, displayHeight = 0;

        if (g_app.hBitmap)
        {
            HDC hdcBitmap = CreateCompatibleDC(hdc);
            HBITMAP hbmOldBitmap = (HBITMAP)SelectObject(hdcBitmap, g_app.hBitmap);

            // Calculate centered position with aspect ratio preservation
            int availableWidth = g_app.windowWidth;
            int availableHeight = topPaneHeight;

            float scaleX = (float)availableWidth / g_app.bitmapWidth;
            float scaleY = (float)availableHeight / g_app.bitmapHeight;
            float scale = std::min(scaleX, scaleY);

            displayWidth = (int)(g_app.bitmapWidth * scale);
            displayHeight = (int)(g_app.bitmapHeight * scale);
            bitmapX = (availableWidth - displayWidth) / 2;
            bitmapY = (topPaneHeight - displayHeight) / 2;

            // High-quality scaling
            SetStretchBltMode(hdcMem, HALFTONE);
            SetBrushOrgEx(hdcMem, 0, 0, nullptr);
            StretchBlt(hdcMem, bitmapX, bitmapY, displayWidth, displayHeight,
                       hdcBitmap, 0, 0, g_app.bitmapWidth, g_app.bitmapHeight, SRCCOPY);

            SelectObject(hdcBitmap, hbmOldBitmap);
            DeleteDC(hdcBitmap);

            // === UI CONTROLS ===
            // Always draw at bottom-left, overlaying seismic if needed
            // Use vertical layout on left if window is landscape, horizontal at bottom if portrait
            int uiHeight = 54 + 54 + 12;  // label + text + padding
            int uiY = topPaneHeight - uiHeight - 8;

            // Use horizontal layout if window is more portrait-oriented (swapped logic)
            if (topPaneHeight > g_app.windowWidth)
            {
                DrawUIControls(hdcMem, hdc, 0, uiY, uiHeight, false);
            }
            else
            {
                DrawUIControls(hdcMem, hdc, 0, 8, topPaneHeight, true);
            }

            // === OVERLAY TEXT (shown while adjusting controls) ===
            if (g_app.showOverlay && !g_app.overlayText.empty())
            {
                // Create large font (160pt = 213 pixels at 96 DPI, ~20% smaller than 200pt)
                HFONT hOverlayFont = CreateFontW(-213, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                HFONT hOldFont = (HFONT)SelectObject(hdcMem, hOverlayFont);

                // Measure text size
                RECT measureRect = { 0, 0, 0, 0 };
                DrawTextW(hdcMem, g_app.overlayText.c_str(), -1, &measureRect,
                         DT_CALCRECT | DT_SINGLELINE);
                int textWidth = measureRect.right - measureRect.left;
                int textHeight = measureRect.bottom - measureRect.top;

                // Center in top pane (not just bitmap area)
                int centerX = g_app.windowWidth / 2;
                int centerY = topPaneHeight / 2;
                int bgPaddingH = 30;  // Horizontal padding
                int bgPaddingV = 8;   // Vertical padding (just slightly taller than text)

                // Calculate background rectangle
                RECT bgRect = {
                    centerX - textWidth / 2 - bgPaddingH,
                    centerY - textHeight / 2 - bgPaddingV,
                    centerX + textWidth / 2 + bgPaddingH,
                    centerY + textHeight / 2 + bgPaddingV
                };

                // Draw 10% opaque black background sized to text
                {
                    int bgWidth = bgRect.right - bgRect.left;
                    int bgHeight = bgRect.bottom - bgRect.top;

                    HDC hdcAlpha = CreateCompatibleDC(hdc);
                    HBITMAP hbmAlpha = CreateCompatibleBitmap(hdc, bgWidth, bgHeight);
                    HBITMAP hbmOldAlpha = (HBITMAP)SelectObject(hdcAlpha, hbmAlpha);

                    // Fill with black
                    RECT fillRect = { 0, 0, bgWidth, bgHeight };
                    HBRUSH hBlackBrush = CreateSolidBrush(RGB(0, 0, 0));
                    FillRect(hdcAlpha, &fillRect, hBlackBrush);
                    DeleteObject(hBlackBrush);

                    // Alpha blend onto main buffer (10% opacity = 25/255)
                    BLENDFUNCTION bf = {};
                    bf.BlendOp = AC_SRC_OVER;
                    bf.SourceConstantAlpha = 25;  // 10% opacity
                    AlphaBlend(hdcMem, bgRect.left, bgRect.top, bgWidth, bgHeight,
                               hdcAlpha, 0, 0, bgWidth, bgHeight, bf);

                    SelectObject(hdcAlpha, hbmOldAlpha);
                    DeleteObject(hbmAlpha);
                    DeleteDC(hdcAlpha);
                }

                // Draw text centered in top pane
                RECT overlayRect = { 0, 0, g_app.windowWidth, topPaneHeight };
                SetBkMode(hdcMem, TRANSPARENT);

                // Draw shadow
                SetTextColor(hdcMem, RGB(0, 0, 0));
                RECT shadowRect = overlayRect;
                OffsetRect(&shadowRect, 3, 3);
                DrawTextW(hdcMem, g_app.overlayText.c_str(), -1, &shadowRect,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                // Draw text in white
                SetTextColor(hdcMem, RGB(255, 255, 255));
                DrawTextW(hdcMem, g_app.overlayText.c_str(), -1, &overlayRect,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hdcMem, hOldFont);
                DeleteObject(hOverlayFont);
            }
        }
        else
        {
            // No bitmap - show instructions in top pane
            SetBkMode(hdcMem, TRANSPARENT);
            SetTextColor(hdcMem, RGB(128, 128, 128));

            HFONT hFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);

            RECT textRect = { 0, 0, g_app.windowWidth, topPaneHeight };
            DrawTextW(hdcMem, L"Press Ctrl+O to open a VDS file\nor drag and drop a file here",
                     -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdcMem, hOldFont);
            DeleteObject(hFont);
        }

        // === SPLITTER BAR ===
        RECT splitterRect = { 0, splitterY, g_app.windowWidth, splitterY + g_app.SPLITTER_HEIGHT };
        hBrush = CreateSolidBrush(RGB(200, 200, 200));
        FillRect(hdcMem, &splitterRect, hBrush);
        DeleteObject(hBrush);

        // Draw grip lines on splitter
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
        HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);
        int gripY = splitterY + g_app.SPLITTER_HEIGHT / 2;
        int gripX1 = g_app.windowWidth / 2 - 20;
        int gripX2 = g_app.windowWidth / 2 + 20;
        MoveToEx(hdcMem, gripX1, gripY - 1, nullptr);
        LineTo(hdcMem, gripX2, gripY - 1);
        MoveToEx(hdcMem, gripX1, gripY + 1, nullptr);
        LineTo(hdcMem, gripX2, gripY + 1);
        SelectObject(hdcMem, hOldPen);
        DeleteObject(hPen);

        // === BOTTOM PANE: VDS Info (two columns) ===
        SetBkMode(hdcMem, TRANSPARENT);
        SetTextColor(hdcMem, RGB(0, 0, 0));

        HFONT hInfoFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        HFONT hOldInfoFont = (HFONT)SelectObject(hdcMem, hInfoFont);

        // Get text metrics for line height
        TEXTMETRICW tm;
        GetTextMetricsW(hdcMem, &tm);
        int lineHeight = tm.tmHeight + 2;

        // Left column: metadata
        int leftX = 10;
        int rightX = g_app.windowWidth / 2 + 10;
        int y = bottomPaneY + 5;

        for (const auto& line : g_app.metadataLines)
        {
            if (y + lineHeight > g_app.windowHeight - statusBarHeight - 5)
                break;
            TextOutW(hdcMem, leftX, y, line.c_str(), (int)line.length());
            y += lineHeight;
        }

        // Right column: technical info
        y = bottomPaneY + 5;
        for (const auto& line : g_app.technicalLines)
        {
            if (y + lineHeight > g_app.windowHeight - statusBarHeight - 5)
                break;
            TextOutW(hdcMem, rightX, y, line.c_str(), (int)line.length());
            y += lineHeight;
        }

        SelectObject(hdcMem, hOldInfoFont);
        DeleteObject(hInfoFont);

        // === STATUS BAR ===
        RECT statusRect = { 0, g_app.windowHeight - statusBarHeight, g_app.windowWidth, g_app.windowHeight };
        hBrush = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdcMem, &statusRect, hBrush);
        DeleteObject(hBrush);

        SetBkMode(hdcMem, TRANSPARENT);
        SetTextColor(hdcMem, RGB(0, 0, 0));

        HFONT hStatusFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        HFONT hOldStatusFont = (HFONT)SelectObject(hdcMem, hStatusFont);

        statusRect.left += 10;
        DrawTextW(hdcMem, g_app.statusText.c_str(), -1, &statusRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdcMem, hOldStatusFont);
        DeleteObject(hStatusFont);

        // Copy back buffer to screen
        BitBlt(hdc, 0, 0, g_app.windowWidth, g_app.windowHeight, hdcMem, 0, 0, SRCCOPY);

        // Cleanup
        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR:
    {
        // Check if mouse is over splitter
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);

        int statusBarHeight = 30;
        int contentHeight = g_app.windowHeight - statusBarHeight;
        int topPaneHeight = (int)(contentHeight * g_app.splitterRatio) - g_app.SPLITTER_HEIGHT / 2;
        int splitterY = topPaneHeight;

        if (pt.y >= splitterY && pt.y < splitterY + g_app.SPLITTER_HEIGHT)
        {
            SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN:
    {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        // Check UI controls first
        if (HandleUIClick(x, y))
        {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        int statusBarHeight = 30;
        int contentHeight = g_app.windowHeight - statusBarHeight;
        int topPaneHeight = (int)(contentHeight * g_app.splitterRatio) - g_app.SPLITTER_HEIGHT / 2;
        int splitterY = topPaneHeight;

        if (y >= splitterY && y < splitterY + g_app.SPLITTER_HEIGHT)
        {
            g_app.draggingSplitter = true;
            SetCapture(hwnd);
            return 0;
        }
        break;
    }

    case WM_LBUTTONUP:
    {
        if (g_app.draggingSplitter)
        {
            g_app.draggingSplitter = false;
            ReleaseCapture();
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
    {
        if (g_app.draggingSplitter)
        {
            int y = GET_Y_LPARAM(lParam);
            int statusBarHeight = 30;
            int contentHeight = g_app.windowHeight - statusBarHeight;

            // Calculate new ratio with constraints
            float newRatio = (float)(y + g_app.SPLITTER_HEIGHT / 2) / contentHeight;

            // Enforce minimum pane heights
            float minRatio = (float)g_app.MIN_PANE_HEIGHT / contentHeight;
            float maxRatio = 1.0f - minRatio;
            newRatio = std::max(minRatio, std::min(maxRatio, newRatio));

            if (newRatio != g_app.splitterRatio)
            {
                g_app.splitterRatio = newRatio;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        break;
    }

    case WM_MOUSEWHEEL:
    {
        Log("WM_MOUSEWHEEL: renderer=%p", g_app.renderer.get());
        if (!g_app.renderer) break;

        int delta = GET_WHEEL_DELTA_WPARAM(wParam);

        // Get mouse position in client coordinates
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);

        // Check UI controls first
        if (HandleUIScroll(pt.x, pt.y, delta))
        {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // Default slice navigation
        int step = (delta > 0) ? -10 : 10;

        // Shift for fine scrolling (1 slice at a time)
        if (GetKeyState(VK_SHIFT) & 0x8000)
            step = (delta > 0) ? -1 : 1;

        int newSlice = g_app.currentSlice + step;
        newSlice = std::max(0, std::min(g_app.sliceCount - 1, newSlice));

        Log("WM_MOUSEWHEEL: delta=%d, step=%d, currentSlice=%d -> newSlice=%d",
            delta, step, g_app.currentSlice, newSlice);

        if (newSlice != g_app.currentSlice)
        {
            // Re-enable LOD refinement for normal navigation
            g_app.lodRefinementEnabled = true;
            g_app.currentSlice = newSlice;
            Log("WM_MOUSEWHEEL: calling RenderCurrentSlice...");
            RenderCurrentSlice();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_KEYDOWN:
    {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        // Ctrl+O to open file
        if (ctrl && wParam == 'O')
        {
            std::wstring path;
            if (ShowOpenFileDialog(path))
            {
                OpenVdsFile(path);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }

        // Number keys to change dimension (1, 2, 3)
        if (g_app.renderer && wParam >= '1' && wParam <= '3')
        {
            int newDim = (int)(wParam - '1');
            int dimensionality = g_app.renderer->GetDimensionality();

            if (newDim < dimensionality && newDim != g_app.currentDimension)
            {
                // Re-enable LOD refinement for normal navigation
                g_app.lodRefinementEnabled = true;
                g_app.currentDimension = newDim;
                g_app.sliceCount = g_app.renderer->GetSliceCount(g_app.currentDimension);
                g_app.currentSlice = g_app.sliceCount / 2;
                RenderCurrentSlice();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        // Arrow keys for navigation
        if (g_app.renderer)
        {
            int step = 0;
            if (wParam == VK_UP || wParam == VK_LEFT) step = -1;
            if (wParam == VK_DOWN || wParam == VK_RIGHT) step = 1;
            if (wParam == VK_PRIOR) step = -10;  // Page Up
            if (wParam == VK_NEXT) step = 10;    // Page Down
            if (wParam == VK_HOME) { g_app.lodRefinementEnabled = true; g_app.currentSlice = 0; step = 0; RenderCurrentSlice(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (wParam == VK_END) { g_app.lodRefinementEnabled = true; g_app.currentSlice = g_app.sliceCount - 1; step = 0; RenderCurrentSlice(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }

            if (step != 0)
            {
                int newSlice = g_app.currentSlice + step;
                newSlice = std::max(0, std::min(g_app.sliceCount - 1, newSlice));
                if (newSlice != g_app.currentSlice)
                {
                    // Re-enable LOD refinement for normal navigation
                    g_app.lodRefinementEnabled = true;
                    g_app.currentSlice = newSlice;
                    RenderCurrentSlice();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }
        break;
    }

    case WM_TIMER:
    {
        if (wParam == g_app.LOD_REFINE_TIMER_ID)
        {
            Log("WM_TIMER: LOD refinement timer fired");
            KillTimer(hwnd, g_app.LOD_REFINE_TIMER_ID);
            if (g_app.renderer && g_app.renderer->IsRefinementPending())
            {
                RenderCurrentSlice();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        else if (wParam == g_app.UI_DEBOUNCE_TIMER_ID)
        {
            Log("WM_TIMER: UI debounce timer fired");
            KillTimer(hwnd, g_app.UI_DEBOUNCE_TIMER_ID);
            // Clear overlay - VDS will render now
            g_app.showOverlay = false;
            g_app.overlayText.clear();
            if (g_app.uiPendingReopen && !g_app.filePath.empty())
            {
                g_app.uiPendingReopen = false;
                std::wstring path = g_app.filePath;
                // Save current view state before reopening
                int savedDimension = g_app.currentDimension;
                int savedSlice = g_app.currentSlice;
                int savedTargetLOD = g_app.targetLOD;
                bool savedLodRefinementEnabled = g_app.lodRefinementEnabled;
                CloseVdsFile();
                OpenVdsFile(path);
                // Restore view state after reopen
                g_app.currentDimension = savedDimension;
                g_app.sliceCount = g_app.renderer ? g_app.renderer->GetSliceCount(savedDimension) : 1;
                g_app.currentSlice = std::min(savedSlice, g_app.sliceCount - 1);
                g_app.targetLOD = savedTargetLOD;
                g_app.lodRefinementEnabled = savedLodRefinementEnabled;
                if (g_app.renderer && !savedLodRefinementEnabled)
                    g_app.renderer->SetUserTargetLOD(savedTargetLOD);
                // Re-render with restored settings
                RenderCurrentSlice();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }

    case WM_DROPFILES:
    {
        Log("WM_DROPFILES: received");
        HDROP hDrop = (HDROP)wParam;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, path, MAX_PATH))
        {
            LogW(L"WM_DROPFILES: path=%s", path);
            Log("WM_DROPFILES: calling OpenVdsFile...");
            OpenVdsFile(path);
            Log("WM_DROPFILES: OpenVdsFile returned, calling InvalidateRect");
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        DragFinish(hDrop);
        Log("WM_DROPFILES: done");
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// WinMain
// ============================================================================

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow)
{
    // Initialize logging first
    InitLogging();
    Log("wWinMain: starting");
    if (lpCmdLine && lpCmdLine[0] != L'\0')
    {
        LogW(L"wWinMain: command line = %s", lpCmdLine);
    }
    else
    {
        Log("wWinMain: no command line arguments");
    }

    // Parse command line options
    std::wstring cmdLine = lpCmdLine ? lpCmdLine : L"";
    std::wstring vdsPath;

    // Helper to extract a float value following an option
    auto extractFloat = [&cmdLine](const wchar_t* option, float& outValue) -> bool {
        size_t pos = cmdLine.find(option);
        if (pos == std::wstring::npos)
            return false;

        size_t optLen = wcslen(option);
        size_t valueStart = cmdLine.find_first_not_of(L" \t", pos + optLen);
        if (valueStart == std::wstring::npos)
            return false;

        // Parse the float value
        wchar_t* endPtr = nullptr;
        float value = wcstof(cmdLine.c_str() + valueStart, &endPtr);
        if (endPtr == cmdLine.c_str() + valueStart)
            return false;  // No conversion

        outValue = value;

        // Remove option and value from command line
        size_t valueEnd = endPtr - cmdLine.c_str();
        cmdLine.erase(pos, valueEnd - pos);
        return true;
    };

    // Check for --prefer-lods option
    if (cmdLine.find(L"--prefer-lods") != std::wstring::npos)
    {
        g_app.preferLODs = true;
        Log("wWinMain: --prefer-lods enabled");
        size_t pos = cmdLine.find(L"--prefer-lods");
        cmdLine.erase(pos, 13);
    }

    // Check for --best-quality option (default, but explicit)
    if (cmdLine.find(L"--best-quality") != std::wstring::npos)
    {
        g_app.waveletAdaptiveMode = OpenVDS::WaveletAdaptiveMode::BestQuality;
        Log("wWinMain: --best-quality enabled");
        size_t pos = cmdLine.find(L"--best-quality");
        cmdLine.erase(pos, 14);
    }

    // Check for --tolerance <value> option (0.01 to 1.0)
    float tolerance = 0.0f;
    if (extractFloat(L"--tolerance", tolerance))
    {
        // Clamp to valid range
        tolerance = std::max(0.01f, std::min(1.0f, tolerance));
        g_app.waveletAdaptiveMode = OpenVDS::WaveletAdaptiveMode::Tolerance;
        g_app.waveletAdaptiveTolerance = tolerance;
        Log("wWinMain: --tolerance %.3f enabled", tolerance);
    }

    // Check for --ratio <value> option (1.0 to 100.0)
    float ratio = 0.0f;
    if (extractFloat(L"--ratio", ratio))
    {
        // Clamp to valid range
        ratio = std::max(1.0f, std::min(100.0f, ratio));
        g_app.waveletAdaptiveMode = OpenVDS::WaveletAdaptiveMode::Ratio;
        g_app.waveletAdaptiveRatio = ratio;
        Log("wWinMain: --ratio %.1f enabled", ratio);
    }

    // Trim whitespace from remaining command line (the VDS path)
    size_t start = cmdLine.find_first_not_of(L" \t\"");
    size_t end = cmdLine.find_last_not_of(L" \t\"");
    if (start != std::wstring::npos && end != std::wstring::npos)
        vdsPath = cmdLine.substr(start, end - start + 1);

    // Register window class
    Log("wWinMain: registering window class");
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"VdsViewerClass";
    // Load application icon from embedded resource (IDI_ICON1 = 1)
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    // Fallback to default if resource not found
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc))
    {
        Log("wWinMain: FAILED to register window class");
        MessageBoxW(nullptr, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        CloseLogging();
        return 1;
    }

    // Create window
    Log("wWinMain: creating window");
    g_app.hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,  // Accept drag and drop
        L"VdsViewerClass",
        L"VDS Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        g_app.windowWidth, g_app.windowHeight,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_app.hwnd)
    {
        Log("wWinMain: FAILED to create window");
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        CloseLogging();
        return 1;
    }
    Log("wWinMain: window created, hwnd=%p", g_app.hwnd);

    // Check for command line VDS file
    if (!vdsPath.empty())
    {
        Log("wWinMain: processing command line VDS file");
        LogW(L"wWinMain: calling OpenVdsFile with path: %s", vdsPath.c_str());
        OpenVdsFile(vdsPath);
        Log("wWinMain: OpenVdsFile returned");
    }

    Log("wWinMain: calling UpdateStatusText");
    UpdateStatusText();
    Log("wWinMain: calling UpdateVdsInfoText");
    UpdateVdsInfoText();
    Log("wWinMain: calling ShowWindow");
    ShowWindow(g_app.hwnd, nCmdShow);

    // Force a repaint after showing window (needed when VDS loaded from command line)
    Log("wWinMain: calling InvalidateRect and UpdateWindow");
    InvalidateRect(g_app.hwnd, nullptr, TRUE);
    UpdateWindow(g_app.hwnd);

    Log("wWinMain: entering message loop, hBitmap=%p", g_app.hBitmap);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Log("wWinMain: message loop exited, cleaning up");
    CloseLogging();
    return (int)msg.wParam;
}
