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
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <sstream>
#include <string>

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
static constexpr float MAX_INVALID_RATIO = 0.5f;    // Max 50% NaN/Inf values
static constexpr float MIN_VALUE_VARIANCE = 1e-10f; // Minimum variance to be considered valid data

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
    : m_vdsHandle(nullptr)
    , m_layout(nullptr)
    , m_stream(nullptr)
{
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

    m_stream = pStream;
    m_stream->AddRef();

    // Open VDS using IStreamOpenOptions
    OpenVDS::IStreamOpenOptions options(pStream);
    OpenVDS::Error error;
    m_vdsHandle = OpenVDS::Open(options, error);

    if (error.code != 0 || !m_vdsHandle)
        return false;

    m_layout = OpenVDS::GetLayout(m_vdsHandle);
    return (m_layout != nullptr);
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

    // Header
    lines.push_back(L"OpenVDS File Information");
    lines.push_back(L"========================");
    lines.push_back(L"");

    // Dimensionality
    swprintf_s(buf, L"Dimensionality: %d", m_layout->GetDimensionality());
    lines.push_back(buf);
    lines.push_back(L"");

    // Dimensions
    lines.push_back(L"Dimensions:");
    for (int dim = 0; dim < m_layout->GetDimensionality(); dim++)
    {
        auto axis = m_layout->GetAxisDescriptor(dim);

        // Convert name and unit from char* to wstring
        wchar_t name[64], unit[64];
        MultiByteToWideChar(CP_UTF8, 0, axis.GetName(), -1, name, 64);
        MultiByteToWideChar(CP_UTF8, 0, axis.GetUnit(), -1, unit, 64);

        swprintf_s(buf, L"  [%d] %s:", dim, name);
        lines.push_back(buf);

        swprintf_s(buf, L"      Samples: %d", m_layout->GetDimensionNumSamples(dim));
        lines.push_back(buf);

        swprintf_s(buf, L"      Range: [%.2f, %.2f] %s",
                   axis.GetCoordinateMin(), axis.GetCoordinateMax(), unit);
        lines.push_back(buf);
    }
    lines.push_back(L"");

    // Channels
    swprintf_s(buf, L"Channels: %d", m_layout->GetChannelCount());
    lines.push_back(buf);
    for (int ch = 0; ch < m_layout->GetChannelCount(); ch++)
    {
        auto channel = m_layout->GetChannelDescriptor(ch);

        wchar_t channelName[64];
        MultiByteToWideChar(CP_UTF8, 0, channel.GetName(), -1, channelName, 64);

        swprintf_s(buf, L"  [%d] %s", ch, channelName);
        lines.push_back(buf);

        swprintf_s(buf, L"      Value range: [%.2e, %.2e]",
                   channel.GetValueRangeMin(), channel.GetValueRangeMax());
        lines.push_back(buf);
    }
    lines.push_back(L"");

    // Compression
    OpenVDS::CompressionMethod compression = OpenVDS::GetCompressionMethod(m_vdsHandle);
    const wchar_t* compressionName = L"Unknown";
    switch (compression)
    {
    case OpenVDS::CompressionMethod::None: compressionName = L"None"; break;
    case OpenVDS::CompressionMethod::Wavelet: compressionName = L"Wavelet"; break;
    case OpenVDS::CompressionMethod::RLE: compressionName = L"RLE"; break;
    case OpenVDS::CompressionMethod::Zip: compressionName = L"Zip"; break;
    case OpenVDS::CompressionMethod::WaveletLossless: compressionName = L"Wavelet Lossless"; break;
    }

    swprintf_s(buf, L"Compression: %s", compressionName);
    lines.push_back(buf);

    if (compression == OpenVDS::CompressionMethod::Wavelet)
    {
        float tolerance = OpenVDS::GetCompressionTolerance(m_vdsHandle);
        swprintf_s(buf, L"Tolerance: %.2f", tolerance);
        lines.push_back(buf);
    }
    lines.push_back(L"");

    // Brick size
    auto layoutDescriptor = m_layout->GetLayoutDescriptor();
    int brickSize = 1 << layoutDescriptor.GetBrickSize();
    swprintf_s(buf, L"Brick size: %d", brickSize);
    lines.push_back(buf);
    lines.push_back(L"");

    // Instructions
    lines.push_back(L"Keyboard Shortcuts:");
    lines.push_back(L"  V or Tab    - Toggle view mode");
    lines.push_back(L"  ↑/↓         - Navigate slices or scroll");
    lines.push_back(L"  PgUp/PgDn   - Fast navigation");
    lines.push_back(L"  Mouse Wheel - Navigate or scroll");
    lines.push_back(L"");
    lines.push_back(L"Click in the preview to enable keyboard navigation");

    return lines;
}

