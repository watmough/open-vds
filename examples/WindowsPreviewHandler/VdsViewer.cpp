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

#include <OpenVDS/OpenVDS.h>
#include <OpenVDS/VolumeDataLayout.h>
#include <OpenVDS/VolumeDataAccess.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")

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

    // VDS data
    OpenVDS::VDSHandle vdsHandle = nullptr;
    OpenVDS::VolumeDataLayout* layout = nullptr;
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

    // VDS info text for bottom pane
    std::wstring vdsInfoText;

    // Status
    std::wstring statusText;
    bool needsRedraw = true;
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
// Colormap and bitmap creation
// ============================================================================

static HBITMAP CreateColorizedBitmap(const uint8_t* grayscaleData, int width, int height)
{
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!hBitmap)
        return nullptr;

    uint32_t* pixels = static_cast<uint32_t*>(pBits);

    // Blue-White-Red seismic colormap
    for (int i = 0; i < width * height; i++)
    {
        uint8_t value = grayscaleData[i];
        uint8_t r, g, b;

        if (value < 128)
        {
            // Blue to White (low values)
            float t = value / 128.0f;
            r = static_cast<uint8_t>(t * 255);
            g = static_cast<uint8_t>(t * 255);
            b = 255;
        }
        else
        {
            // White to Red (high values)
            float t = (value - 128) / 127.0f;
            r = 255;
            g = static_cast<uint8_t>((1.0f - t) * 255);
            b = static_cast<uint8_t>((1.0f - t) * 255);
        }

        pixels[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
    }

    return hBitmap;
}

// ============================================================================
// LOD size calculation
// ============================================================================

static inline int GetLODSize(int voxelMin, int voxelMax, int lod)
{
    return ((voxelMax - voxelMin - 1) >> lod) + 1;
}

// ============================================================================
// Rendering
// ============================================================================

