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

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>

#include <OpenVDS/OpenVDS.h>
#include <OpenVDS/VolumeDataLayout.h>
#include <OpenVDS/VolumeDataAccess.h>

// Helper class for rendering VDS slices to bitmaps
class VdsRenderer
{
public:
    VdsRenderer();
    ~VdsRenderer();

    // Initialize from IStream
    bool Initialize(IStream* pStream);

    // Get metadata as formatted text lines
    std::vector<std::wstring> GetMetadataLines() const;

    // Render a slice to an HBITMAP
    // dimension: 0=inline, 1=crossline, 2=timeslice/depth
    // sliceIndex: which slice to render
    // maxSize: maximum dimension for output bitmap
    HBITMAP RenderSlice(int dimension, int sliceIndex, int maxSize);

    // Get the number of slices for a dimension
    int GetSliceCount(int dimension) const;

    // Get recommended default slice index (middle)
    int GetDefaultSliceIndex(int dimension) const;

    // Get dimensions
    int GetDimensionality() const;
    const wchar_t* GetDimensionName(int dimension) const;

    // Set the log file path (for debugging)
    void SetLogFile(const char* logFilePath);

    // Get VDS name from metadata
    std::wstring GetVdsName() const;

    // Get last render debug messages (for on-screen display)
    const std::vector<std::wstring>& GetLastRenderDebugMessages() const { return m_lastRenderDebugMessages; }

    // Check if progressive LOD refinement is still pending (caller should trigger another render)
    bool IsRefinementPending() const { return m_isRefining && m_currentLOD > 0; }

    // Wait for any in-progress render to complete before destruction
    // Call this before destroying the renderer to avoid orphaned requests
    void WaitForPendingRender() { std::lock_guard<std::mutex> lock(m_renderMutex); }

private:
    std::vector<std::wstring> m_lastRenderDebugMessages;
    std::string m_logFilePath;
    OpenVDS::VDSHandle m_vdsHandle;
    OpenVDS::VolumeDataLayout* m_layout;
    IStream* m_stream;          // Stream reference
    mutable std::mutex m_renderMutex;  // Prevent concurrent render requests

    // Progressive LOD state - start with fastest LOD, refine if renders are quick
    int m_currentLOD = -1;           // Current LOD being displayed (-1 = not set)
    int m_targetLOD = 0;             // Target LOD based on display size
    int m_lastSliceIndex = -1;       // Last rendered slice index
    int m_lastDimension = -1;        // Last rendered dimension
    double m_lastRenderTimeMs = 0;   // Timing of last render
    bool m_isRefining = true;        // Still in progressive refinement mode
    static constexpr double FAST_RENDER_THRESHOLD_MS = 50.0;  // Threshold for "fast" render

    // Internal render at specific LOD
    HBITMAP RenderSliceAtLOD(int dimension, int sliceIndex, int maxSize, int lod,
                             double& outRenderTimeMs);

    // Convert float data to 8-bit grayscale with value mapping
    void NormalizeToGrayscale(const float* source, uint8_t* dest,
                             int count, float minVal, float maxVal);

    // Create a colorized bitmap from grayscale data (blue-white-red colormap)
    HBITMAP CreateColorizedBitmap(const uint8_t* grayscaleData,
                                  int width, int height);
};

static void LogToFile(const std::vector<std::wstring>& lines, const std::string& logFilePath);
