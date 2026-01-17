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

#include "VdsRenderer.h"
#include "utils/running_stats.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

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

// Save HBITMAP to BMP file for debugging
static bool SaveBitmapToFile(HBITMAP hBitmap, const char* filepath)
{
    if (!hBitmap || !filepath)
        return false;

    BITMAP bm;
    GetObject(hBitmap, sizeof(bm), &bm);

    // Get bitmap bits
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = bm.bmWidth;
    bi.biHeight = bm.bmHeight;  // Positive = bottom-up DIB
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    int rowSize = ((bm.bmWidth * 32 + 31) / 32) * 4;
    int imageSize = rowSize * bm.bmHeight;

    std::vector<uint8_t> bits(imageSize);
    BITMAPINFO bmi = {};
    bmi.bmiHeader = bi;

    // Get the DIB bits (this flips the image to bottom-up format)
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBitmap);
    GetDIBits(hdcMem, hBitmap, 0, bm.bmHeight, bits.data(), &bmi, DIB_RGB_COLORS);
    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    // Write BMP file
    BITMAPFILEHEADER bf = {};
    bf.bfType = 0x4D42;  // "BM"
    bf.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imageSize;
    bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open())
        return false;

    file.write(reinterpret_cast<char*>(&bf), sizeof(bf));
    file.write(reinterpret_cast<char*>(&bi), sizeof(bi));
    file.write(reinterpret_cast<char*>(bits.data()), imageSize);
    file.close();

    return true;
}

// Default log file paths (lazily initialized)
static std::string GetThumbnailLogPath()
{
    static std::string path = GetLogFilePath("openvds-thumbnails.log");
    return path;
}

static std::string GetPreviewLogPath()
{
    static std::string path = GetLogFilePath("openvds-previewpane.log");
    return path;
}