bool RenderCurrentSlice()
{
    if (!g_app.vdsHandle || !g_app.layout)
        return false;

    // Keep old bitmap until we successfully create a new one
    HBITMAP oldBitmap = g_app.hBitmap;

    int dimensionality = g_app.layout->GetDimensionality();
    OpenVDS::VolumeDataAccessManager accessManager = OpenVDS::GetAccessManager(g_app.vdsHandle);

    // Set up voxel bounds
    int voxelMin[OpenVDS::Dimensionality_Max] = { 0, 0, 0, 0, 0, 0 };
    int voxelMax[OpenVDS::Dimensionality_Max] = { 1, 1, 1, 1, 1, 1 };

    int displayDims[OpenVDS::Dimensionality_Max - 1];
    int displayDimCount = 0;

    if (dimensionality == 2)
    {
        // 2D data - display entire slice
        displayDims[0] = 0;
        displayDims[1] = 1;
        displayDimCount = 2;
        voxelMin[0] = 0;
        voxelMax[0] = g_app.layout->GetDimensionNumSamples(0);
        voxelMin[1] = 0;
        voxelMax[1] = g_app.layout->GetDimensionNumSamples(1);
    }
    else
    {
        // 3D+ data - slice one dimension
        for (int dim = 0; dim < dimensionality; dim++)
        {
            if (dim == g_app.currentDimension)
            {
                voxelMin[dim] = g_app.currentSlice;
                voxelMax[dim] = g_app.currentSlice + 1;
            }
            else
            {
                voxelMin[dim] = 0;
                voxelMax[dim] = g_app.layout->GetDimensionNumSamples(dim);
                displayDims[displayDimCount++] = dim;
            }
        }

        // If 4D+, fix extra dimensions at midpoint
        if (displayDimCount > 2)
        {
            int dim = displayDims[2];
            int midpoint = g_app.layout->GetDimensionNumSamples(dim) / 2;
            voxelMin[dim] = midpoint;
            voxelMax[dim] = midpoint + 1;
        }
    }

    int dim0 = displayDims[0];
    int dim1 = displayDims[1];

    // Determine if we need to transpose (vertical axis should be dim0/samples)
    bool needsTranspose = false;
    if (dimensionality == 2)
    {
        auto axis0 = g_app.layout->GetAxisDescriptor(0);
        const char* axis0Name = axis0.GetName();
        if (axis0Name)
        {
            std::string name(axis0Name);
            for (auto& c : name) c = std::tolower(c);
            if (name.find("sample") != std::string::npos ||
                name.find("time") != std::string::npos ||
                name.find("depth") != std::string::npos ||
                name.find("z") != std::string::npos)
            {
                needsTranspose = true;
            }
        }
    }
    else
    {
        needsTranspose = (dim0 == 0);
    }

    // Calculate LOD-sized buffer dimensions
    auto layoutDescriptor = g_app.layout->GetLayoutDescriptor();
    int fullResDim = layoutDescriptor.GetFullResolutionDimension();
    int lod = 0;  // Use full resolution for now

    int lodDim0Size, lodDim1Size;
    if (fullResDim == dim0)
    {
        lodDim0Size = voxelMax[dim0] - voxelMin[dim0];
        lodDim1Size = GetLODSize(voxelMin[dim1], voxelMax[dim1], lod);
    }
    else if (fullResDim == dim1)
    {
        lodDim0Size = GetLODSize(voxelMin[dim0], voxelMax[dim0], lod);
        lodDim1Size = voxelMax[dim1] - voxelMin[dim1];
    }
    else
    {
        lodDim0Size = GetLODSize(voxelMin[dim0], voxelMax[dim0], lod);
        lodDim1Size = GetLODSize(voxelMin[dim1], voxelMax[dim1], lod);
    }

    int width = needsTranspose ? lodDim1Size : lodDim0Size;
    int height = needsTranspose ? lodDim0Size : lodDim1Size;

    // Get buffer size from API
    int64_t expectedBufferSize = accessManager.GetVolumeSubsetBufferSize<float>(voxelMin, voxelMax, lod, 0);
    size_t bufferFloatCount = static_cast<size_t>(expectedBufferSize / sizeof(float));
    std::vector<float> buffer(bufferFloatCount);

    // Find available dimension group
    OpenVDS::DimensionsND dimGroup = OpenVDS::Dimensions_012;
    OpenVDS::DimensionsND candidates[] = {
        OpenVDS::Dimensions_01,
        OpenVDS::Dimensions_012,
        OpenVDS::Dimensions_02
    };

    for (int i = 0; i < 3; i++)
    {
        auto status = accessManager.GetVDSProduceStatus(candidates[i], lod, 0);
        if (status != OpenVDS::VDSProduceStatus::Unavailable)
        {
            dimGroup = candidates[i];
            break;
        }
    }

    // Request the data
    auto request = accessManager.RequestVolumeSubset<float>(
        buffer.data(),
        buffer.size() * sizeof(float),
        dimGroup,
        lod, 0,
        voxelMin, voxelMax
    );

    if (!request->WaitForCompletion())
    {
        // Keep the old bitmap, show raw error in status
        std::string errMsg = request->GetErrorMessage();
        g_app.statusText = L"Slice " + std::to_wstring(g_app.currentSlice + 1) +
                          L"/" + std::to_wstring(g_app.sliceCount) +
                          L" - Error: " + std::wstring(errMsg.begin(), errMsg.end());
        return false;
    }

    // Compute min/max for normalization
    float minVal = buffer[0];
    float maxVal = buffer[0];

    for (size_t i = 0; i < buffer.size(); i++)
    {
        float v = buffer[i];
        if (std::isnan(v) || std::isinf(v)) continue;
        if (v < minVal) minVal = v;
        if (v > maxVal) maxVal = v;
    }

    if (maxVal - minVal < 1e-6f)
    {
        minVal = g_app.layout->GetChannelValueRangeMin(0);
        maxVal = g_app.layout->GetChannelValueRangeMax(0);
    }

    // Create grayscale buffer
    std::vector<uint8_t> grayscale(width * height);
    float range = maxVal - minVal;
    if (range < 1e-6f) range = 1.0f;

    if (needsTranspose)
    {
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                float v = buffer[x * lodDim0Size + y];
                if (!std::isfinite(v)) v = minVal;
                float normalized = (v - minVal) / range;
                normalized = std::max(0.0f, std::min(1.0f, normalized));
                grayscale[y * width + x] = static_cast<uint8_t>(normalized * 255.0f);
            }
        }
    }
    else
    {
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                float v = buffer[y * lodDim0Size + x];
                if (!std::isfinite(v)) v = minVal;
                float normalized = (v - minVal) / range;
                normalized = std::max(0.0f, std::min(1.0f, normalized));
                grayscale[y * width + x] = static_cast<uint8_t>(normalized * 255.0f);
            }
        }
    }

    // Create colorized bitmap
    HBITMAP newBitmap = CreateColorizedBitmap(grayscale.data(), width, height);
    if (newBitmap)
    {
        // Success - delete old bitmap and use new one
        if (oldBitmap)
            DeleteObject(oldBitmap);
        g_app.hBitmap = newBitmap;
        g_app.bitmapWidth = width;
        g_app.bitmapHeight = height;
        UpdateStatusText();
        return true;
    }

    // Failed to create bitmap - keep old one
    g_app.statusText = L"Failed to create bitmap";
    return false;
}

