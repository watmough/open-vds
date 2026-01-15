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
#include <cmath>
#include <sstream>

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
    m_stream = nullptr;
}

bool VdsRenderer::Initialize(IStream* pStream)
{
    if (!pStream)
        return false;

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

HBITMAP VdsRenderer::RenderSlice(int dimension, int sliceIndex, int maxSize)
{
    if (!m_vdsHandle || !m_layout)
        return nullptr;

    if (dimension < 0 || dimension >= m_layout->GetDimensionality())
        return nullptr;

    if (sliceIndex < 0 || sliceIndex >= GetSliceCount(dimension))
        return nullptr;

    try
    {
        // Get access manager
        OpenVDS::VolumeDataAccessManager accessManager = OpenVDS::GetAccessManager(m_vdsHandle);

        // Determine slice dimensions
        int voxelMin[OpenVDS::Dimensionality_Max] = { 0, 0, 0, 0, 0, 0 };
        int voxelMax[OpenVDS::Dimensionality_Max] = { 1, 1, 1, 1, 1, 1 };

        int width = 0, height = 0;
        for (int dim = 0; dim < m_layout->GetDimensionality(); dim++)
        {
            if (dim == dimension)
            {
                voxelMin[dim] = sliceIndex;
                voxelMax[dim] = sliceIndex + 1;
            }
            else
            {
                voxelMin[dim] = 0;
                voxelMax[dim] = m_layout->GetDimensionNumSamples(dim);
                if (width == 0)
                    width = voxelMax[dim];
                else if (height == 0)
                    height = voxelMax[dim];
            }
        }

        // For 2D data
        if (m_layout->GetDimensionality() == 2)
        {
            if (dimension == 0)
            {
                width = m_layout->GetDimensionNumSamples(1);
                height = 1;
            }
            else
            {
                width = m_layout->GetDimensionNumSamples(0);
                height = 1;
            }
        }

        // Allocate buffer
        std::vector<float> buffer(width * height);

        // Request the slice
        auto request = accessManager.RequestVolumeSubset<float>(
            buffer.data(),
            buffer.size() * sizeof(float),
            OpenVDS::Dimensions_012,
            0, 0,
            voxelMin, voxelMax
        );

        if (!request->WaitForCompletion())
            return nullptr;

        // Get value range from channel
        float minVal = m_layout->GetChannelValueRangeMin(0);
        float maxVal = m_layout->GetChannelValueRangeMax(0);

        // Normalize to grayscale
        std::vector<uint8_t> grayscale(width * height);
        NormalizeToGrayscale(buffer.data(), grayscale.data(),
                            width * height, minVal, maxVal);

        // Create colorized bitmap
        return CreateColorizedBitmap(grayscale.data(), width, height);
    }
    catch (...)
    {
        return nullptr;
    }
}
