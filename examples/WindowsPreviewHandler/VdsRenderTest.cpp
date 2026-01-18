/****************************************************************************
** VDS Render Test Harness
**
** Command-line tool to test VDS rendering logic and benchmark performance.
**
** Usage:
**   VdsRenderTest.exe <input.vds> [output.bmp] [dimension] [sliceIndex]
**   VdsRenderTest.exe --benchmark <folder> [--output report.csv]
**
** Benchmark mode recursively processes all .vds files, testing:
**   - Different slice dimensions
**   - Different LOD levels
**   - Recording timing and success/failure
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
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;

// ============================================================================
// IStream wrapper for file - mimics how preview handler accesses VDS
// ============================================================================

class FileStream : public IStream
{
public:
    FileStream(const std::wstring& filepath) : m_refCount(1), m_position(0)
    {
        m_hFile = CreateFileW(filepath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_hFile != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER size;
            GetFileSizeEx(m_hFile, &size);
            m_size = size.QuadPart;
        }
        else
        {
            m_size = 0;
        }
    }

    ~FileStream()
    {
        if (m_hFile != INVALID_HANDLE_VALUE)
            CloseHandle(m_hFile);
    }

    bool IsValid() const { return m_hFile != INVALID_HANDLE_VALUE; }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream)
        {
            *ppv = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }

    // ISequentialStream
    STDMETHODIMP Read(void* pv, ULONG cb, ULONG* pcbRead) override
    {
        if (!pv) return E_INVALIDARG;
        if (m_hFile == INVALID_HANDLE_VALUE) return E_FAIL;

        LARGE_INTEGER pos;
        pos.QuadPart = m_position;
        if (!SetFilePointerEx(m_hFile, pos, nullptr, FILE_BEGIN))
            return HRESULT_FROM_WIN32(GetLastError());

        DWORD bytesRead = 0;
        if (!ReadFile(m_hFile, pv, cb, &bytesRead, nullptr))
            return HRESULT_FROM_WIN32(GetLastError());

        m_position += bytesRead;
        if (pcbRead) *pcbRead = bytesRead;
        return (bytesRead == cb) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Write(const void*, ULONG, ULONG*) override { return E_NOTIMPL; }

    // IStream
    STDMETHODIMP Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) override
    {
        LONGLONG newPos = m_position;
        switch (dwOrigin)
        {
        case STREAM_SEEK_SET: newPos = dlibMove.QuadPart; break;
        case STREAM_SEEK_CUR: newPos += dlibMove.QuadPart; break;
        case STREAM_SEEK_END: newPos = m_size + dlibMove.QuadPart; break;
        default: return E_INVALIDARG;
        }
        if (newPos < 0) return E_INVALIDARG;
        m_position = newPos;
        if (plibNewPosition) plibNewPosition->QuadPart = m_position;
        return S_OK;
    }

    STDMETHODIMP SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    STDMETHODIMP Commit(DWORD) override { return S_OK; }
    STDMETHODIMP Revert() override { return E_NOTIMPL; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }

    STDMETHODIMP Stat(STATSTG* pstatstg, DWORD grfStatFlag) override
    {
        if (!pstatstg) return E_INVALIDARG;
        ZeroMemory(pstatstg, sizeof(STATSTG));
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart = m_size;
        return S_OK;
    }

    STDMETHODIMP Clone(IStream**) override { return E_NOTIMPL; }

private:
    LONG m_refCount;
    HANDLE m_hFile;
    LONGLONG m_size;
    LONGLONG m_position;
};

// ============================================================================
// Timing utilities
// ============================================================================

using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::duration<double, std::milli>;

double ElapsedMs(TimePoint start, TimePoint end)
{
    return std::chrono::duration_cast<Duration>(end - start).count();
}

// Calculate the number of voxels at a given LOD for a range [voxelMin, voxelMax)
// At LOD 0, returns voxelMax - voxelMin. At LOD 1, returns half that, etc.
// This matches OpenVDS::GetLODSize() from VolumeData.h
static inline int GetLODSize(int voxelMin, int voxelMax, int lod)
{
    return ((voxelMax - voxelMin - 1) >> lod) + 1;
}

// ============================================================================
// Result tracking
// ============================================================================

struct RenderResult
{
    std::string filename;
    int dimensionality;
    int dimension;      // Which dimension was sliced (-1 for 2D)
    int sliceIndex;
    int lod;
    std::string dimGroup;
    int outputWidth;
    int outputHeight;
    int64_t totalVoxels;
    bool success;
    std::string errorMessage;
    double openTimeMs;
    double renderTimeMs;
    double totalTimeMs;
};

std::vector<RenderResult> g_results;

void WriteResultsCsv(const std::string& filename)
{
    std::ofstream out(filename);
    if (!out.is_open())
    {
        printf("ERROR: Cannot write to %s\n", filename.c_str());
        return;
    }

    // Header
    out << "Filename,Dimensionality,SliceDim,SliceIndex,LOD,DimGroup,"
        << "Width,Height,TotalVoxels,Success,ErrorMessage,"
        << "OpenTimeMs,RenderTimeMs,TotalTimeMs\n";

    for (const auto& r : g_results)
    {
        out << "\"" << r.filename << "\","
            << r.dimensionality << ","
            << r.dimension << ","
            << r.sliceIndex << ","
            << r.lod << ","
            << "\"" << r.dimGroup << "\","
            << r.outputWidth << ","
            << r.outputHeight << ","
            << r.totalVoxels << ","
            << (r.success ? "TRUE" : "FALSE") << ","
            << "\"" << r.errorMessage << "\","
            << std::fixed << std::setprecision(2)
            << r.openTimeMs << ","
            << r.renderTimeMs << ","
            << r.totalTimeMs << "\n";
    }

    out.close();
    printf("\nResults written to: %s\n", filename.c_str());
}

void PrintResultsSummary()
{
    if (g_results.empty())
    {
        printf("\nNo results to summarize.\n");
        return;
    }

    int totalTests = (int)g_results.size();
    int successes = 0;
    int failures = 0;
    double totalRenderTime = 0;
    double minRenderTime = 1e9;
    double maxRenderTime = 0;

    std::map<std::string, int> errorCounts;

    for (const auto& r : g_results)
    {
        if (r.success)
        {
            successes++;
            totalRenderTime += r.renderTimeMs;
            minRenderTime = std::min(minRenderTime, r.renderTimeMs);
            maxRenderTime = std::max(maxRenderTime, r.renderTimeMs);
        }
        else
        {
            failures++;
            errorCounts[r.errorMessage]++;
        }
    }

    printf("\n");
    printf("================================================================================\n");
    printf("BENCHMARK SUMMARY\n");
    printf("================================================================================\n");
    printf("Total tests:     %d\n", totalTests);
    printf("Successes:       %d (%.1f%%)\n", successes, 100.0 * successes / totalTests);
    printf("Failures:        %d (%.1f%%)\n", failures, 100.0 * failures / totalTests);

    if (successes > 0)
    {
        printf("\nRender times (successful):\n");
        printf("  Min:     %.2f ms\n", minRenderTime);
        printf("  Max:     %.2f ms\n", maxRenderTime);
        printf("  Average: %.2f ms\n", totalRenderTime / successes);
    }

    if (!errorCounts.empty())
    {
        printf("\nError breakdown:\n");
        for (const auto& [msg, count] : errorCounts)
        {
            printf("  [%d] %s\n", count, msg.c_str());
        }
    }
    printf("================================================================================\n");
}

// ============================================================================
// Bitmap utilities
// ============================================================================

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
    bmih.biHeight = bm.bmHeight;
    bmih.biPlanes = 1;
    bmih.biBitCount = 32;
    bmih.biCompression = BI_RGB;
    bmih.biSizeImage = bm.bmWidth * bm.bmHeight * 4;

    bmfh.bfType = 0x4D42;
    bmfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bmfh.bfSize = bmfh.bfOffBits + bmih.biSizeImage;

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

    FILE* f = nullptr;
    if (_wfopen_s(&f, filename, L"wb") != 0 || !f)
        return false;

    fwrite(&bmfh, sizeof(bmfh), 1, f);
    fwrite(&bmih, sizeof(bmih), 1, f);
    fwrite(pixels.data(), 1, pixels.size(), f);
    fclose(f);

    return true;
}

HBITMAP CreateColorizedBitmap(const uint8_t* grayscaleData, int width, int height)
{
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
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

// ============================================================================
// VDS Info
// ============================================================================

void PrintVdsInfo(OpenVDS::VolumeDataLayout* layout, bool verbose = true)
{
    if (verbose)
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
}

// ============================================================================
// LOD Information
// ============================================================================

struct LODInfo
{
    int lod;
    bool available;
    std::string status;  // "Normal", "Remapped", "Unavailable"
    int dim0Samples;
    int dim1Samples;
};

std::vector<LODInfo> GetAvailableLODs(OpenVDS::VolumeDataAccessManager& accessManager,
                                       OpenVDS::VolumeDataLayout* layout,
                                       OpenVDS::DimensionsND dimGroup)
{
    std::vector<LODInfo> lods;

    // LODLevels enum value equals the number of LOD levels
    int maxLOD = static_cast<int>(layout->GetLayoutDescriptor().GetLODLevels());
    if (maxLOD == 0) maxLOD = 1;  // At least LOD0 is always available

    for (int lod = 0; lod < maxLOD; lod++)
    {
        LODInfo info;
        info.lod = lod;

        auto status = accessManager.GetVDSProduceStatus(dimGroup, lod, 0);
        switch (status)
        {
        case OpenVDS::VDSProduceStatus::Normal:
            info.available = true;
            info.status = "Normal";
            break;
        case OpenVDS::VDSProduceStatus::Remapped:
            info.available = true;
            info.status = "Remapped";
            break;
        case OpenVDS::VDSProduceStatus::Unavailable:
        default:
            info.available = false;
            info.status = "Unavailable";
            break;
        }

        // Calculate expected dimensions at this LOD
        // Note: OpenVDS returns full-res data regardless of LOD requested,
        // but higher LOD = faster wavelet decompression
        info.dim0Samples = layout->GetDimensionNumSamples(0);
        info.dim1Samples = layout->GetDimensionNumSamples(1);

        lods.push_back(info);
    }

    return lods;
}

// ============================================================================
// Core rendering with timing
// ============================================================================

struct RenderParams
{
    int dimension;
    int sliceIndex;
    int lod;
    bool quiet;  // Suppress per-render output
};

struct RenderOutput
{
    HBITMAP bitmap;
    int width;
    int height;
    int64_t totalVoxels;
    std::string dimGroup;
    bool success;
    std::string errorMessage;
    double renderTimeMs;
};

RenderOutput RenderSliceWithTiming(OpenVDS::VDSHandle vdsHandle,
                                    OpenVDS::VolumeDataLayout* layout,
                                    const RenderParams& params)
{
    RenderOutput output = {};
    output.bitmap = nullptr;
    output.success = false;

    TimePoint startRender = Clock::now();

    int dimensionality = layout->GetDimensionality();

    try
    {
        OpenVDS::VolumeDataAccessManager accessManager = OpenVDS::GetAccessManager(vdsHandle);

        int voxelMin[OpenVDS::Dimensionality_Max] = { 0, 0, 0, 0, 0, 0 };
        int voxelMax[OpenVDS::Dimensionality_Max] = { 1, 1, 1, 1, 1, 1 };

        int displayDims[OpenVDS::Dimensionality_Max - 1];
        int displayDimCount = 0;

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
            for (int dim = 0; dim < dimensionality; dim++)
            {
                if (dim == params.dimension)
                {
                    voxelMin[dim] = params.sliceIndex;
                    voxelMax[dim] = params.sliceIndex + 1;
                }
                else
                {
                    voxelMin[dim] = 0;
                    voxelMax[dim] = layout->GetDimensionNumSamples(dim);
                    displayDims[displayDimCount++] = dim;
                }
            }

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

        // Determine transpose
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

        // Calculate LOD-sized buffer dimensions
        // OpenVDS returns smaller buffers at higher LODs: LOD N returns 1/(2^N) resolution per axis
        // BUT the "full resolution dimension" is NOT decimated at higher LODs
        auto layoutDescriptor = layout->GetLayoutDescriptor();
        int fullResDim = layoutDescriptor.GetFullResolutionDimension();

        int lodDim0Size, lodDim1Size;
        if (fullResDim == dim0)
        {
            // dim0 keeps full resolution
            lodDim0Size = voxelMax[dim0] - voxelMin[dim0];
            lodDim1Size = GetLODSize(voxelMin[dim1], voxelMax[dim1], params.lod);
        }
        else if (fullResDim == dim1)
        {
            // dim1 keeps full resolution
            lodDim0Size = GetLODSize(voxelMin[dim0], voxelMax[dim0], params.lod);
            lodDim1Size = voxelMax[dim1] - voxelMin[dim1];
        }
        else
        {
            // Neither dim0 nor dim1 is full resolution, both are decimated
            lodDim0Size = GetLODSize(voxelMin[dim0], voxelMax[dim0], params.lod);
            lodDim1Size = GetLODSize(voxelMin[dim1], voxelMax[dim1], params.lod);
        }

        int width = needsTranspose ? lodDim1Size : lodDim0Size;
        int height = needsTranspose ? lodDim0Size : lodDim1Size;

        output.width = width;
        output.height = height;
        output.totalVoxels = (int64_t)lodDim0Size * lodDim1Size;

        // Use OpenVDS to calculate exact buffer size (safest approach)
        int64_t expectedBufferSize = accessManager.GetVolumeSubsetBufferSize<float>(voxelMin, voxelMax, params.lod, 0);
        size_t bufferFloatCount = static_cast<size_t>(expectedBufferSize / sizeof(float));
        std::vector<float> buffer(bufferFloatCount);

        // Find available dimension group
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

        bool foundGroup = false;
        for (int i = 0; i < 3; i++)
        {
            auto status = accessManager.GetVDSProduceStatus(candidates[i], params.lod, 0);
            if (status != OpenVDS::VDSProduceStatus::Unavailable)
            {
                dimGroup = candidates[i];
                dimGroupName = candidateNames[i];
                foundGroup = true;
                break;
            }
        }

        output.dimGroup = dimGroupName;

        if (!foundGroup)
        {
            output.errorMessage = "No available dimension group for LOD " + std::to_string(params.lod);
            output.renderTimeMs = ElapsedMs(startRender, Clock::now());
            return output;
        }

        // Diagnostic: dump all request parameters
        printf("=== RequestVolumeSubset Parameters ===\n");
        printf("  voxelMin: [%d, %d, %d, %d, %d, %d]\n",
               voxelMin[0], voxelMin[1], voxelMin[2], voxelMin[3], voxelMin[4], voxelMin[5]);
        printf("  voxelMax: [%d, %d, %d, %d, %d, %d]\n",
               voxelMax[0], voxelMax[1], voxelMax[2], voxelMax[3], voxelMax[4], voxelMax[5]);
        printf("  LOD: %d, Channel: 0\n", params.lod);
        printf("  DimGroup: %s\n", dimGroupName);
        printf("  fullResDim: %d\n", fullResDim);
        printf("  lodDim0Size: %d, lodDim1Size: %d\n", lodDim0Size, lodDim1Size);
        printf("  buffer size: %zu floats (%zu bytes)\n", buffer.size(), buffer.size() * sizeof(float));
        printf("  expectedBufferSize from API: %lld bytes\n", (long long)expectedBufferSize);
        printf("======================================\n");
        fflush(stdout);

        // Request data
        auto request = accessManager.RequestVolumeSubset<float>(
            buffer.data(),
            buffer.size() * sizeof(float),
            dimGroup,
            params.lod, 0,
            voxelMin, voxelMax
        );

        if (!request->WaitForCompletion())
        {
            output.errorMessage = request->GetErrorMessage();
            if (output.errorMessage.empty())
                output.errorMessage = "Request failed (code " + std::to_string(request->GetErrorCode()) + ")";
            output.renderTimeMs = ElapsedMs(startRender, Clock::now());
            return output;
        }

        // Compute min/max
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
            minVal = layout->GetChannelValueRangeMin(0);
            maxVal = layout->GetChannelValueRangeMax(0);
        }

        // Create grayscale
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

        output.bitmap = CreateColorizedBitmap(grayscale.data(), width, height);
        output.success = (output.bitmap != nullptr);
        if (!output.success)
            output.errorMessage = "Failed to create bitmap";
    }
    catch (const std::exception& e)
    {
        output.errorMessage = std::string("Exception: ") + e.what();
    }
    catch (...)
    {
        output.errorMessage = "Unknown exception";
    }

    output.renderTimeMs = ElapsedMs(startRender, Clock::now());
    return output;
}

// ============================================================================
// Benchmark a single VDS file
// ============================================================================

void BenchmarkVdsFile(const std::string& filepath, bool verbose = false)
{
    printf("\n--- %s ---\n", filepath.c_str());

    TimePoint startOpen = Clock::now();

    OpenVDS::Error error;
    OpenVDS::VDSHandle vdsHandle = OpenVDS::Open(filepath, error);

    double openTimeMs = ElapsedMs(startOpen, Clock::now());

    if (error.code != 0 || !vdsHandle)
    {
        printf("  ERROR: Failed to open: %s\n", error.string.c_str());
        RenderResult result;
        result.filename = filepath;
        result.dimensionality = 0;
        result.dimension = -1;
        result.sliceIndex = -1;
        result.lod = 0;
        result.success = false;
        result.errorMessage = "Open failed: " + error.string;
        result.openTimeMs = openTimeMs;
        result.renderTimeMs = 0;
        result.totalTimeMs = openTimeMs;
        g_results.push_back(result);
        return;
    }

    OpenVDS::VolumeDataLayout* layout = OpenVDS::GetLayout(vdsHandle);
    if (!layout)
    {
        printf("  ERROR: Failed to get layout\n");
        RenderResult result;
        result.filename = filepath;
        result.dimensionality = 0;
        result.dimension = -1;
        result.sliceIndex = -1;
        result.lod = 0;
        result.success = false;
        result.errorMessage = "GetLayout failed";
        result.openTimeMs = openTimeMs;
        result.renderTimeMs = 0;
        result.totalTimeMs = openTimeMs;
        g_results.push_back(result);
        OpenVDS::Close(vdsHandle, error);
        return;
    }

    int dimensionality = layout->GetDimensionality();
    // LODLevels enum value equals the number of LOD levels (0 means only LOD0)
    int lodCount = static_cast<int>(layout->GetLayoutDescriptor().GetLODLevels());
    if (lodCount == 0) lodCount = 1;  // At least LOD0

    printf("  Dimensionality: %d, LOD count: %d, Open time: %.2f ms\n",
           dimensionality, lodCount, openTimeMs);

    // Print dimensions
    for (int d = 0; d < dimensionality; d++)
    {
        auto axis = layout->GetAxisDescriptor(d);
        printf("    [%d] %s: %d samples\n", d, axis.GetName(), layout->GetDimensionNumSamples(d));
    }

    OpenVDS::VolumeDataAccessManager accessManager = OpenVDS::GetAccessManager(vdsHandle);

    // Determine which dimensions to slice
    std::vector<int> sliceDimensions;
    if (dimensionality == 2)
    {
        sliceDimensions.push_back(-1);  // -1 means render full 2D
    }
    else
    {
        // Test slicing each dimension
        for (int d = 0; d < dimensionality && d < 3; d++)
        {
            sliceDimensions.push_back(d);
        }
    }

    // Determine which LODs to test
    std::vector<int> lodsToTest;
    for (int lod = 0; lod < lodCount; lod++)
    {
        lodsToTest.push_back(lod);
    }
    if (lodsToTest.empty())
        lodsToTest.push_back(0);

    // Test each combination
    for (int sliceDim : sliceDimensions)
    {
        int sliceIndex = 0;
        if (sliceDim >= 0)
        {
            sliceIndex = layout->GetDimensionNumSamples(sliceDim) / 2;
        }

        for (int lod : lodsToTest)
        {
            RenderParams params;
            params.dimension = (sliceDim >= 0) ? sliceDim : 0;
            params.sliceIndex = sliceIndex;
            params.lod = lod;
            params.quiet = !verbose;

            // Check LOD availability first
            OpenVDS::DimensionsND testGroup = OpenVDS::Dimensions_012;
            if (dimensionality == 2)
                testGroup = OpenVDS::Dimensions_01;

            auto lodStatus = accessManager.GetVDSProduceStatus(testGroup, lod, 0);
            const char* lodStatusStr = "Unknown";
            switch (lodStatus)
            {
            case OpenVDS::VDSProduceStatus::Normal: lodStatusStr = "Normal"; break;
            case OpenVDS::VDSProduceStatus::Remapped: lodStatusStr = "Remapped"; break;
            case OpenVDS::VDSProduceStatus::Unavailable: lodStatusStr = "Unavailable"; break;
            }

            if (sliceDim < 0)
            {
                printf("  Test: 2D render, LOD%d (%s)... ", lod, lodStatusStr);
            }
            else
            {
                printf("  Test: dim%d slice %d, LOD%d (%s)... ", sliceDim, sliceIndex, lod, lodStatusStr);
            }
            fflush(stdout);

            TimePoint startTest = Clock::now();
            RenderOutput output = RenderSliceWithTiming(vdsHandle, layout, params);
            double totalTestMs = ElapsedMs(startTest, Clock::now());

            RenderResult result;
            result.filename = filepath;
            result.dimensionality = dimensionality;
            result.dimension = sliceDim;
            result.sliceIndex = sliceIndex;
            result.lod = lod;
            result.dimGroup = output.dimGroup;
            result.outputWidth = output.width;
            result.outputHeight = output.height;
            result.totalVoxels = output.totalVoxels;
            result.success = output.success;
            result.errorMessage = output.errorMessage;
            result.openTimeMs = openTimeMs;
            result.renderTimeMs = output.renderTimeMs;
            result.totalTimeMs = totalTestMs;

            if (output.success)
            {
                printf("OK (%.2f ms, %dx%d, %s)\n",
                       output.renderTimeMs, output.width, output.height, output.dimGroup.c_str());
                DeleteObject(output.bitmap);
            }
            else
            {
                printf("FAILED: %s\n", output.errorMessage.c_str());
            }

            g_results.push_back(result);
        }
    }

    OpenVDS::Close(vdsHandle, error);
}

// ============================================================================
// Recursive folder scanning
// ============================================================================

std::vector<std::string> FindVdsFiles(const std::string& rootPath)
{
    std::vector<std::string> files;

    try
    {
        for (const auto& entry : fs::recursive_directory_iterator(rootPath))
        {
            if (entry.is_regular_file())
            {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".vds")
                {
                    files.push_back(entry.path().string());
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        printf("Error scanning directory: %s\n", e.what());
    }

    return files;
}

// ============================================================================
// Single file render (original behavior)
// ============================================================================

HBITMAP RenderSlice(OpenVDS::VDSHandle vdsHandle, OpenVDS::VolumeDataLayout* layout,
                    int dimension, int sliceIndex)
{
    RenderParams params;
    params.dimension = dimension;
    params.sliceIndex = sliceIndex;
    params.lod = 0;
    params.quiet = false;

    RenderOutput output = RenderSliceWithTiming(vdsHandle, layout, params);

    if (!output.success)
    {
        printf("ERROR: %s\n", output.errorMessage.c_str());
    }

    return output.bitmap;
}

// ============================================================================
// Main
// ============================================================================

// ============================================================================
// LOD Comparison Test - renders same slice at all LODs, scales to maxSize
// ============================================================================

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

// Calculate optimal LOD for a given slice size and target display size
// Returns the highest LOD where effective resolution >= targetSize
int CalculateOptimalLOD(int sliceWidth, int sliceHeight, int targetSize, int maxLOD)
{
    int minSliceDim = std::min(sliceWidth, sliceHeight);

    // Find highest LOD that still provides >= targetSize effective resolution
    for (int lod = maxLOD; lod >= 0; lod--)
    {
        int effectiveRes = minSliceDim >> lod;  // Resolution at this LOD
        if (effectiveRes >= targetSize)
            return lod;  // This LOD has enough resolution
    }
    return 0;  // Fall back to LOD 0
}

void RunLODComparisonTest(const std::string& filepath, int targetMaxSize)
{
    printf("\n================================================================================\n");
    printf("LOD COMPARISON TEST\n");
    printf("================================================================================\n");
    printf("File: %s\n", filepath.c_str());
    printf("Target display size: %d pixels\n\n", targetMaxSize);

    TimePoint startOpen = Clock::now();

    OpenVDS::Error error;
    OpenVDS::VDSHandle vdsHandle = OpenVDS::Open(filepath, error);

    double openTimeMs = ElapsedMs(startOpen, Clock::now());

    if (error.code != 0 || !vdsHandle)
    {
        printf("ERROR: Failed to open: %s\n", error.string.c_str());
        return;
    }

    OpenVDS::VolumeDataLayout* layout = OpenVDS::GetLayout(vdsHandle);
    if (!layout)
    {
        printf("ERROR: Failed to get layout\n");
        OpenVDS::Close(vdsHandle, error);
        return;
    }

    int dimensionality = layout->GetDimensionality();
    int lodCount = static_cast<int>(layout->GetLayoutDescriptor().GetLODLevels());
    if (lodCount == 0) lodCount = 1;

    printf("Open time: %.2f ms\n", openTimeMs);
    printf("Dimensionality: %d\n", dimensionality);
    printf("LOD count: %d\n\n", lodCount);

    // Print dimensions
    printf("Dimensions:\n");
    for (int d = 0; d < dimensionality; d++)
    {
        auto axis = layout->GetAxisDescriptor(d);
        printf("  [%d] %s: %d samples\n", d, axis.GetName(), layout->GetDimensionNumSamples(d));
    }
    printf("\n");

    // Get slice dimensions (dim0 x dim1)
    int dim0Size = layout->GetDimensionNumSamples(0);
    int dim1Size = layout->GetDimensionNumSamples(1);
    int sliceWidth = dim1Size;  // After transpose: dim1 is X (width)
    int sliceHeight = dim0Size; // After transpose: dim0 is Y (height)

    printf("Slice size: %d x %d (%d voxels)\n", sliceWidth, sliceHeight, sliceWidth * sliceHeight);

    // Calculate optimal LOD
    int optimalLOD = CalculateOptimalLOD(sliceWidth, sliceHeight, targetMaxSize, lodCount - 1);
    printf("Calculated optimal LOD for %d px target: LOD %d\n\n", targetMaxSize, optimalLOD);

    // Set up render parameters
    int sliceDim = (dimensionality >= 3) ? 2 : 0;
    int sliceIndex = (dimensionality >= 3) ? layout->GetDimensionNumSamples(2) / 2 : 0;

    printf("Slicing dimension %d at index %d\n\n", sliceDim, sliceIndex);

    OpenVDS::VolumeDataAccessManager accessManager = OpenVDS::GetAccessManager(vdsHandle);

    // Test each LOD
    printf("%-6s  %-12s  %-10s  %-12s  %-10s  %-s\n",
           "LOD", "Status", "EffRes", "RenderTime", "ScaledSize", "Output");
    printf("------  ------------  ----------  ------------  ----------  ---------------\n");

    for (int lod = 0; lod < lodCount; lod++)
    {
        // Check availability
        OpenVDS::DimensionsND dimGroup = (dimensionality == 2) ? OpenVDS::Dimensions_01 : OpenVDS::Dimensions_012;
        auto lodStatus = accessManager.GetVDSProduceStatus(dimGroup, lod, 0);

        const char* statusStr = "Unknown";
        bool available = false;
        switch (lodStatus)
        {
        case OpenVDS::VDSProduceStatus::Normal:
            statusStr = "Normal";
            available = true;
            break;
        case OpenVDS::VDSProduceStatus::Remapped:
            statusStr = "Remapped";
            available = true;
            break;
        case OpenVDS::VDSProduceStatus::Unavailable:
            statusStr = "Unavailable";
            break;
        }

        int effectiveRes = std::min(sliceWidth, sliceHeight) >> lod;

        if (!available)
        {
            printf("LOD%d    %-12s  %-10s  %-12s  %-10s  %s\n",
                   lod, statusStr, "-", "-", "-", "(skipped)");
            continue;
        }

        // Render at this LOD
        RenderParams params;
        params.dimension = sliceDim;
        params.sliceIndex = sliceIndex;
        params.lod = lod;
        params.quiet = true;

        RenderOutput output = RenderSliceWithTiming(vdsHandle, layout, params);

        if (!output.success)
        {
            printf("LOD%d    %-12s  %4d px     FAILED       -           %s\n",
                   lod, statusStr, effectiveRes, output.errorMessage.c_str());
            continue;
        }

        // Scale to target size
        HBITMAP scaledBitmap = ScaleBitmap(output.bitmap, targetMaxSize);

        BITMAP bm;
        GetObject(scaledBitmap, sizeof(bm), &bm);

        // Generate output filename
        char outFilename[64];
        sprintf_s(outFilename, "lod_compare_%d.bmp", lod);
        wchar_t wOutFilename[64];
        MultiByteToWideChar(CP_UTF8, 0, outFilename, -1, wOutFilename, 64);

        bool saved = SaveBitmapToFile(scaledBitmap, wOutFilename);
        DeleteObject(scaledBitmap);

        char sizeStr[32];
        sprintf_s(sizeStr, "%dx%d", bm.bmWidth, bm.bmHeight);

        printf("LOD%d    %-12s  %4d px     %8.2f ms  %-10s  %s%s\n",
               lod, statusStr, effectiveRes, output.renderTimeMs, sizeStr,
               saved ? outFilename : "(save failed)",
               (lod == optimalLOD) ? " <-- OPTIMAL" : "");
    }

    printf("\n");
    printf("Legend:\n");
    printf("  EffRes = Effective resolution at this LOD (data is upsampled to full res)\n");
    printf("  OPTIMAL = Calculated optimal LOD for target size %d px\n", targetMaxSize);
    printf("\nCompare output files to verify visual quality:\n");
    for (int lod = 0; lod < lodCount; lod++)
    {
        printf("  lod_compare_%d.bmp\n", lod);
    }

    OpenVDS::Close(vdsHandle, error);
}

// Test all slices in a VDS file to find which ones crash
// Test all slices using IStream (like preview handler does)
void TestAllSlicesWithStream(const std::wstring& filepath, int dimension = -1, int lod = 0)
{
    printf("\n================================================================================\n");
    printf("TESTING ALL SLICES (via IStream)\n");
    printf("================================================================================\n");
    wprintf(L"File: %s\n", filepath.c_str());

    // Create IStream wrapper around file
    FileStream* pStream = new FileStream(filepath);
    if (!pStream->IsValid())
    {
        printf("ERROR: Failed to open file for IStream\n");
        pStream->Release();
        return;
    }

    STATSTG stat;
    pStream->Stat(&stat, STATFLAG_NONAME);
    printf("Stream size: %lld bytes\n", stat.cbSize.QuadPart);

    // Open VDS via IStream (same as preview handler)
    OpenVDS::IStreamOpenOptions options(pStream);
    OpenVDS::Error error;
    OpenVDS::VDSHandle vdsHandle = OpenVDS::Open(options, error);

    if (error.code != 0 || !vdsHandle)
    {
        printf("ERROR: Failed to open VDS via IStream: %s\n", error.string.c_str());
        pStream->Release();
        return;
    }

    OpenVDS::VolumeDataLayout* layout = OpenVDS::GetLayout(vdsHandle);
    if (!layout)
    {
        printf("ERROR: Failed to get layout\n");
        OpenVDS::Close(vdsHandle, error);
        pStream->Release();
        return;
    }

    int dimensionality = layout->GetDimensionality();
    printf("Dimensionality: %d\n", dimensionality);

    // Determine slice dimension
    if (dimension < 0)
    {
        dimension = (dimensionality >= 3) ? 2 : 0;
    }
    if (dimension >= dimensionality)
    {
        printf("ERROR: Dimension %d out of range (max: %d)\n", dimension, dimensionality - 1);
        OpenVDS::Close(vdsHandle, error);
        pStream->Release();
        return;
    }

    int sliceCount = layout->GetDimensionNumSamples(dimension);
    auto axis = layout->GetAxisDescriptor(dimension);
    printf("Testing dimension %d (%s): %d slices at LOD %d\n\n", dimension, axis.GetName(), sliceCount, lod);

    int successCount = 0;
    int failCount = 0;
    std::vector<int> failedSlices;

    for (int slice = 0; slice < sliceCount; slice++)
    {
        printf("\rSlice %d/%d...", slice + 1, sliceCount);
        fflush(stdout);

        RenderParams params;
        params.dimension = dimension;
        params.sliceIndex = slice;
        params.lod = lod;
        params.quiet = true;

        RenderOutput output = RenderSliceWithTiming(vdsHandle, layout, params);

        if (output.success)
        {
            successCount++;
            if (output.bitmap)
                DeleteObject(output.bitmap);
        }
        else
        {
            failCount++;
            failedSlices.push_back(slice);
            printf("\n  FAILED at slice %d: %s\n", slice, output.errorMessage.c_str());
        }
    }

    printf("\r                              \n");  // Clear progress line
    printf("\n================================================================================\n");
    printf("RESULTS (IStream mode)\n");
    printf("================================================================================\n");
    printf("Total slices: %d\n", sliceCount);
    printf("Success: %d (%.1f%%)\n", successCount, 100.0 * successCount / sliceCount);
    printf("Failed:  %d (%.1f%%)\n", failCount, 100.0 * failCount / sliceCount);

    if (!failedSlices.empty())
    {
        printf("\nFailed slices: ");
        for (size_t i = 0; i < failedSlices.size() && i < 20; i++)
        {
            if (i > 0) printf(", ");
            printf("%d", failedSlices[i]);
        }
        if (failedSlices.size() > 20)
            printf(", ... (%zu more)", failedSlices.size() - 20);
        printf("\n");
    }

    OpenVDS::Close(vdsHandle, error);
    pStream->Release();
}

void TestAllSlices(const std::string& filepath, int dimension = -1, int lod = 0)
{
    printf("\n================================================================================\n");
    printf("TESTING ALL SLICES\n");
    printf("================================================================================\n");
    printf("File: %s\n", filepath.c_str());

    OpenVDS::Error error;
    OpenVDS::VDSHandle vdsHandle = OpenVDS::Open(filepath, error);

    if (error.code != 0 || !vdsHandle)
    {
        printf("ERROR: Failed to open VDS: %s\n", error.string.c_str());
        return;
    }

    OpenVDS::VolumeDataLayout* layout = OpenVDS::GetLayout(vdsHandle);
    if (!layout)
    {
        printf("ERROR: Failed to get layout\n");
        OpenVDS::Close(vdsHandle, error);
        return;
    }

    int dimensionality = layout->GetDimensionality();
    printf("Dimensionality: %d\n", dimensionality);

    // Determine slice dimension
    if (dimension < 0)
    {
        dimension = (dimensionality >= 3) ? 2 : 0;
    }
    if (dimension >= dimensionality)
    {
        printf("ERROR: Dimension %d out of range (max: %d)\n", dimension, dimensionality - 1);
        OpenVDS::Close(vdsHandle, error);
        return;
    }

    int sliceCount = layout->GetDimensionNumSamples(dimension);
    auto axis = layout->GetAxisDescriptor(dimension);
    printf("Testing dimension %d (%s): %d slices at LOD %d\n\n", dimension, axis.GetName(), sliceCount, lod);

    int successCount = 0;
    int failCount = 0;
    std::vector<int> failedSlices;

    for (int slice = 0; slice < sliceCount; slice++)
    {
        printf("\rSlice %d/%d...", slice + 1, sliceCount);
        fflush(stdout);

        RenderParams params;
        params.dimension = dimension;
        params.sliceIndex = slice;
        params.lod = lod;
        params.quiet = true;

        RenderOutput output = RenderSliceWithTiming(vdsHandle, layout, params);

        if (output.success)
        {
            successCount++;
            if (output.bitmap)
                DeleteObject(output.bitmap);
        }
        else
        {
            failCount++;
            failedSlices.push_back(slice);
            printf("\n  FAILED at slice %d: %s\n", slice, output.errorMessage.c_str());
        }
    }

    printf("\r                              \n");  // Clear progress line
    printf("\n================================================================================\n");
    printf("RESULTS\n");
    printf("================================================================================\n");
    printf("Total slices: %d\n", sliceCount);
    printf("Success: %d (%.1f%%)\n", successCount, 100.0 * successCount / sliceCount);
    printf("Failed:  %d (%.1f%%)\n", failCount, 100.0 * failCount / sliceCount);

    if (!failedSlices.empty())
    {
        printf("\nFailed slices: ");
        for (size_t i = 0; i < failedSlices.size() && i < 20; i++)
        {
            if (i > 0) printf(", ");
            printf("%d", failedSlices[i]);
        }
        if (failedSlices.size() > 20)
            printf(", ... (%zu more)", failedSlices.size() - 20);
        printf("\n");
    }

    OpenVDS::Close(vdsHandle, error);
}

void PrintUsage()
{
    printf("VDS Render Test Harness\n\n");
    printf("Usage:\n");
    printf("  VdsRenderTest.exe <input.vds> [output.bmp] [dimension] [sliceIndex]\n");
    printf("  VdsRenderTest.exe --benchmark <folder> [--output report.csv] [--verbose]\n");
    printf("  VdsRenderTest.exe --lod-compare <input.vds> [maxSize]\n");
    printf("  VdsRenderTest.exe --all-slices <input.vds> [dimension] [lod]\n");
    printf("\n");
    printf("Single file mode:\n");
    printf("  input.vds   - Path to VDS file\n");
    printf("  output.bmp  - Output bitmap file (default: output.bmp)\n");
    printf("  dimension   - Dimension to slice (default: 2 for 3D, ignored for 2D)\n");
    printf("  sliceIndex  - Slice index (default: middle slice)\n");
    printf("\n");
    printf("Benchmark mode:\n");
    printf("  --benchmark <folder>  - Recursively process all .vds files in folder\n");
    printf("  --output <file.csv>   - Write results to CSV file (default: benchmark_results.csv)\n");
    printf("  --verbose             - Show detailed output for each render\n");
    printf("\n");
    printf("LOD comparison mode:\n");
    printf("  --lod-compare <vds>   - Render same slice at all LODs, scale to maxSize\n");
    printf("  maxSize               - Target display size in pixels (default: 400)\n");
    printf("                          Outputs: lod_compare_0.bmp, lod_compare_1.bmp, etc.\n");
    printf("\n");
    printf("All slices mode:\n");
    printf("  --all-slices <vds>    - Test rendering every slice to find crashes\n");
    printf("  dimension             - Dimension to slice (default: 2 for 3D)\n");
    printf("  lod                   - LOD level to use (default: 0)\n");
    printf("\n");
    printf("IStream mode (mimics preview handler):\n");
    printf("  --stream <vds>        - Test all slices using IStream (like preview handler)\n");
    printf("  dimension             - Dimension to slice (default: 2 for 3D)\n");
    printf("  lod                   - LOD level to use (default: 0)\n");
    printf("\n");
    printf("Benchmark tests each VDS with:\n");
    printf("  - Different slice dimensions (for 3D+ data)\n");
    printf("  - Different LOD levels where available\n");
    printf("  - Records timing and success/failure\n");
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 2)
    {
        PrintUsage();
        return 1;
    }

    // Parse all arguments flexibly - flags can be in any order
    std::wstring filepath;
    std::wstring outputPath = L"output.bmp";
    bool useStream = false;
    bool allSlices = false;
    bool lodCompare = false;
    bool benchmark = false;
    int dimension = -1;  // Auto-detect
    int lod = 0;
    int sliceIndex = -1;
    int maxSize = 400;
    bool verbose = false;

    for (int i = 1; i < argc; i++)
    {
        std::wstring arg = argv[i];

        if (arg == L"--stream" || arg == L"--istream")
        {
            useStream = true;
        }
        else if (arg == L"--all-slices" || arg == L"--test-slices")
        {
            allSlices = true;
        }
        else if (arg == L"--lod-compare" || arg == L"--lod-test")
        {
            lodCompare = true;
        }
        else if (arg == L"--benchmark" || arg == L"-b")
        {
            benchmark = true;
        }
        else if (arg == L"--verbose" || arg == L"-v")
        {
            verbose = true;
        }
        else if ((arg == L"--lod" || arg == L"-l") && i + 1 < argc)
        {
            lod = _wtoi(argv[++i]);
        }
        else if ((arg == L"--dim" || arg == L"-d") && i + 1 < argc)
        {
            dimension = _wtoi(argv[++i]);
        }
        else if ((arg == L"--slice" || arg == L"-s") && i + 1 < argc)
        {
            sliceIndex = _wtoi(argv[++i]);
        }
        else if ((arg == L"--output" || arg == L"-o") && i + 1 < argc)
        {
            outputPath = argv[++i];
        }
        else if ((arg == L"--size") && i + 1 < argc)
        {
            maxSize = _wtoi(argv[++i]);
        }
        else if (arg[0] != L'-' && filepath.empty())
        {
            // First non-flag argument is the filepath
            filepath = arg;
        }
    }

    if (filepath.empty())
    {
        printf("ERROR: No VDS file path specified\n");
        PrintUsage();
        return 1;
    }

    // Handle --stream --all-slices combination
    if (useStream && allSlices)
    {
        TestAllSlicesWithStream(filepath, dimension, lod);
        return 0;
    }

    // Handle --stream alone (single slice via IStream)
    if (useStream)
    {
        TestAllSlicesWithStream(filepath, dimension, lod);
        return 0;
    }

    // Handle --all-slices (without stream)
    if (allSlices)
    {
        char vdsPath[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, filepath.c_str(), -1, vdsPath, MAX_PATH, nullptr, nullptr);
        TestAllSlices(vdsPath, dimension, lod);
        return 0;
    }

    // Check for LOD comparison mode
    if (lodCompare)
    {
        char vdsPath[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, filepath.c_str(), -1, vdsPath, MAX_PATH, nullptr, nullptr);
        RunLODComparisonTest(vdsPath, maxSize);
        return 0;
    }

    // Check for benchmark mode
    if (benchmark)
    {
        char folderPath[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, filepath.c_str(), -1, folderPath, MAX_PATH, nullptr, nullptr);

        std::string outputFile = "benchmark_results.csv";
        char outPath[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, outputPath.c_str(), -1, outPath, MAX_PATH, nullptr, nullptr);
        if (outputPath != L"output.bmp")
            outputFile = outPath;

        printf("Scanning for VDS files in: %s\n", folderPath);
        std::vector<std::string> vdsFiles = FindVdsFiles(folderPath);

        if (vdsFiles.empty())
        {
            printf("No .vds files found.\n");
            return 1;
        }

        printf("Found %zu VDS files.\n", vdsFiles.size());

        TimePoint startBenchmark = Clock::now();

        for (const auto& file : vdsFiles)
        {
            BenchmarkVdsFile(file, verbose);
        }

        double totalBenchmarkTime = ElapsedMs(startBenchmark, Clock::now());

        PrintResultsSummary();
        printf("\nTotal benchmark time: %.2f seconds\n", totalBenchmarkTime / 1000.0);

        WriteResultsCsv(outputFile);

        return 0;
    }

    // Single file mode
    printf("Input: %ls\n", filepath.c_str());
    printf("Output: %ls\n", outputPath.c_str());

    char narrowPath[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, filepath.c_str(), -1, narrowPath, MAX_PATH, nullptr, nullptr);

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

    if (dimension < 0)
    {
        dimension = (dimensionality >= 3) ? 2 : 0;
    }

    if (sliceIndex < 0)
    {
        sliceIndex = layout->GetDimensionNumSamples(dimension) / 2;
    }

    HBITMAP hBitmap = RenderSlice(vdsHandle, layout, dimension, sliceIndex);

    if (!hBitmap)
    {
        printf("ERROR: Failed to render slice\n");
        OpenVDS::Close(vdsHandle, error);
        return 1;
    }

    if (SaveBitmapToFile(hBitmap, outputPath.c_str()))
    {
        printf("\nSuccess! Saved to: %ls\n", outputPath.c_str());
    }
    else
    {
        printf("\nERROR: Failed to save bitmap\n");
    }

    DeleteObject(hBitmap);
    OpenVDS::Close(vdsHandle, error);

    return 0;
}