int VdsRenderer::GetSliceCount(int dimension) const
{
    if (!m_layout || dimension < 0 || dimension >= m_layout->GetDimensionality())
        return 0;
    return m_layout->GetDimensionNumSamples(dimension);
}

int VdsRenderer::GetDefaultSliceIndex(int dimension) const
{
    return GetSliceCount(dimension) / 2;
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
    // Create DIB section for 32-bit RGBA bitmap
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS,
                                       &pBits, nullptr, 0);
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
    if (lodCount == 0) lodCount = 1;  // At least LOD0

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
// Returns true if data appears valid, false if it seems corrupted/empty
struct DataValidationResult
{
    bool isValid;
    float minVal;
    float maxVal;
    int invalidCount;   // NaN + Inf count
    int totalCount;
    std::wstring errorMessage;
};

static DataValidationResult ValidateSliceData(const float* data, size_t count)
{
    DataValidationResult result = {};
    result.totalCount = static_cast<int>(count);

    if (!data || count == 0)
    {
        result.isValid = false;
        result.errorMessage = L"No data returned";
        return result;
    }

    // First pass: count invalid values and find initial valid value
    int invalidCount = 0;
    float firstValidValue = 0.0f;
    bool foundValid = false;

    for (size_t i = 0; i < count; i++)
    {
        float v = data[i];
        if (std::isnan(v) || std::isinf(v))
        {
            invalidCount++;
        }
        else if (!foundValid)
        {
            firstValidValue = v;
            foundValid = true;
        }
    }

    result.invalidCount = invalidCount;

    // Check if too many invalid values
    float invalidRatio = static_cast<float>(invalidCount) / count;
    if (invalidRatio > MAX_INVALID_RATIO)
    {
        result.isValid = false;
        wchar_t buf[128];
        swprintf_s(buf, L"Too many invalid values: %.1f%%", invalidRatio * 100);
        result.errorMessage = buf;
        return result;
    }

    if (!foundValid)
    {
        result.isValid = false;
        result.errorMessage = L"No valid data values found";
        return result;
    }

    // Second pass: compute min/max
    result.minVal = firstValidValue;
    result.maxVal = firstValidValue;

    for (size_t i = 0; i < count; i++)
    {
        float v = data[i];
        if (std::isfinite(v))
        {
            if (v < result.minVal) result.minVal = v;
            if (v > result.maxVal) result.maxVal = v;
        }
    }

    // Check if data has any variance
    float range = result.maxVal - result.minVal;
    if (range < MIN_VALUE_VARIANCE)
    {
        result.isValid = false;
        wchar_t buf[128];
        swprintf_s(buf, L"Data has no variance (all values ~%.2e)", result.minVal);
        result.errorMessage = buf;
        return result;
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

HBITMAP VdsRenderer::RenderSlice(int dimension, int sliceIndex, int maxSize)
{
    std::vector<std::wstring> dbg;  // Debug info for error bitmap

    if (!m_vdsHandle || !m_layout)
    {
        dbg.push_back(L"VDS Error");
        dbg.push_back(L"Handle or layout is null");
        return CreateDebugBitmap(256, 256, dbg);
    }

    int dimensionality = m_layout->GetDimensionality();

    // For 2D data, render the entire dataset (ignore dimension/sliceIndex)
    if (dimensionality == 2)
    {
        return Render2D(maxSize);
    }

    // For 3D/4D data, validate dimension and slice index
    if (dimension < 0 || dimension >= dimensionality)
    {
        dbg.push_back(L"VDS Error");
        wchar_t buf[64];
        swprintf_s(buf, L"Invalid dimension: %d", dimension);
        dbg.push_back(buf);
        return CreateDebugBitmap(256, 256, dbg);
    }

    if (sliceIndex < 0 || sliceIndex >= GetSliceCount(dimension))
    {
        dbg.push_back(L"VDS Error");
        wchar_t buf[64];
        swprintf_s(buf, L"Invalid slice: %d", sliceIndex);
        dbg.push_back(buf);
        return CreateDebugBitmap(256, 256, dbg);
    }

    // Add detailed debug info
    wchar_t buf[128];
    dbg.push_back(L"=== VDS Information ===");
    swprintf_s(buf, L"Dimensionality: %d", dimensionality);
    dbg.push_back(buf);
    dbg.push_back(L"");
    dbg.push_back(L"Dimensions:");
    for (int d = 0; d < dimensionality; d++)
    {
        auto axis = m_layout->GetAxisDescriptor(d);
        wchar_t name[64], unit[32];
        MultiByteToWideChar(CP_UTF8, 0, axis.GetName(), -1, name, 64);
        MultiByteToWideChar(CP_UTF8, 0, axis.GetUnit(), -1, unit, 32);
        swprintf_s(buf, L"[%d] %s: %d [%.1f-%.1f] %s",
                   d, name, m_layout->GetDimensionNumSamples(d),
                   axis.GetCoordinateMin(), axis.GetCoordinateMax(), unit);
        dbg.push_back(buf);
    }
    dbg.push_back(L"");
    swprintf_s(buf, L"Channels: %d", m_layout->GetChannelCount());
    dbg.push_back(buf);
    for (int ch = 0; ch < m_layout->GetChannelCount(); ch++)
    {
        auto channel = m_layout->GetChannelDescriptor(ch);
        wchar_t chName[64];
        MultiByteToWideChar(CP_UTF8, 0, channel.GetName(), -1, chName, 64);
        swprintf_s(buf, L"[%d] %s: [%.2e, %.2e]",
                   ch, chName, channel.GetValueRangeMin(), channel.GetValueRangeMax());
        dbg.push_back(buf);
    }
    dbg.push_back(L"");

    try
    {
        // Get access manager
        OpenVDS::VolumeDataAccessManager accessManager = OpenVDS::GetAccessManager(m_vdsHandle);

        // Set up voxel bounds
        int voxelMin[OpenVDS::Dimensionality_Max] = { 0, 0, 0, 0, 0, 0 };
        int voxelMax[OpenVDS::Dimensionality_Max] = { 1, 1, 1, 1, 1, 1 };

        // Collect the dimensions that are NOT the slice dimension
        int displayDims[OpenVDS::Dimensionality_Max - 1];
        int displayDimCount = 0;

        for (int dim = 0; dim < dimensionality; dim++)
        {
            if (dim == dimension)
            {
                // This is the slice dimension - fix at sliceIndex
                voxelMin[dim] = sliceIndex;
                voxelMax[dim] = sliceIndex + 1;
            }
            else
            {
                voxelMin[dim] = 0;
                voxelMax[dim] = m_layout->GetDimensionNumSamples(dim);
                displayDims[displayDimCount++] = dim;
            }
        }

        // For 4D data, fix the third display dimension at midpoint
        // We only display 2 dimensions at a time
        if (displayDimCount > 2)
        {
            int dim = displayDims[2];
            int midpoint = m_layout->GetDimensionNumSamples(dim) / 2;
            voxelMin[dim] = midpoint;
            voxelMax[dim] = midpoint + 1;
        }

        // VDS dimension conventions:
        // 3D poststack: dim0=Sample, dim1=Crossline, dim2=Inline
        // 4D prestack:  dim0=Sample, dim1=Offset, dim2=Crossline, dim3=Inline
        //
        // For display:
        // - If Sample (dim0) is one of the display dimensions, it should be Y (vertical)
        // - The other dimension becomes X (horizontal)
        // - If Sample is sliced (time/depth slice), lower dim = X, higher dim = Y

        int dim0 = displayDims[0];  // First display dimension (lower index)
        int dim1 = displayDims[1];  // Second display dimension (higher index)

        int dim0Size = m_layout->GetDimensionNumSamples(dim0);
        int dim1Size = m_layout->GetDimensionNumSamples(dim1);

        // OpenVDS returns data with dim0 as fastest-varying
        // buffer layout: buffer[dim1_idx * dim0Size + dim0_idx]

        // Determine if we need to transpose for display
        // If dim0 == 0 (Sample dimension), Sample should be Y-axis (vertical)
        // This requires transposing: X = dim1, Y = dim0
        bool needsTranspose = (dim0 == 0);

        int width, height;
        if (needsTranspose)
        {
            // Sample (dim0) is vertical (Y), other dim (dim1) is horizontal (X)
            width = dim1Size;
            height = dim0Size;
        }
        else
        {
            // Time/depth slice - dim0 is X, dim1 is Y
            width = dim0Size;
            height = dim1Size;
        }

        // Allocate buffer for VDS data
        std::vector<float> buffer(dim0Size * dim1Size);

        // Find an available dimension group
        OpenVDS::DimensionsND dimGroup = OpenVDS::Dimensions_012;
        const wchar_t* dimGroupName = L"Dimensions_012";
        OpenVDS::DimensionsND candidates[] = {
            OpenVDS::Dimensions_01,
            OpenVDS::Dimensions_012,
            OpenVDS::Dimensions_02
        };
        const wchar_t* candidateNames[] = {
            L"Dimensions_01",
            L"Dimensions_012",
            L"Dimensions_02"
        };

        for (int i = 0; i < 3; i++)
        {
            auto status = accessManager.GetVDSProduceStatus(candidates[i], 0, 0);
            if (status != OpenVDS::VDSProduceStatus::Unavailable)
            {
                dimGroup = candidates[i];
                dimGroupName = candidateNames[i];
                break;
            }
        }

        // Select optimal LOD based on slice size and target thumbnail size
        // Higher LOD = faster render (10-100x speedup for large datasets)
        int selectedLOD = SelectOptimalLOD(accessManager, m_layout, dimGroup,
                                           dim0Size, dim1Size, maxSize);

        swprintf_s(buf, L"DimGroup: %s, LOD: %d", dimGroupName, selectedLOD);
        dbg.push_back(buf);
        swprintf_s(buf, L"voxelMin: [%d,%d,%d,%d]",
                   voxelMin[0], voxelMin[1], voxelMin[2], voxelMin[3]);
        dbg.push_back(buf);
        swprintf_s(buf, L"voxelMax: [%d,%d,%d,%d]",
                   voxelMax[0], voxelMax[1], voxelMax[2], voxelMax[3]);
        dbg.push_back(buf);
        dbg.push_back(L"");

        // Request the slice using selected LOD
        auto request = accessManager.RequestVolumeSubset<float>(
            buffer.data(),
            buffer.size() * sizeof(float),
            dimGroup,
            selectedLOD, 0,
            voxelMin, voxelMax
        );

        if (!request->WaitForCompletion())
        {
            dbg.push_back(L"=== ERROR ===");
            swprintf_s(buf, L"Error code: %d", request->GetErrorCode());
            dbg.push_back(buf);

            std::string errMsg = request->GetErrorMessage();
            wchar_t wErrMsg[256];
            MultiByteToWideChar(CP_UTF8, 0, errMsg.c_str(), -1, wErrMsg, 256);
            dbg.push_back(wErrMsg);

            return CreateDebugBitmap(256, 256, dbg);
        }

        // Validate the returned data
        DataValidationResult validation = ValidateSliceData(buffer.data(), buffer.size());

        swprintf_s(buf, L"Data validation: %s", validation.isValid ? L"PASSED" : L"FAILED");
        dbg.push_back(buf);

        if (!validation.isValid)
        {
            dbg.push_back(L"");
            dbg.push_back(L"=== DATA VALIDATION FAILED ===");
            dbg.push_back(validation.errorMessage.c_str());
            swprintf_s(buf, L"Invalid values: %d/%d (%.1f%%)",
                       validation.invalidCount, validation.totalCount,
                       100.0f * validation.invalidCount / validation.totalCount);
            dbg.push_back(buf);

            // Output debug info for diagnostic purposes
            OutputDebugStringW(L"VdsRenderer: Data validation failed for slice\n");
            OutputDebugStringW(validation.errorMessage.c_str());
            OutputDebugStringW(L"\n");

            return CreateDebugBitmap(256, 256, dbg);
        }

        swprintf_s(buf, L"Data range: [%.3e, %.3e]", validation.minVal, validation.maxVal);
        dbg.push_back(buf);
        if (validation.invalidCount > 0)
        {
            swprintf_s(buf, L"Invalid values: %d (%.1f%%)",
                       validation.invalidCount,
                       100.0f * validation.invalidCount / validation.totalCount);
            dbg.push_back(buf);
        }

        // Use validated min/max for normalization
        float minVal = validation.minVal;
        float maxVal = validation.maxVal;

        // Fallback to channel metadata if data range is still degenerate
        if (maxVal - minVal < 1e-6f)
        {
            minVal = m_layout->GetChannelValueRangeMin(0);
            maxVal = m_layout->GetChannelValueRangeMax(0);
        }

        // Create grayscale buffer with proper layout for bitmap
        std::vector<uint8_t> grayscale(width * height);
        float range = maxVal - minVal;
        if (range < 1e-6f) range = 1.0f;

        if (needsTranspose)
        {
            // Transpose: bitmap(x,y) comes from buffer(y, x) where x is dim1, y is dim0
            for (int y = 0; y < height; y++)      // y iterates over dim0 (Sample)
            {
                for (int x = 0; x < width; x++)   // x iterates over dim1
                {
                    float v = buffer[x * dim0Size + y];  // buffer[dim1_idx * dim0Size + dim0_idx]
                    if (!std::isfinite(v)) v = minVal;
                    float normalized = (v - minVal) / range;
                    normalized = std::max(0.0f, std::min(1.0f, normalized));
                    grayscale[y * width + x] = static_cast<uint8_t>(normalized * 255.0f);
                }
            }
        }
        else
        {
            // No transpose needed: bitmap(x,y) comes from buffer(y, x) directly
            for (int y = 0; y < height; y++)      // y iterates over dim1
            {
                for (int x = 0; x < width; x++)   // x iterates over dim0
                {
                    float v = buffer[y * dim0Size + x];  // buffer[dim1_idx * dim0Size + dim0_idx]
                    if (!std::isfinite(v)) v = minVal;
                    float normalized = (v - minVal) / range;
                    normalized = std::max(0.0f, std::min(1.0f, normalized));
                    grayscale[y * width + x] = static_cast<uint8_t>(normalized * 255.0f);
                }
            }
        }

        // Create colorized bitmap and scale to requested size
        HBITMAP hBitmap = CreateColorizedBitmap(grayscale.data(), width, height);
        return ScaleBitmap(hBitmap, maxSize);
    }
    catch (const std::exception& e)
    {
        dbg.push_back(L"=== EXCEPTION ===");
        wchar_t wMsg[256];
        MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, wMsg, 256);
        dbg.push_back(wMsg);
        return CreateDebugBitmap(maxSize > 0 ? maxSize : 256, maxSize > 0 ? maxSize : 256, dbg);
    }
    catch (...)
    {
        dbg.push_back(L"=== EXCEPTION ===");
        dbg.push_back(L"Unknown exception");
        return CreateDebugBitmap(maxSize > 0 ? maxSize : 256, maxSize > 0 ? maxSize : 256, dbg);
    }
}

HBITMAP VdsRenderer::Render2D(int maxSize)
{
    std::vector<std::wstring> dbg;  // Debug info for error bitmap

    if (!m_vdsHandle || !m_layout)
    {
        dbg.push_back(L"VDS Error");
        dbg.push_back(L"Handle or layout is null");
        return CreateDebugBitmap(256, 256, dbg);
    }

    // Add detailed debug info
    wchar_t buf[128];
    swprintf_s(buf, L"Dimensionality: %d", m_layout->GetDimensionality());
    dbg.push_back(buf);

    for (int d = 0; d < m_layout->GetDimensionality(); d++)
    {
        auto axis = m_layout->GetAxisDescriptor(d);
        wchar_t name[64];
        MultiByteToWideChar(CP_UTF8, 0, axis.GetName(), -1, name, 64);
        swprintf_s(buf, L"  [%d] %s: %d samples", d, name, m_layout->GetDimensionNumSamples(d));
        dbg.push_back(buf);
    }

    try
    {
        OpenVDS::VolumeDataAccessManager accessManager = OpenVDS::GetAccessManager(m_vdsHandle);

        int voxelMin[OpenVDS::Dimensionality_Max] = { 0, 0, 0, 0, 0, 0 };
        int voxelMax[OpenVDS::Dimensionality_Max] = { 1, 1, 1, 1, 1, 1 };

        // For 2D data, use all of dimensions 0 and 1
        voxelMin[0] = 0;
        voxelMax[0] = m_layout->GetDimensionNumSamples(0);
        voxelMin[1] = 0;
        voxelMax[1] = m_layout->GetDimensionNumSamples(1);

        int dim0Size = m_layout->GetDimensionNumSamples(0);
        int dim1Size = m_layout->GetDimensionNumSamples(1);

        swprintf_s(buf, L"Display: dim0 x dim1 (%dx%d)", dim0Size, dim1Size);
        dbg.push_back(buf);

        // Check if this looks like seismic data with Sample as dim0
        bool needsTranspose = false;
        auto axis0 = m_layout->GetAxisDescriptor(0);
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

        int width, height;
        if (needsTranspose)
        {
            width = dim1Size;
            height = dim0Size;
            swprintf_s(buf, L"Transpose: YES (%dx%d)", width, height);
        }
        else
        {
            width = dim0Size;
            height = dim1Size;
            swprintf_s(buf, L"Transpose: NO (%dx%d)", width, height);
        }
        dbg.push_back(buf);

        std::vector<float> buffer(dim0Size * dim1Size);

        // Find an available dimension group
        OpenVDS::DimensionsND dimGroup = OpenVDS::Dimensions_012;
        const wchar_t* dimGroupName = L"Dimensions_012";

        OpenVDS::DimensionsND candidates[] = {
            OpenVDS::Dimensions_01,
            OpenVDS::Dimensions_012,
            OpenVDS::Dimensions_02
        };
        const wchar_t* candidateNames[] = {
            L"Dimensions_01",
            L"Dimensions_012",
            L"Dimensions_02"
        };

        for (int i = 0; i < 3; i++)
        {
            auto status = accessManager.GetVDSProduceStatus(candidates[i], 0, 0);
            if (status != OpenVDS::VDSProduceStatus::Unavailable)
            {
                dimGroup = candidates[i];
                dimGroupName = candidateNames[i];
                break;
            }
        }

        // Select optimal LOD based on slice size and target thumbnail size
        // Higher LOD = faster render (10-100x speedup for large 2D datasets)
        int selectedLOD = SelectOptimalLOD(accessManager, m_layout, dimGroup,
                                           dim0Size, dim1Size, maxSize);

        swprintf_s(buf, L"DimGroup: %s, LOD: %d", dimGroupName, selectedLOD);
        dbg.push_back(buf);

        auto request = accessManager.RequestVolumeSubset<float>(
            buffer.data(),
            buffer.size() * sizeof(float),
            dimGroup,
            selectedLOD, 0,  // Selected LOD, channel 0
            voxelMin, voxelMax
        );

        if (!request->WaitForCompletion())
        {
            dbg.push_back(L"");
            dbg.push_back(L"ERROR: Request failed!");
            swprintf_s(buf, L"Code: %d", request->GetErrorCode());
            dbg.push_back(buf);

            std::string errMsg = request->GetErrorMessage();
            wchar_t wErrMsg[256];
            MultiByteToWideChar(CP_UTF8, 0, errMsg.c_str(), -1, wErrMsg, 256);
            // Split long error messages
            if (errMsg.length() > 40)
            {
                swprintf_s(buf, L"Msg: %.40s", wErrMsg);
                dbg.push_back(buf);
            }
            else
            {
                swprintf_s(buf, L"Msg: %s", wErrMsg);
                dbg.push_back(buf);
            }

            return CreateDebugBitmap(256, 256, dbg);
        }
        dbg.push_back(L"Request: OK");

        // Validate the returned data
        DataValidationResult validation = ValidateSliceData(buffer.data(), buffer.size());

        swprintf_s(buf, L"Data validation: %s", validation.isValid ? L"PASSED" : L"FAILED");
        dbg.push_back(buf);

        if (!validation.isValid)
        {
            dbg.push_back(L"");
            dbg.push_back(L"=== DATA VALIDATION FAILED ===");
            dbg.push_back(validation.errorMessage.c_str());
            swprintf_s(buf, L"Invalid values: %d/%d (%.1f%%)",
                       validation.invalidCount, validation.totalCount,
                       100.0f * validation.invalidCount / validation.totalCount);
            dbg.push_back(buf);

            // Output debug info for diagnostic purposes
            OutputDebugStringW(L"VdsRenderer: Data validation failed for 2D render\n");
            OutputDebugStringW(validation.errorMessage.c_str());
            OutputDebugStringW(L"\n");

            return CreateDebugBitmap(256, 256, dbg);
        }

        swprintf_s(buf, L"Data range: [%.3e, %.3e]", validation.minVal, validation.maxVal);
        dbg.push_back(buf);
        if (validation.invalidCount > 0)
        {
            swprintf_s(buf, L"Invalid values: %d (%.1f%%)",
                       validation.invalidCount,
                       100.0f * validation.invalidCount / validation.totalCount);
            dbg.push_back(buf);
        }

        // Use validated min/max for normalization
        float minVal = validation.minVal;
        float maxVal = validation.maxVal;

        // Fallback to channel metadata if data range is still degenerate
        if (maxVal - minVal < 1e-6f)
        {
            minVal = m_layout->GetChannelValueRangeMin(0);
            maxVal = m_layout->GetChannelValueRangeMax(0);
        }

        // Create grayscale buffer with proper layout
        std::vector<uint8_t> grayscale(width * height);
        float range = maxVal - minVal;
        if (range < 1e-6f) range = 1.0f;

        if (needsTranspose)
        {
            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    float v = buffer[x * dim0Size + y];
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
                    float v = buffer[y * dim0Size + x];
                    if (!std::isfinite(v)) v = minVal;
                    float normalized = (v - minVal) / range;
                    normalized = std::max(0.0f, std::min(1.0f, normalized));
                    grayscale[y * width + x] = static_cast<uint8_t>(normalized * 255.0f);
                }
            }
        }

        HBITMAP hBitmap = CreateColorizedBitmap(grayscale.data(), width, height);
        return ScaleBitmap(hBitmap, maxSize);
    }
    catch (const std::exception& e)
    {
        dbg.push_back(L"");
        dbg.push_back(L"EXCEPTION:");
        wchar_t wMsg[256];
        MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, wMsg, 256);
        dbg.push_back(wMsg);
        return CreateDebugBitmap(256, 256, dbg);
    }
    catch (...)
    {
        dbg.push_back(L"");
        dbg.push_back(L"UNKNOWN EXCEPTION");
        return CreateDebugBitmap(256, 256, dbg);
    }
}