// ============================================================================
// VDS file operations
// ============================================================================

bool OpenVdsFile(const std::wstring& path)
{
    CloseVdsFile();

    // Convert to narrow string for OpenVDS
    char narrowPath[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrowPath, MAX_PATH, nullptr, nullptr);

    OpenVDS::Error error;
    g_app.vdsHandle = OpenVDS::Open(narrowPath, error);

    if (error.code != 0 || !g_app.vdsHandle)
    {
        MessageBoxW(g_app.hwnd,
            (L"Failed to open VDS file:\n" + std::wstring(error.string.begin(), error.string.end())).c_str(),
            L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    g_app.layout = OpenVDS::GetLayout(g_app.vdsHandle);
    if (!g_app.layout)
    {
        MessageBoxW(g_app.hwnd, L"Failed to get VDS layout", L"Error", MB_OK | MB_ICONERROR);
        OpenVDS::Close(g_app.vdsHandle, error);
        g_app.vdsHandle = nullptr;
        return false;
    }

    g_app.filePath = path;

    // Extract filename
    size_t lastSlash = path.find_last_of(L"\\/");
    g_app.fileName = (lastSlash != std::wstring::npos) ? path.substr(lastSlash + 1) : path;

    // Set up initial view
    int dimensionality = g_app.layout->GetDimensionality();
    g_app.currentDimension = (dimensionality >= 3) ? 2 : 0;
    g_app.sliceCount = g_app.layout->GetDimensionNumSamples(g_app.currentDimension);
    g_app.currentSlice = g_app.sliceCount / 2;

    UpdateWindowTitle();
    UpdateVdsInfoText();
    RenderCurrentSlice();
    g_app.needsRedraw = true;

    return true;
}

void CloseVdsFile()
{
    if (g_app.hBitmap)
    {
        DeleteObject(g_app.hBitmap);
        g_app.hBitmap = nullptr;
    }

    if (g_app.vdsHandle)
    {
        OpenVDS::Error error;
        OpenVDS::Close(g_app.vdsHandle, error);
        g_app.vdsHandle = nullptr;
    }

    g_app.layout = nullptr;
    g_app.filePath.clear();
    g_app.fileName.clear();
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
    if (!g_app.layout)
    {
        g_app.statusText = L"No file loaded. Press Ctrl+O to open a VDS file.";
        return;
    }

    auto axis = g_app.layout->GetAxisDescriptor(g_app.currentDimension);
    const char* axisName = axis.GetName();
    std::wstring axisNameW(axisName, axisName + strlen(axisName));

    wchar_t buf[256];
    swprintf_s(buf, L"%s: Slice %d / %d  |  Size: %dx%d  |  Mouse wheel: navigate  |  1/2/3: change dimension",
        axisNameW.c_str(),
        g_app.currentSlice + 1,
        g_app.sliceCount,
        g_app.bitmapWidth,
        g_app.bitmapHeight);
    g_app.statusText = buf;
}

void UpdateVdsInfoText()
{
    if (!g_app.layout)
    {
        g_app.vdsInfoText = L"No VDS file loaded.\r\nPress Ctrl+O to open a file, or drag and drop a .vds file.";
        return;
    }

    std::wstring info;
    wchar_t buf[512];

    // File name
    info += L"File: " + g_app.fileName + L"\r\n";

    // Dimensions (merged with dimensionality count)
    int dimensionality = g_app.layout->GetDimensionality();
    swprintf_s(buf, L"Dimensions: %d\r\n", dimensionality);
    info += buf;
    for (int dim = 0; dim < dimensionality; dim++)
    {
        auto axis = g_app.layout->GetAxisDescriptor(dim);
        const char* name = axis.GetName();
        const char* unit = axis.GetUnit();
        std::wstring nameW(name, name + strlen(name));
        std::wstring unitW(unit, unit + strlen(unit));

        swprintf_s(buf, L"  [%d] %s: %d samples (%.2f to %.2f %s)\r\n",
            dim,
            nameW.c_str(),
            g_app.layout->GetDimensionNumSamples(dim),
            axis.GetCoordinateMin(),
            axis.GetCoordinateMax(),
            unitW.c_str());
        info += buf;
    }

    // Channels
    int channelCount = g_app.layout->GetChannelCount();
    swprintf_s(buf, L"Channels: %d\r\n", channelCount);
    info += buf;
    for (int ch = 0; ch < channelCount; ch++)
    {
        auto channel = g_app.layout->GetChannelDescriptor(ch);
        const char* name = channel.GetName();
        std::wstring nameW(name, name + strlen(name));
        swprintf_s(buf, L"  [%d] %s\r\n", ch, nameW.c_str());
        info += buf;
    }

    // Available dimension groups with LODs
    info += L"Dimension Groups:\r\n";

    OpenVDS::VolumeDataAccessManager accessManager = OpenVDS::GetAccessManager(g_app.vdsHandle);
    auto layoutDesc = g_app.layout->GetLayoutDescriptor();
    int maxLOD = static_cast<int>(layoutDesc.GetLODLevels());
    if (maxLOD == 0) maxLOD = 1;

    struct DimGroupInfo {
        OpenVDS::DimensionsND group;
        const wchar_t* name;
    };
    DimGroupInfo allGroups[] = {
        { OpenVDS::Dimensions_01, L"Dimensions_01" },
        { OpenVDS::Dimensions_02, L"Dimensions_02" },
        { OpenVDS::Dimensions_03, L"Dimensions_03" },
        { OpenVDS::Dimensions_12, L"Dimensions_12" },
        { OpenVDS::Dimensions_13, L"Dimensions_13" },
        { OpenVDS::Dimensions_23, L"Dimensions_23" },
        { OpenVDS::Dimensions_012, L"Dimensions_012" },
        { OpenVDS::Dimensions_013, L"Dimensions_013" },
        { OpenVDS::Dimensions_023, L"Dimensions_023" },
        { OpenVDS::Dimensions_123, L"Dimensions_123" },
    };

    for (const auto& dg : allGroups)
    {
        // Check which LODs are available for this dimension group
        std::wstring availableLODs;
        for (int lod = 0; lod < maxLOD; lod++)
        {
            auto status = accessManager.GetVDSProduceStatus(dg.group, lod, 0);
            if (status != OpenVDS::VDSProduceStatus::Unavailable)
            {
                if (!availableLODs.empty()) availableLODs += L", ";
                availableLODs += std::to_wstring(lod);
            }
        }

        if (!availableLODs.empty())
        {
            swprintf_s(buf, L"  %s (LOD: %s)\r\n", dg.name, availableLODs.c_str());
            info += buf;
        }
    }

    g_app.vdsInfoText = info;
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

            int displayWidth = (int)(g_app.bitmapWidth * scale);
            int displayHeight = (int)(g_app.bitmapHeight * scale);
            int x = (availableWidth - displayWidth) / 2;
            int y = (topPaneHeight - displayHeight) / 2;

            // High-quality scaling
            SetStretchBltMode(hdcMem, HALFTONE);
            SetBrushOrgEx(hdcMem, 0, 0, nullptr);
            StretchBlt(hdcMem, x, y, displayWidth, displayHeight,
                       hdcBitmap, 0, 0, g_app.bitmapWidth, g_app.bitmapHeight, SRCCOPY);

            SelectObject(hdcBitmap, hbmOldBitmap);
            DeleteDC(hdcBitmap);
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

        // === BOTTOM PANE: VDS Info text ===
        RECT infoPaneRect = { 0, bottomPaneY, g_app.windowWidth, g_app.windowHeight - statusBarHeight };

        // Draw the text
        SetBkMode(hdcMem, TRANSPARENT);
        SetTextColor(hdcMem, RGB(0, 0, 0));

        HFONT hInfoFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        HFONT hOldInfoFont = (HFONT)SelectObject(hdcMem, hInfoFont);

        RECT textRect = infoPaneRect;
        textRect.left += 10;
        textRect.top += 5;
        textRect.right -= 10;
        textRect.bottom -= 5;
        DrawTextW(hdcMem, g_app.vdsInfoText.c_str(), -1, &textRect,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EXPANDTABS);

        SelectObject(hdcMem, hOldInfoFont);
        DeleteObject(hInfoFont);

        // === STATUS BAR ===
        RECT statusRect = { 0, g_app.windowHeight - statusBarHeight, g_app.windowWidth, g_app.windowHeight };
        hBrush = CreateSolidBrush(RGB(60, 60, 60));
        FillRect(hdcMem, &statusRect, hBrush);
        DeleteObject(hBrush);

        SetBkMode(hdcMem, TRANSPARENT);
        SetTextColor(hdcMem, RGB(220, 220, 220));

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
        int y = GET_Y_LPARAM(lParam);

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
        if (!g_app.layout) break;

        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int step = (delta > 0) ? -1 : 1;

        // Shift for faster scrolling
        if (GetKeyState(VK_SHIFT) & 0x8000)
            step *= 10;

        int newSlice = g_app.currentSlice + step;
        newSlice = std::max(0, std::min(g_app.sliceCount - 1, newSlice));

        if (newSlice != g_app.currentSlice)
        {
            g_app.currentSlice = newSlice;
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
        if (g_app.layout && wParam >= '1' && wParam <= '3')
        {
            int newDim = (int)(wParam - '1');
            int dimensionality = g_app.layout->GetDimensionality();

            if (newDim < dimensionality && newDim != g_app.currentDimension)
            {
                g_app.currentDimension = newDim;
                g_app.sliceCount = g_app.layout->GetDimensionNumSamples(g_app.currentDimension);
                g_app.currentSlice = g_app.sliceCount / 2;
                RenderCurrentSlice();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        // Arrow keys for navigation
        if (g_app.layout)
        {
            int step = 0;
            if (wParam == VK_UP || wParam == VK_LEFT) step = -1;
            if (wParam == VK_DOWN || wParam == VK_RIGHT) step = 1;
            if (wParam == VK_PRIOR) step = -10;  // Page Up
            if (wParam == VK_NEXT) step = 10;    // Page Down
            if (wParam == VK_HOME) { g_app.currentSlice = 0; step = 0; RenderCurrentSlice(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (wParam == VK_END) { g_app.currentSlice = g_app.sliceCount - 1; step = 0; RenderCurrentSlice(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }

            if (step != 0)
            {
                int newSlice = g_app.currentSlice + step;
                newSlice = std::max(0, std::min(g_app.sliceCount - 1, newSlice));
                if (newSlice != g_app.currentSlice)
                {
                    g_app.currentSlice = newSlice;
                    RenderCurrentSlice();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }
        break;
    }

    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, path, MAX_PATH))
        {
            OpenVdsFile(path);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        DragFinish(hDrop);
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
    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"VdsViewerClass";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc))
    {
        MessageBoxW(nullptr, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Create window
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
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Check for command line argument
    if (lpCmdLine && lpCmdLine[0] != L'\0')
    {
        // Remove quotes if present
        std::wstring path = lpCmdLine;
        if (path.front() == L'"' && path.back() == L'"')
        {
            path = path.substr(1, path.length() - 2);
        }
        OpenVdsFile(path);
    }

    UpdateStatusText();
    UpdateVdsInfoText();
    ShowWindow(g_app.hwnd, nCmdShow);
    UpdateWindow(g_app.hwnd);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