// Append debug lines to log file
static void LogToFile(const std::vector<std::wstring>& lines, const std::string& logFilePath)
{
    if (logFilePath.empty())
        return;

    std::ofstream logFile(logFilePath, std::ios::app);
    if (!logFile.is_open())
        return;

    // Add timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[64];
    sprintf_s(timestamp, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
              st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    logFile << timestamp;

    for (const auto& line : lines)
    {
        // Convert wide string to UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 0)
        {
            std::string utf8(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, &utf8[0], len, nullptr, nullptr);
            logFile << utf8 << " | ";
        }
    }
    logFile << "\n";
    logFile.flush();
}

// Performance thresholds based on benchmark data:
// - 90% of renders complete in under 50ms
// - Target render time for thumbnails: < 100ms
// - Use higher LOD if slice voxels > 500K (roughly 700x700)
static constexpr int64_t VOXEL_THRESHOLD_FOR_LOD = 500000;  // Use higher LOD if slice has more voxels
static constexpr int MIN_LOD_DIMENSION = 128;  // Don't use LOD that would give < 128 pixels in smallest dim

// TEMPORARY: Disable LOD optimization until data quality issues are resolved
// Higher LODs appear to return incorrect/degraded data in some cases
static constexpr bool ENABLE_LOD_OPTIMIZATION = false;

// Data validation thresholds
static constexpr float MAX_INVALID_RATIO = 0.5f;    // Warn if more than 50% NaN/Inf values
static constexpr float MIN_VALUE_VARIANCE = 1e-10f; // Minimum variance to be considered valid data
static constexpr float MAX_VALID_MAGNITUDE = 1e30f; // Filter values with |v| > this magnitude

// Color mapping parameters - scale data to use mean +/- TRANSFER_FUNCTION_GAIN * stddev
static constexpr float TRANSFER_FUNCTION_GAIN = 4.0f;  // Number of std deviations for full color range
static constexpr bool SCALE_AROUND_MEAN = true;        // Center around mean (true) or zero (false)

// Create a debug bitmap with error/status text (black text on white background)
static HBITMAP CreateDebugBitmap(int width, int height, const std::vector<std::wstring>& lines)
{
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!hBitmap)
    {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return nullptr;
    }

    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    // Fill with white background
    RECT rc = { 0, 0, width, height };
    HBRUSH hBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdcMem, &rc, hBrush);
    DeleteObject(hBrush);

    // Black text
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(0, 0, 0));

    HFONT hFont = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);

    int y = 6;
    int lineHeight = 13;
    for (const auto& line : lines)
    {
        if (line.empty())
            continue;  // Skip blank lines
        if (y + lineHeight > height - 6)
            break;
        RECT textRect = { 6, y, width - 6, y + lineHeight };
        DrawTextW(hdcMem, line.c_str(), -1, &textRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += lineHeight;
    }

    SelectObject(hdcMem, hOldFont);
    DeleteObject(hFont);
    SelectObject(hdcMem, hOldBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    return hBitmap;
}

VdsRenderer::VdsRenderer()
    : m_logFilePath(GetThumbnailLogPath())
    , m_vdsHandle(nullptr)
    , m_layout(nullptr)
    , m_stream(nullptr)
{
}

void VdsRenderer::SetLogFile(const char* logFilePath)
{
    m_logFilePath = logFilePath ? logFilePath : "";
}

std::wstring VdsRenderer::GetVdsName() const
{
    if (!m_layout)
        return L"Unknown";

    // Try to get name from metadata (VolumeDataLayout inherits from MetadataReadAccess)
    // Try common metadata keys for name
    const char* nameKeys[] = { "SurveyName", "Name", "FileName", "DatasetName" };
    for (const char* key : nameKeys)
    {
        if (m_layout->IsMetadataStringAvailable("", key))
        {
            const char* name = m_layout->GetMetadataString("", key);
            if (name && name[0])
            {
                wchar_t wName[256];
                MultiByteToWideChar(CP_UTF8, 0, name, -1, wName, 256);
                return wName;
            }
        }
    }

    return L"VDS";
}

VdsRenderer::~VdsRenderer()
{
    if (m_vdsHandle)
    {
        OpenVDS::Error error;
        OpenVDS::Close(m_vdsHandle, error);
        m_vdsHandle = nullptr;
    }
    m_layout = nullptr;
    if (m_stream)
    {
        m_stream->Release();
        m_stream = nullptr;
    }
}

bool VdsRenderer::Initialize(IStream* pStream)
{
    if (!pStream)
        return false;

    std::vector<std::wstring> dbg;
    wchar_t buf[256];
    dbg.push_back(L"VdsRenderer::Initialize");

    // Clean up any previous state
    if (m_vdsHandle)
    {
        OpenVDS::Error error;
        OpenVDS::Close(m_vdsHandle, error);
        m_vdsHandle = nullptr;
    }
    m_layout = nullptr;
    if (m_stream)
    {
        m_stream->Release();
        m_stream = nullptr;
    }

    // Keep a reference to the stream
    m_stream = pStream;
    m_stream->AddRef();

    // Open VDS using the IStream
    // Note: OpenVDS must be built with OPENVDS_SINGLE_THREADED to work
    // in prevhost.exe (preview handler), otherwise COM marshaling deadlocks
    OpenVDS::IStreamOpenOptions options(pStream);
    OpenVDS::Error error;
    m_vdsHandle = OpenVDS::Open(options, error);

    if (error.code != 0 || !m_vdsHandle)
    {
        swprintf_s(buf, L"OpenVDS::Open failed: %d", error.code);
        dbg.push_back(buf);
        wchar_t wMsg[256];
        MultiByteToWideChar(CP_UTF8, 0, error.string.c_str(), -1, wMsg, 256);
        dbg.push_back(wMsg);
        LogToFile(dbg, m_logFilePath);
        return false;
    }

    m_layout = OpenVDS::GetLayout(m_vdsHandle);
    if (!m_layout)
    {
        dbg.push_back(L"Failed to get layout");
        LogToFile(dbg, m_logFilePath);
        return false;
    }

    // Log success with dimension info
    int dims = m_layout->GetDimensionality();
    swprintf_s(buf, L"Opened %dD VDS successfully", dims);
    dbg.push_back(buf);

    std::wstring dimStr = L"Dims: ";
    for (int i = 0; i < dims; i++)
    {
        if (i > 0) dimStr += L" x ";
        swprintf_s(buf, L"%d", m_layout->GetDimensionNumSamples(i));
        dimStr += buf;
    }
    dbg.push_back(dimStr);
    LogToFile(dbg, m_logFilePath);

    return true;
}

std::vector<std::wstring> VdsRenderer::GetMetadataLines() const
{
    std::vector<std::wstring> lines;

    if (!m_layout)
    {
        lines.push_back(L"Failed to open VDS");
        return lines;
    }

    wchar_t buf[256];

    // Header with VDS name
    std::wstring name = GetVdsName();
    swprintf_s(buf, L"VDS: %s", name.c_str());
    lines.push_back(buf);
    lines.push_back(L"");

    // Dimensions - compact format
    int dims = m_layout->GetDimensionality();
    swprintf_s(buf, L"Dimensions: %dD", dims);
    lines.push_back(buf);

    for (int dim = 0; dim < dims; dim++)
    {
        auto axis = m_layout->GetAxisDescriptor(dim);
        wchar_t axisName[32], unit[16];
        MultiByteToWideChar(CP_UTF8, 0, axis.GetName(), -1, axisName, 32);
        MultiByteToWideChar(CP_UTF8, 0, axis.GetUnit(), -1, unit, 16);

        swprintf_s(buf, L"  %s: %d [%.1f..%.1f] %s",
                   axisName,
                   m_layout->GetDimensionNumSamples(dim),
                   axis.GetCoordinateMin(),
                   axis.GetCoordinateMax(),
                   unit);
        lines.push_back(buf);
    }
    lines.push_back(L"");

    // Channels - compact format
    int chCount = m_layout->GetChannelCount();
    swprintf_s(buf, L"Channels: %d", chCount);
    lines.push_back(buf);

    for (int ch = 0; ch < chCount && ch < 4; ch++)
    {
        auto channel = m_layout->GetChannelDescriptor(ch);
        wchar_t chName[32];
        MultiByteToWideChar(CP_UTF8, 0, channel.GetName(), -1, chName, 32);

        swprintf_s(buf, L"  %s [%.2e..%.2e]",
                   chName,
                   channel.GetValueRangeMin(),
                   channel.GetValueRangeMax());
        lines.push_back(buf);
    }
    if (chCount > 4)
    {
        swprintf_s(buf, L"  ... +%d more", chCount - 4);
        lines.push_back(buf);
    }
    lines.push_back(L"");

    // Compression - single line
    OpenVDS::CompressionMethod compression = OpenVDS::GetCompressionMethod(m_vdsHandle);
    const wchar_t* compName = L"Unknown";
    switch (compression)
    {
    case OpenVDS::CompressionMethod::None: compName = L"None"; break;
    case OpenVDS::CompressionMethod::Wavelet: compName = L"Wavelet"; break;
    case OpenVDS::CompressionMethod::RLE: compName = L"RLE"; break;
    case OpenVDS::CompressionMethod::Zip: compName = L"Zip"; break;
    case OpenVDS::CompressionMethod::WaveletLossless: compName = L"Wavelet (Lossless)"; break;
    }

    if (compression == OpenVDS::CompressionMethod::Wavelet)
    {
        float tolerance = OpenVDS::GetCompressionTolerance(m_vdsHandle);
        swprintf_s(buf, L"Compression: %s (tol: %.2f)", compName, tolerance);
    }
    else
    {
        swprintf_s(buf, L"Compression: %s", compName);
    }
    lines.push_back(buf);

    // Brick size and LOD levels
    auto layoutDescriptor = m_layout->GetLayoutDescriptor();
    int brickSize = 1 << layoutDescriptor.GetBrickSize();
    int lodLevels = static_cast<int>(layoutDescriptor.GetLODLevels());
    swprintf_s(buf, L"Brick Size: %d, LOD Levels: %d", brickSize, lodLevels);
    lines.push_back(buf);
    lines.push_back(L"");

    // Dimension groups availability
    lines.push_back(L"Dimension Groups:");
    OpenVDS::VolumeDataAccessManager accessManager = OpenVDS::GetAccessManager(m_vdsHandle);

    struct DimGroupInfo {
        OpenVDS::DimensionsND group;
        const wchar_t* name;
    };
    DimGroupInfo groups[] = {
        { OpenVDS::Dimensions_01, L"01" },
        { OpenVDS::Dimensions_02, L"02" },
        { OpenVDS::Dimensions_12, L"12" },
        { OpenVDS::Dimensions_012, L"012" },
        { OpenVDS::Dimensions_013, L"013" },
        { OpenVDS::Dimensions_023, L"023" },
        { OpenVDS::Dimensions_123, L"123" },
    };

    std::wstring availGroups;
    for (const auto& dg : groups)
    {
        auto status = accessManager.GetVDSProduceStatus(dg.group, 0, 0);
        if (status == OpenVDS::VDSProduceStatus::Normal)
        {
            if (!availGroups.empty()) availGroups += L", ";
            availGroups += dg.name;
        }
        else if (status == OpenVDS::VDSProduceStatus::Remapped)
        {
            if (!availGroups.empty()) availGroups += L", ";
            availGroups += dg.name;
            availGroups += L"(R)";
        }
    }
    if (availGroups.empty())
        availGroups = L"(none)";
    swprintf_s(buf, L"  Available: %s", availGroups.c_str());
    lines.push_back(buf);

    return lines;
}

int VdsRenderer::GetSliceCount(int dimension) const
{
    if (!m_layout)
        return 0;
    if (dimension < 0)
        return 0;
    // For dimensions beyond the data's dimensionality, return 1
    // This allows 2D data to work with slice dimension 2 (returns 1 slice at index 0)
    if (dimension >= m_layout->GetDimensionality())
        return 1;
    return m_layout->GetDimensionNumSamples(dimension);
}

int VdsRenderer::GetDefaultSliceIndex(int dimension) const
{
    return GetSliceCount(dimension) / 2;            // will work even if that dimension is just 0 - 1
}

int VdsRenderer::GetDimensionality() const
{
    return m_layout ? m_layout->GetDimensionality() : 0;
}

const wchar_t* VdsRenderer::GetDimensionName(int dimension) const
{
    if (!m_layout || dimension < 0 || dimension >= m_layout->GetDimensionality())
        return L"Unknown";

    auto axis = m_layout->GetAxisDescriptor(dimension);
    static wchar_t name[64];
    MultiByteToWideChar(CP_UTF8, 0, axis.GetName(), -1, name, 64);
    return name;
}

void VdsRenderer::NormalizeToGrayscale(const float* source, uint8_t* dest,
                                       int count, float minVal, float maxVal)
{
    float range = maxVal - minVal;
    if (range < 1e-6f)
        range = 1.0f;

    for (int i = 0; i < count; i++)
    {
        float normalized = (source[i] - minVal) / range;
        normalized = std::max(0.0f, std::min(1.0f, normalized));
        dest[i] = static_cast<uint8_t>(normalized * 255.0f);
    }
}

HBITMAP VdsRenderer::CreateColorizedBitmap(const uint8_t* grayscaleData,
                                           int width, int height)
{
    // Get a proper DC (important for surrogate processes like preview handlers)
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    // Create DIB section for 32-bit RGBA bitmap
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS,
                                       &pBits, nullptr, 0);

    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    if (!hBitmap)
        return nullptr;

    // Apply blue-white-red colormap (seismic style)
    uint32_t* pixels = static_cast<uint32_t*>(pBits);
    for (int i = 0; i < width * height; i++)
    {
        uint8_t value = grayscaleData[i];
        uint8_t r, g, b;

        if (value < 128)
        {
            // Blue to white (low values)
            float t = value / 128.0f;
            r = static_cast<uint8_t>(t * 255);
            g = static_cast<uint8_t>(t * 255);
            b = 255;
        }
        else
        {
            // White to red (high values)
            float t = (value - 128) / 127.0f;
            r = 255;
            g = static_cast<uint8_t>((1.0f - t) * 255);
            b = static_cast<uint8_t>((1.0f - t) * 255);
        }

        // BGRA format
        pixels[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
    }

    return hBitmap;
}

// Select optimal LOD for rendering based on slice size and target thumbnail size
// Returns the LOD level to use (0 = full resolution, higher = faster but lower quality)
// Based on benchmark data showing 10-100x speedup with higher LODs for large datasets
static int SelectOptimalLOD(OpenVDS::VolumeDataAccessManager& accessManager,
                            OpenVDS::VolumeDataLayout* layout,
                            OpenVDS::DimensionsND dimGroup,
                            int dim0Size, int dim1Size,
                            int targetSize)
{
    // TEMPORARY: LOD optimization disabled due to data quality issues
    // Higher LODs appear to return incorrect data in some cases
    if (!ENABLE_LOD_OPTIMIZATION)
        return 0;

    // Get available LOD count from layout descriptor
    int lodCount = static_cast<int>(layout->GetLayoutDescriptor().GetLODLevels());
    // Note: lodCount == 0 means this dimension group is unavailable
    // Only proceed if lodCount > 0
    if (lodCount == 0)
        return 0;  // No LODs available for this dimension group

    int64_t sliceVoxels = static_cast<int64_t>(dim0Size) * dim1Size;

    // If slice is small enough, use LOD0 for best quality
    if (sliceVoxels <= VOXEL_THRESHOLD_FOR_LOD)
        return 0;

    // Calculate minimum dimension at LOD0
    int minDim = std::min(dim0Size, dim1Size);

    // Try each LOD starting from highest (fastest) to find one that:
    // 1. Is available (Normal or Remapped status)
    // 2. Still provides sufficient resolution for the target thumbnail
    for (int lod = lodCount - 1; lod >= 0; lod--)
    {
        // Check if this LOD is available
        auto status = accessManager.GetVDSProduceStatus(dimGroup, lod, 0);
        if (status == OpenVDS::VDSProduceStatus::Unavailable)
            continue;

        // Estimate the effective resolution at this LOD
        // Higher LOD = lower resolution, roughly halving each level
        // Note: OpenVDS returns full-res data but decompression is faster at higher LOD
        int effectiveMinDim = minDim >> lod;

        // Ensure we have at least MIN_LOD_DIMENSION pixels and enough for the target
        if (effectiveMinDim >= MIN_LOD_DIMENSION && effectiveMinDim >= targetSize / 2)
        {
            return lod;
        }
    }

    // Fall back to LOD0 if no suitable higher LOD found
    return 0;
}

// Validate that the returned data looks reasonable
// Returns statistics for color mapping
struct DataValidationResult
{
    bool isValid;
    float minVal;
    float maxVal;
    float mean;
    float stdDev;
    int invalidCount;   // NaN + Inf + extreme magnitude count
    int totalCount;
    std::wstring errorMessage;
};

// Helper to check if a value is valid for rendering (finite and reasonable magnitude)
static inline bool IsValidRenderValue(float v)
{
    return std::isfinite(v) && std::fabs(v) <= MAX_VALID_MAGNITUDE;
}

static DataValidationResult ValidateSliceData(const float* data, size_t count)
{
    DataValidationResult result = {};
    result.totalCount = static_cast<int>(count);

    if (!data || count == 0)
    {
        result.isValid = true;  // Don't fail, just note it
        result.mean = 0.0f;
        result.stdDev = 1.0f;
        result.errorMessage = L"Warning: No data returned";
        return result;
    }

    // Use RunningStat for numerically stable mean/variance calculation
    RunningStat stats;
    int invalidCount = 0;
    float minVal = 0.0f;
    float maxVal = 0.0f;
    bool foundValid = false;

    for (size_t i = 0; i < count; i++)
    {
        float v = data[i];
        if (!IsValidRenderValue(v))
        {
            invalidCount++;
        }
        else
        {
            stats.Push(v);
            if (!foundValid)
            {
                minVal = maxVal = v;
                foundValid = true;
            }
            else
            {
                if (v < minVal) minVal = v;
                if (v > maxVal) maxVal = v;
            }
        }
    }

    result.invalidCount = invalidCount;
    result.minVal = minVal;
    result.maxVal = maxVal;

    // Check if too many invalid values (warning only, don't fail)
    float invalidRatio = static_cast<float>(invalidCount) / count;
    if (invalidRatio > MAX_INVALID_RATIO)
    {
        wchar_t buf[128];
        swprintf_s(buf, L"Warning: Many invalid/extreme values: %.1f%%", invalidRatio * 100);
        result.errorMessage = buf;
        // Continue anyway - don't fail
    }

    if (!foundValid || stats.NumDataValues() == 0)
    {
        // No valid values - use defaults
        result.minVal = 0.0f;
        result.maxVal = 1.0f;
        result.mean = 0.0f;
        result.stdDev = 1.0f;
        result.isValid = true;  // Don't fail, let rendering continue with defaults
        if (result.errorMessage.empty())
            result.errorMessage = L"Warning: No valid data values found, using defaults";
        return result;
    }

    // Get statistics from running stats
    result.mean = static_cast<float>(stats.Mean());
    result.stdDev = static_cast<float>(stats.StandardDeviation());

    // Ensure stdDev is not zero (would cause division by zero)
    if (result.stdDev < MIN_VALUE_VARIANCE)
    {
        result.stdDev = 1.0f;
        wchar_t buf[128];
        swprintf_s(buf, L"Warning: Data has low variance (all values ~%.2e)", result.mean);
        if (result.errorMessage.empty())
            result.errorMessage = buf;
    }

    result.isValid = true;
    return result;
}

// Scale a bitmap to fit within maxSize while maintaining aspect ratio
static HBITMAP ScaleBitmap(HBITMAP hSource, int maxSize)
{
    if (!hSource || maxSize <= 0)
        return hSource;

    BITMAP bm;
    GetObject(hSource, sizeof(bm), &bm);

    int srcWidth = bm.bmWidth;
    int srcHeight = bm.bmHeight;

    // If already smaller than maxSize, return as-is
    if (srcWidth <= maxSize && srcHeight <= maxSize)
        return hSource;

    // Calculate scaled dimensions maintaining aspect ratio
    float scale = std::min((float)maxSize / srcWidth, (float)maxSize / srcHeight);
    int dstWidth = std::max(1, (int)(srcWidth * scale));
    int dstHeight = std::max(1, (int)(srcHeight * scale));

    // Create destination bitmap
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcSrc = CreateCompatibleDC(hdcScreen);
    HDC hdcDst = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = dstWidth;
    bmi.bmiHeader.biHeight = -dstHeight;  // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hDest = CreateDIBSection(hdcDst, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);

    if (hDest)
    {
        HBITMAP hOldSrc = (HBITMAP)SelectObject(hdcSrc, hSource);
        HBITMAP hOldDst = (HBITMAP)SelectObject(hdcDst, hDest);

        // Use high-quality scaling
        SetStretchBltMode(hdcDst, HALFTONE);
        SetBrushOrgEx(hdcDst, 0, 0, nullptr);
        StretchBlt(hdcDst, 0, 0, dstWidth, dstHeight,
                   hdcSrc, 0, 0, srcWidth, srcHeight, SRCCOPY);

        SelectObject(hdcSrc, hOldSrc);
        SelectObject(hdcDst, hOldDst);

        // Delete source bitmap and return scaled version
        DeleteObject(hSource);
    }

    DeleteDC(hdcSrc);
    DeleteDC(hdcDst);
    ReleaseDC(nullptr, hdcScreen);

    // Return scaled bitmap, or original if scaling failed
    return hDest ? hDest : hSource;
}

HBITMAP VdsRenderer::RenderSlice(int sliceOnDimension, int sliceIndex, int maxSize)
{
    std::vector<std::wstring> dbg;  // Debug info for error bitmap
    m_lastRenderDebugMessages.clear();

    // Early logging to confirm we entered RenderSlice
    wchar_t entryBuf[128];
    swprintf_s(entryBuf, L"RenderSlice(dim=%d, slice=%d, maxSize=%d)", sliceOnDimension, sliceIndex, maxSize);
    dbg.push_back(entryBuf);
    LogToFile(dbg, m_logFilePath);

    if (!m_vdsHandle || !m_layout)
    {
        dbg.push_back(L"VDS Error");
        dbg.push_back(L"Handle or layout is null");
        m_lastRenderDebugMessages = std::move(dbg);
        return CreateDebugBitmap(256, 256, m_lastRenderDebugMessages);
    }

    int dimensionality = m_layout->GetDimensionality();

    // Validate slice index (GetSliceCount returns 1 for non-existent dimensions,
    // so 2D data with sliceOnDimension=2 and sliceIndex=0 is valid)
    int sliceCount = GetSliceCount(sliceOnDimension);
    if (sliceIndex < 0 || sliceIndex >= sliceCount)
    {
        dbg.push_back(L"VDS Error");
        wchar_t buf[64];
        swprintf_s(buf, L"Invalid slice: %d", sliceIndex);
        dbg.push_back(buf);
        m_lastRenderDebugMessages = std::move(dbg);
        return CreateDebugBitmap(256, 256, m_lastRenderDebugMessages);
    }

    // Build compact debug info
    wchar_t buf[256];
    std::wstring vdsName = GetVdsName();
    swprintf_s(buf, L"%s (%dD)", vdsName.c_str(), dimensionality);
    dbg.push_back(buf);

    try
    {
        // Get access manager
        OpenVDS::VolumeDataAccessManager accessManager = OpenVDS::GetAccessManager(m_vdsHandle);

        // Build compact dimension summary
        std::wstring dimStr = L"Dims: ";
        for (int dim = 0; dim < dimensionality; dim++)
        {
            if (dim > 0) dimStr += L" x ";
            swprintf_s(buf, L"%d", m_layout->GetDimensionNumSamples(dim));
            dimStr += buf;
        }
        dbg.push_back(dimStr);

        // Set up voxel bounds
        // Always display dim0 × dim1, slice on dim2+ (if they exist)
        int voxelMin[OpenVDS::Dimensionality_Max] = { 0, 0, 0, 0, 0, 0 };
        int voxelMax[OpenVDS::Dimensionality_Max] = { 1, 1, 1, 1, 1, 1 };

        // Display dimensions are always dim0 and dim1
        int dim0 = 0;
        int dim1 = 1;
        int dim0Size = m_layout->GetDimensionNumSamples(0);
        int dim1Size = m_layout->GetDimensionNumSamples(1);

        // Full extent for display dimensions
        voxelMin[0] = 0;
        voxelMax[0] = dim0Size;
        voxelMin[1] = 0;
        voxelMax[1] = dim1Size;

        // For dimensions 2 and above, fix at sliceIndex (or midpoint for dim3+)
        // sliceOnDimension parameter is used for dim2, sliceIndex selects which slice
        if (dimensionality >= 3)
        {
            // Dim2 is the primary slice dimension
            voxelMin[2] = sliceIndex;
            voxelMax[2] = sliceIndex + 1;
        }
        if (dimensionality >= 4)
        {
            // Dim3+ fixed at midpoint
            for (int dim = 3; dim < dimensionality; dim++)
            {
                int midpoint = m_layout->GetDimensionNumSamples(dim) / 2;
                voxelMin[dim] = midpoint;
                voxelMax[dim] = midpoint + 1;
            }
        }

        // VDS dimension conventions:
        // 2D poststack: dim0=Sample, dim1=CDP
        // 3D poststack: dim0=Sample, dim1=Crossline, dim2=Inline
        // 4D prestack:  dim0=Sample, dim1=Offset, dim2=Crossline, dim3=Inline
        //
        // Display: dim0 (Sample) is Y-axis (vertical), dim1 is X-axis (horizontal)
        // Slice on dim2 (Inline for 3D)

        // OpenVDS returns data with dim0 as fastest-varying
        // buffer layout: buffer[dim1_idx * dim0Size + dim0_idx]
        //
        // For display: dim0 (Sample) should be vertical (Y), dim1 should be horizontal (X)
        // This requires transposing the buffer when creating the bitmap
        int width = dim1Size;
        int height = dim0Size;

        // Allocate buffer for slice data
        std::vector<float> buffer(dim0Size * dim1Size);

        // Find available dimension groups
        struct DimGroupInfo {
            OpenVDS::DimensionsND group;
            const wchar_t* name;
        };
        DimGroupInfo allGroups[] = {
            { OpenVDS::Dimensions_01, L"01" },
            { OpenVDS::Dimensions_02, L"02" },
            { OpenVDS::Dimensions_12, L"12" },
            { OpenVDS::Dimensions_012, L"012" },
        };
        OpenVDS::DimensionsND dimGroup = OpenVDS::Dimensions_01;

        for (const auto& dg : allGroups)
        {
            auto status = accessManager.GetVDSProduceStatus(dg.group, 0, 0);
            if (status == OpenVDS::VDSProduceStatus::Normal)
            {
                dimGroup = dg.group;
                break;
            }
        }

        // Select LOD (prefer LOD0 for quality)
        int lodCount = static_cast<int>(m_layout->GetLayoutDescriptor().GetLODLevels());
        int selectedLOD = 0;
        for (int lod = 0; lod < lodCount; lod++)
        {
            if (accessManager.GetVDSProduceStatus(dimGroup, lod, 0) == OpenVDS::VDSProduceStatus::Normal)
            {
                selectedLOD = lod;
                break;
            }
        }

        // Log before request to help diagnose hangs
#ifdef OPENVDS_SINGLE_THREADED
        dbg.push_back(L"Mode: SINGLE_THREADED");
#else
        dbg.push_back(L"Mode: MULTI_THREADED (will deadlock in prevhost.exe!)");
#endif
        LogToFile(dbg, m_logFilePath);

        swprintf_s(buf, L"Requesting %dx%d slice...", dim0Size, dim1Size);
        dbg.push_back(buf);
        LogToFile(dbg, m_logFilePath);

        // Request the slice using selected LOD
        auto request = accessManager.RequestVolumeSubset<float>(
            buffer.data(),
            buffer.size() * sizeof(float),
            dimGroup,
            selectedLOD, 0,
            voxelMin, voxelMax
        );

        dbg.push_back(L"Request created, waiting for completion...");
        LogToFile(dbg, m_logFilePath);

        if (!request->WaitForCompletion())
        {
            swprintf_s(buf, L"VDS request failed: %d", request->GetErrorCode());
            dbg.push_back(buf);
            std::string errMsg = request->GetErrorMessage();
            wchar_t wErrMsg[256];
            MultiByteToWideChar(CP_UTF8, 0, errMsg.c_str(), -1, wErrMsg, 256);
            dbg.push_back(wErrMsg);
            LogToFile(dbg, m_logFilePath);
            m_lastRenderDebugMessages = std::move(dbg);
            return CreateDebugBitmap(256, 256, m_lastRenderDebugMessages);
        }

        const float* bufferPtr = buffer.data();

        // Add slice info to debug
        if (dimensionality >= 3)
        {
            const wchar_t* dimName = GetDimensionName(2);
            swprintf_s(buf, L"Slice: %s %d/%d", dimName, sliceIndex + 1, m_layout->GetDimensionNumSamples(2));
            dbg.push_back(buf);
        }
        swprintf_s(buf, L"Output: %d x %d", width, height);
        dbg.push_back(buf);

        // Quick min/max/mean of buffer data
        float rawMin = bufferPtr[0], rawMax = bufferPtr[0];
        double rawSum = 0.0;
        size_t bufferSize = static_cast<size_t>(dim0Size) * dim1Size;
        for (size_t i = 0; i < bufferSize; i++)
        {
            float v = bufferPtr[i];
            if (v < rawMin) rawMin = v;
            if (v > rawMax) rawMax = v;
            rawSum += v;
        }
        float rawMean = static_cast<float>(rawSum / bufferSize);
        swprintf_s(buf, L"Raw: min=%.2e max=%.2e mean=%.2e", rawMin, rawMax, rawMean);
        dbg.push_back(buf);

        // Validate and compute statistics
        DataValidationResult validation = ValidateSliceData(bufferPtr, bufferSize);

        // Log to file
        LogToFile(dbg, m_logFilePath);

        // Use mean/stddev for normalization (produces better contrast for seismic data)
        float histoMean = SCALE_AROUND_MEAN ? validation.mean : 0.0f;
        float invScaling = TRANSFER_FUNCTION_GAIN * validation.stdDev;
        if (invScaling < 1e-6f) invScaling = 1.0f;

        // Create grayscale buffer with proper layout for bitmap
        // Transpose: bitmap(x,y) where x is dim1 (horizontal), y is dim0/Sample (vertical)
        std::vector<uint8_t> grayscale(width * height);

        for (int y = 0; y < height; y++)      // y iterates over dim0 (Sample)
        {
            for (int x = 0; x < width; x++)   // x iterates over dim1
            {
                float v = bufferPtr[x * dim0Size + y];  // buffer[dim1_idx * dim0Size + dim0_idx]
                if (!IsValidRenderValue(v)) v = histoMean;  // Filter NaN/Inf/extreme values
                // Map to 0-255: center at 128, scale by stddev
                float val = 128.0f + (v - histoMean) * 128.0f / invScaling;
                val = std::max(0.0f, std::min(255.0f, val));
                grayscale[y * width + x] = static_cast<uint8_t>(val);
            }
        }

        // Create colorized bitmap and scale to requested size
        HBITMAP hBitmap = CreateColorizedBitmap(grayscale.data(), width, height);

        // Save debug bitmap to temp folder
        if (hBitmap)
        {
            std::string debugBmpPath = GetLogFilePath("openvds-preview-slice.bmp");
            SaveBitmapToFile(hBitmap, debugBmpPath.c_str());
            swprintf_s(buf, L"Saved: %S", debugBmpPath.c_str());
            dbg.push_back(buf);
        }

        // Store debug messages for preview handler to display
        m_lastRenderDebugMessages = std::move(dbg);

        return ScaleBitmap(hBitmap, maxSize);
    }
    catch (const std::exception& e)
    {
        dbg.push_back(L"EXCEPTION");
        wchar_t wMsg[256];
        MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, wMsg, 256);
        dbg.push_back(wMsg);
        LogToFile(dbg, m_logFilePath);
        m_lastRenderDebugMessages = std::move(dbg);
        return CreateDebugBitmap(maxSize > 0 ? maxSize : 256, maxSize > 0 ? maxSize : 256, m_lastRenderDebugMessages);
    }
    catch (...)
    {
        dbg.push_back(L"EXCEPTION: Unknown");
        LogToFile(dbg, m_logFilePath);
        m_lastRenderDebugMessages = std::move(dbg);
        return CreateDebugBitmap(maxSize > 0 ? maxSize : 256, maxSize > 0 ? maxSize : 256, m_lastRenderDebugMessages);
    }
}
