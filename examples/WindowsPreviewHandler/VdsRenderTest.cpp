/****************************************************************************
** VDS Render Test Harness
**
** Simple command-line tool to test VDS rendering logic independently
** of the Windows Shell extension.
**
** Usage: VdsRenderTest.exe <input.vds> [output.bmp] [dimension] [sliceIndex]
****************************************************************************/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <shlwapi.h>

#include <OpenVDS/OpenVDS.h>
#include <OpenVDS/VolumeDataLayout.h>
#include <OpenVDS/VolumeDataAccess.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

// Save HBITMAP to BMP file
bool SaveBitmapToFile(HBITMAP hBitmap, const wchar_t* filename, bool flipY = false)
{
    if (!hBitmap)
        return false;

    BITMAP bm;
    GetObject(hBitmap, sizeof(bm), &bm);

    BITMAPFILEHEADER bmfh = {};
    BITMAPINFOHEADER bmih = {};

    bmih.biSize = sizeof(BITMAPINFOHEADER);
    bmih.biWidth = bm.bmWidth;
    bmih.biHeight = bm.bmHeight;  // Positive = bottom-up (standard BMP)
    bmih.biPlanes = 1;
    bmih.biBitCount = 32;
    bmih.biCompression = BI_RGB;
    bmih.biSizeImage = bm.bmWidth * bm.bmHeight * 4;

    bmfh.bfType = 0x4D42;  // 'BM'
    bmfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bmfh.bfSize = bmfh.bfOffBits + bmih.biSizeImage;

    // Get bitmap bits
    std::vector<uint8_t> pixels(bmih.biSizeImage);

    HDC hdc = GetDC(nullptr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader = bmih;

    if (!GetDIBits(hdc, hBitmap, 0, bm.bmHeight, pixels.data(), &bmi, DIB_RGB_COLORS))
    {
        ReleaseDC(nullptr, hdc);
        return false;
    }
    ReleaseDC(nullptr, hdc);

    // Flip Y-axis if requested (usually not needed - GetDIBits handles conversion)
    if (flipY)
    {
        int rowSize = bm.bmWidth * 4;
        std::vector<uint8_t> row(rowSize);
        for (int y = 0; y < bm.bmHeight / 2; y++)
        {
            int topRow = y * rowSize;
            int bottomRow = (bm.bmHeight - 1 - y) * rowSize;
            memcpy(row.data(), &pixels[topRow], rowSize);
            memcpy(&pixels[topRow], &pixels[bottomRow], rowSize);
            memcpy(&pixels[bottomRow], row.data(), rowSize);
        }
    }

    // Write to file
    FILE* f = nullptr;
    if (_wfopen_s(&f, filename, L"wb") != 0 || !f)
        return false;

    fwrite(&bmfh, sizeof(bmfh), 1, f);
    fwrite(&bmih, sizeof(bmih), 1, f);
    fwrite(pixels.data(), 1, pixels.size(), f);
    fclose(f);

    return true;
}

// Create a debug image with text information (black text on white background)
HBITMAP CreateDebugBitmap(int width, int height, const std::vector<std::string>& lines)
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

        wchar_t wline[256];
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wline, 256);

        RECT textRect = { 6, y, width - 6, y + lineHeight };
        DrawTextW(hdcMem, wline, -1, &textRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += lineHeight;
    }

    SelectObject(hdcMem, hOldFont);
    DeleteObject(hFont);
    SelectObject(hdcMem, hOldBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    return hBitmap;
}

// Create colorized bitmap from grayscale (blue-white-red colormap)
HBITMAP CreateColorizedBitmap(const uint8_t* grayscaleData, int width, int height)
{
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!hBitmap)
        return nullptr;

    uint32_t* pixels = static_cast<uint32_t*>(pBits);
    for (int i = 0; i < width * height; i++)
    {
        uint8_t value = grayscaleData[i];
        uint8_t r, g, b;

        if (value < 128)
        {
            float t = value / 128.0f;
            r = static_cast<uint8_t>(t * 255);
            g = static_cast<uint8_t>(t * 255);
            b = 255;
        }
        else
        {
            float t = (value - 128) / 127.0f;
            r = 255;
            g = static_cast<uint8_t>((1.0f - t) * 255);
            b = static_cast<uint8_t>((1.0f - t) * 255);
        }

        pixels[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
    }

    return hBitmap;
}

void PrintVdsInfo(OpenVDS::VolumeDataLayout* layout)
{
    printf("\n=== VDS Information ===\n");
    printf("Dimensionality: %d\n", layout->GetDimensionality());

    printf("\nDimensions:\n");
    for (int dim = 0; dim < layout->GetDimensionality(); dim++)
    {
        auto axis = layout->GetAxisDescriptor(dim);
        printf("  [%d] %s: %d samples, range [%.2f, %.2f] %s\n",
               dim,
               axis.GetName(),
               layout->GetDimensionNumSamples(dim),
               axis.GetCoordinateMin(),
               axis.GetCoordinateMax(),
               axis.GetUnit());
    }

    printf("\nChannels: %d\n", layout->GetChannelCount());
    for (int ch = 0; ch < layout->GetChannelCount(); ch++)
    {
        auto channel = layout->GetChannelDescriptor(ch);
        printf("  [%d] %s: value range [%.2e, %.2e]\n",
               ch,
               channel.GetName(),
               channel.GetValueRangeMin(),
               channel.GetValueRangeMax());
    }
    printf("\n");
}

// Structure to collect debug info for error bitmap
struct DebugInfo {
    std::vector<std::string> lines;
    void add(const char* fmt, ...) {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        lines.push_back(buf);
        printf("%s\n", buf);
    }
};

HBITMAP RenderSlice(OpenVDS::VDSHandle vdsHandle, OpenVDS::VolumeDataLayout* layout,
                    int dimension, int sliceIndex, bool generateDebugOnError = true)
{
    DebugInfo dbg;
    int dimensionality = layout->GetDimensionality();

    dbg.add("Dimensionality: %d", dimensionality);

    // Add dimension info
    for (int dim = 0; dim < dimensionality; dim++)
    {
        auto axis = layout->GetAxisDescriptor(dim);
        dbg.add("  [%d] %s: %d samples", dim, axis.GetName(), layout->GetDimensionNumSamples(dim));
    }

    try
    {
        OpenVDS::VolumeDataAccessManager accessManager = OpenVDS::GetAccessManager(vdsHandle);

        int voxelMin[OpenVDS::Dimensionality_Max] = { 0, 0, 0, 0, 0, 0 };
        int voxelMax[OpenVDS::Dimensionality_Max] = { 1, 1, 1, 1, 1, 1 };

        int displayDims[OpenVDS::Dimensionality_Max - 1];
        int displayDimCount = 0;

        // For 2D data, use all dimensions
        if (dimensionality == 2)
        {
            displayDims[0] = 0;
            displayDims[1] = 1;
            displayDimCount = 2;
            voxelMin[0] = 0;
            voxelMax[0] = layout->GetDimensionNumSamples(0);
            voxelMin[1] = 0;
            voxelMax[1] = layout->GetDimensionNumSamples(1);
        }
        else
        {
            // For 3D+ data, slice one dimension
            for (int dim = 0; dim < dimensionality; dim++)
            {
                if (dim == dimension)
                {
                    voxelMin[dim] = sliceIndex;
                    voxelMax[dim] = sliceIndex + 1;
                    dbg.add("Slice dim %d at index %d", dim, sliceIndex);
                }
                else
                {
                    voxelMin[dim] = 0;
                    voxelMax[dim] = layout->GetDimensionNumSamples(dim);
                    displayDims[displayDimCount++] = dim;
                }
            }

            // For 4D data, fix extra dimensions at midpoint
            if (displayDimCount > 2)
            {
                int dim = displayDims[2];
                int midpoint = layout->GetDimensionNumSamples(dim) / 2;
                voxelMin[dim] = midpoint;
                voxelMax[dim] = midpoint + 1;
            }
        }

        int dim0 = displayDims[0];
        int dim1 = displayDims[1];
        int dim0Size = layout->GetDimensionNumSamples(dim0);
        int dim1Size = layout->GetDimensionNumSamples(dim1);

        dbg.add("Display: dim%d x dim%d (%dx%d)", dim0, dim1, dim0Size, dim1Size);

        // Determine if transpose is needed
        bool needsTranspose = false;

        if (dimensionality == 2)
        {
            auto axis0 = layout->GetAxisDescriptor(0);
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

        int width, height;
        if (needsTranspose)
        {
            width = dim1Size;
            height = dim0Size;
            dbg.add("Transpose: YES (%dx%d)", width, height);
        }
        else
        {
            width = dim0Size;
            height = dim1Size;
            dbg.add("Transpose: NO (%dx%d)", width, height);
        }

        std::vector<float> buffer(dim0Size * dim1Size);

        // Find an available dimension group
        OpenVDS::DimensionsND dimGroup = OpenVDS::Dimensions_012;
        const char* dimGroupName = "Dimensions_012";

        OpenVDS::DimensionsND candidates[] = {
            OpenVDS::Dimensions_01,
            OpenVDS::Dimensions_012,
            OpenVDS::Dimensions_02
        };
        const char* candidateNames[] = {
            "Dimensions_01",
            "Dimensions_012",
            "Dimensions_02"
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

        dbg.add("DimGroup: %s", dimGroupName);

        auto request = accessManager.RequestVolumeSubset<float>(
            buffer.data(),
            buffer.size() * sizeof(float),
            dimGroup,
            0, 0,
            voxelMin, voxelMax
        );

        if (!request->WaitForCompletion())
        {
            dbg.add("");
            dbg.add("ERROR: Request failed!");
            dbg.add("Code: %d", request->GetErrorCode());
            std::string errMsg = request->GetErrorMessage();
            // Split long error messages
            if (errMsg.length() > 40)
            {
                dbg.add("Msg: %s", errMsg.substr(0, 40).c_str());
                for (size_t i = 40; i < errMsg.length(); i += 40)
                    dbg.add("     %s", errMsg.substr(i, 40).c_str());
            }
            else
            {
                dbg.add("Msg: %s", errMsg.c_str());
            }

            if (generateDebugOnError)
                return CreateDebugBitmap(256, 256, dbg.lines);
            return nullptr;
        }
        dbg.add("Request: OK");

        // Compute actual min/max
        float minVal = buffer[0];
        float maxVal = buffer[0];
        int nanCount = 0;
        int infCount = 0;

        for (size_t i = 0; i < buffer.size(); i++)
        {
            float v = buffer[i];
            if (std::isnan(v)) { nanCount++; continue; }
            if (std::isinf(v)) { infCount++; continue; }
            if (v < minVal) minVal = v;
            if (v > maxVal) maxVal = v;
        }

        printf("  Data range: [%g, %g]\n", minVal, maxVal);
        if (nanCount > 0) printf("  WARNING: %d NaN values\n", nanCount);
        if (infCount > 0) printf("  WARNING: %d Inf values\n", infCount);

        // Check channel metadata range
        float metaMin = layout->GetChannelValueRangeMin(0);
        float metaMax = layout->GetChannelValueRangeMax(0);
        printf("  Channel metadata range: [%g, %g]\n", metaMin, metaMax);

        if (maxVal - minVal < 1e-6f)
        {
            printf("  WARNING: Degenerate range, using metadata\n");
            minVal = metaMin;
            maxVal = metaMax;
        }

        // Create grayscale with transpose handling
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

        dbg.add("Creating bitmap...");
        return CreateColorizedBitmap(grayscale.data(), width, height);
    }
    catch (const std::exception& e)
    {
        dbg.add("");
        dbg.add("EXCEPTION: %s", e.what());
        if (generateDebugOnError)
            return CreateDebugBitmap(256, 256, dbg.lines);
        return nullptr;
    }
    catch (...)
    {
        dbg.add("");
        dbg.add("UNKNOWN EXCEPTION");
        if (generateDebugOnError)
            return CreateDebugBitmap(256, 256, dbg.lines);
        return nullptr;
    }
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 2)
    {
        printf("Usage: VdsRenderTest.exe <input.vds> [output.bmp] [dimension] [sliceIndex]\n");
        printf("\n");
        printf("  input.vds   - Path to VDS file\n");
        printf("  output.bmp  - Output bitmap file (default: output.bmp)\n");
        printf("  dimension   - Dimension to slice (default: 2 for 3D, ignored for 2D)\n");
        printf("  sliceIndex  - Slice index (default: middle slice)\n");
        return 1;
    }

    const wchar_t* inputPath = argv[1];
    const wchar_t* outputPath = (argc > 2) ? argv[2] : L"output.bmp";
    int dimension = (argc > 3) ? _wtoi(argv[3]) : -1;
    int sliceIndex = (argc > 4) ? _wtoi(argv[4]) : -1;

    printf("Input: %ls\n", inputPath);
    printf("Output: %ls\n", outputPath);

    // Convert wide string to narrow for OpenVDS
    char narrowPath[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, inputPath, -1, narrowPath, MAX_PATH, nullptr, nullptr);

    // Open VDS file
    OpenVDS::Error error;
    OpenVDS::VDSHandle vdsHandle = OpenVDS::Open(narrowPath, error);

    if (error.code != 0 || !vdsHandle)
    {
        printf("ERROR: Failed to open VDS: %s\n", error.string.c_str());
        return 1;
    }

    OpenVDS::VolumeDataLayout* layout = OpenVDS::GetLayout(vdsHandle);
    if (!layout)
    {
        printf("ERROR: Failed to get layout\n");
        OpenVDS::Close(vdsHandle, error);
        return 1;
    }

    PrintVdsInfo(layout);

    int dimensionality = layout->GetDimensionality();

    // Set defaults
    if (dimension < 0)
    {
        dimension = (dimensionality >= 3) ? 2 : 0;
    }

    if (sliceIndex < 0)
    {
        sliceIndex = layout->GetDimensionNumSamples(dimension) / 2;
    }

    // Render
    HBITMAP hBitmap = RenderSlice(vdsHandle, layout, dimension, sliceIndex);

    if (!hBitmap)
    {
        printf("ERROR: Failed to render slice\n");
        OpenVDS::Close(vdsHandle, error);
        return 1;
    }

    // Save to file
    if (SaveBitmapToFile(hBitmap, outputPath))
    {
        printf("\nSuccess! Saved to: %ls\n", outputPath);
    }
    else
    {
        printf("\nERROR: Failed to save bitmap\n");
    }

    DeleteObject(hBitmap);
    OpenVDS::Close(vdsHandle, error);

    return 0;
}
