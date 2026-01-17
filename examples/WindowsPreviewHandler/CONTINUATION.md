# Preview Handler Fix - Continuation Notes

## Current Status: WORKING - Slice scrolling improved

The preview handler is now fully functional:
- Single-threaded OpenVDS mode avoids COM marshaling deadlock
- File switching works (no more hangs)
- Slice scrolling is smooth with minimal flicker
- Re-entrancy guard prevents corruption from COM message pumping

**Known Issue**: Very large VDS files (2GB+) still have slow initial load times (~44 seconds for first slice). This is expected due to single-threaded decompression. Subsequent slices are fast (~10ms) due to OpenVDS caching.

---

## Fix #3: Flicker Reduction (2026-01-17)

Reduced visual flicker when scrolling through slices by eliminating background clearing and implementing atomic bitmap swaps.

### Problem

When scrolling slices, the preview would flash gray:
1. Mouse wheel → delete cached bitmap → InvalidateRect
2. OnPaint fills entire right panel with gray → **FLASH**
3. New bitmap rendered → drawn on screen

### Solution

**1. Removed full panel clear** (`VdsShellExtension.cpp`):
```cpp
// BEFORE: Cleared entire right panel
RECT rightRect = { midX + 2, rc.top, rc.right, rc.bottom };
HBRUSH hbrLight = CreateSolidBrush(RGB(245, 245, 245));
FillRect(hdc, &rightRect, hbrLight);  // <-- FLASH!

// AFTER: Only define bounds, fill targeted areas
RECT rightRect = { midX + 2, rc.top, rc.right, rc.bottom };
// Info panel and status bar get their own fills, bitmap area untouched
```

**2. Added slice index tracking**:
```cpp
// New member variable
int m_cachedSliceIndex = -1;  // Track which slice the cached bitmap represents
```

**3. Mouse wheel no longer deletes bitmap**:
```cpp
// BEFORE:
if (newSlice != m_sliceIndex)
{
    m_sliceIndex = newSlice;
    DeleteObject(m_cachedBitmap);  // <-- Causes blank frame
    m_cachedBitmap = nullptr;
}

// AFTER:
m_sliceIndex = newSlice;  // Just update index, let OnPaint handle it
```

**4. Atomic bitmap swap in UpdateCachedBitmap()**:
```cpp
// Check if re-render needed
if (m_cachedBitmap && m_cachedSliceIndex == m_sliceIndex)
    return;  // Already have the right slice

// Render new bitmap first
HBITMAP newBitmap = m_renderer->RenderSlice(...);

if (newBitmap)
{
    // Only delete old AFTER new is ready (no blank frame)
    if (m_cachedBitmap)
        DeleteObject(m_cachedBitmap);
    m_cachedBitmap = newBitmap;
    m_cachedSliceIndex = m_sliceIndex;
}
else
{
    // Keep old bitmap visible if render fails
}
```

### Result

- Old slice stays visible while new one renders
- No flash to gray background
- Smooth scrolling experience for cached data (~10ms per slice)

---

## Previous Issue: VDS SWITCH HANG (RESOLVED)

### Root Cause Analysis

When switching VDS files, Windows Explorer calls `VdsPreviewHandler::Initialize(newStream)`:

```cpp
// VdsShellExtension.cpp line 368
m_renderer = std::make_unique<VdsRenderer>();  // <-- DESTROYS old renderer!
```

This destroys the OLD renderer while it may have a render in progress:
1. Old renderer's `RenderSlice()` is blocked on `request->WaitForCompletion()`
2. `unique_ptr` assignment calls old renderer's destructor
3. Destructor calls `OpenVDS::Close(m_vdsHandle)` on the VDS handle still in use
4. Destructor releases `m_stream` that OpenVDS IO is actively reading
5. The pending render request becomes orphaned/invalid → hang

The per-instance `m_renderMutex` can't protect against this because it's the OLD instance being destroyed while its render runs.

### Proposed Fix

**Option A: Wait for pending render before replacing renderer**
```cpp
// In VdsPreviewHandler::Initialize()
if (m_renderer)
{
    // Acquire the old renderer's mutex before destruction
    // This blocks until any in-progress render completes
    std::lock_guard<std::mutex> lock(m_renderer->GetRenderMutex());
}
m_renderer = std::make_unique<VdsRenderer>();
```

**Option B: Add cancellation mechanism**
- Add `VdsRenderer::Cancel()` method that sets a flag
- Check flag in RenderSlice before WaitForCompletion
- Destructor calls Cancel() and waits

**Option C: Mutex in Initialize() method**
- Have `VdsRenderer::Initialize()` also acquire `m_renderMutex`
- This prevents reinitialization while a render is active
- But this is per-instance, need to wait on OLD instance first

**Recommended: Option A** - simplest and most reliable.

### Fix #1: WaitForPendingRender (Partial Fix)

Added `WaitForPendingRender()` method to VdsRenderer and call it before destroying the renderer.
This prevents destroying a renderer while a render is in progress, but doesn't fix the root cause.

### Fix #2: Re-entrancy Guard (Current Fix)

**Root Cause Discovered:** When OpenVDS calls `IStream::Read()`, COM may pump Windows messages.
This causes `WM_PAINT` to re-enter while a render is in progress, corrupting the IStream read position.

Log evidence:
```
[09:43:28.444] RenderSlice... | Requesting 368x368 slice... |
[09:43:28.447] Render: slice=184, maxSize=1205   <-- Re-entrant call 3ms later!
```

**VdsShellExtension.cpp** - Added re-entrancy guard:
```cpp
// Member variable:
bool m_isRendering = false;

// In UpdateCachedBitmap():
if (m_isRendering)
{
    LogPreview("Skipping re-entrant render");
    return;
}

m_isRendering = true;
struct RenderGuard {
    bool& flag;
    ~RenderGuard() { flag = false; }
} guard{m_isRendering};

m_cachedBitmap = m_renderer->RenderSlice(...);
```

### Next Steps

1. Rebuild: `cmake --build build --target VdsShellExtension`
2. Deploy with `retry.bat`
3. Test switching between multiple VDS files
4. Look for "Skipping re-entrant render" in logs to confirm the fix is working

---

## Problem Summary

The Windows Preview Handler had a COM threading/marshaling deadlock:

1. Preview handlers run in `prevhost.exe` (isolated process for security)
2. The shell marshals an IStream to the preview handler's thread
3. OpenVDS uses a thread pool (`VolumeDataRequestProcessor`) for data requests
4. When `WaitForCompletion()` blocks the main thread, worker threads try to call the IStream
5. IStream calls must marshal back to the main thread's COM apartment
6. Main thread is blocked → **DEADLOCK**

**Why thumbnails work**: They run in `explorer.exe` directly, no COM marshaling needed.

## Solution Implemented: OPENVDS_SINGLE_THREADED Compile-Time Option

Created a compile-time option that makes OpenVDS execute all data requests synchronously on the calling thread, avoiding the COM marshaling deadlock.

### Changes Made

#### 1. ThreadPool.h (`common/ThreadPool/ThreadPool.h`)
Added synchronous stub when `OPENVDS_SINGLE_THREADED` is defined:
```cpp
#ifdef OPENVDS_SINGLE_THREADED
class ThreadPool
{
public:
  ThreadPool(size_t) {}
  ~ThreadPool() {}
  template <class F>
  auto Enqueue(F&& f) -> std::future<typename std::result_of<F()>::type>
  {
    using return_type = typename std::result_of<F()>::type;
    auto task = std::make_shared<std::packaged_task<return_type()>>(std::forward<F>(f));
    std::future<return_type> res = task->get_future();
    (*task)();  // Execute immediately on calling thread (synchronous)
    return res;
  }
  size_t ThreadCount() const { return 1; }
  static int ConfigureThreadCount(const char*, int = 1) { return 1; }
};
#endif
```

#### 2. VolumeDataRequestProcessor.cpp (`src/OpenVDS/VDS/VolumeDataRequestProcessor.cpp`)
Critical fix: Release mutex before Enqueue in single-threaded mode to prevent deadlock:
```cpp
if (singleThread)
{
#ifdef OPENVDS_SINGLE_THREADED
  int64_t jobId = job->jobId;
  lock.unlock();  // Release to avoid deadlock in ProcessPageInJob
#endif
  job->future.push_back(m_threadPool.Enqueue([job, pageAccessor, processor]
  { ... }));
#ifdef OPENVDS_SINGLE_THREADED
  return jobId;
#else
  return job->jobId;
#endif
}
```

#### 3. CMakeLists.txt (root)
Added option:
```cmake
option(OPENVDS_SINGLE_THREADED "Disable threading for COM/IStream compatibility" OFF)
```

#### 4. src/OpenVDS/CMakeLists.txt
Added compile definitions for both targets:
```cmake
if (OPENVDS_SINGLE_THREADED)
  target_compile_definitions(openvds_objects PUBLIC OPENVDS_SINGLE_THREADED)
endif()
# ... and later:
if (OPENVDS_SINGLE_THREADED)
  target_compile_definitions(openvds PUBLIC OPENVDS_SINGLE_THREADED)
endif()
```

#### 5. examples/WindowsPreviewHandler/CMakeLists.txt
Added definition for shell extension:
```cmake
if (OPENVDS_SINGLE_THREADED)
  target_compile_definitions(VdsShellExtension PRIVATE OPENVDS_SINGLE_THREADED)
endif()
```

#### 6. VdsRenderer.h
- Removed GIT wrapper code
- Added mutex for concurrent render protection:
```cpp
#include <mutex>
// ... in private section:
mutable std::mutex m_renderMutex;  // Prevent concurrent render requests
```

#### 7. VdsRenderer.cpp
- Removed GIT wrapper code from `Initialize()`
- Added mutex lock at start of `RenderSlice()`:
```cpp
HBITMAP VdsRenderer::RenderSlice(int sliceOnDimension, int sliceIndex, int maxSize)
{
    std::lock_guard<std::mutex> lock(m_renderMutex);
    // ... rest of function
}
```

### Build Commands

```bash
# Configure with single-threaded option
cmake -G Ninja -B build -DOPENVDS_SINGLE_THREADED=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Build the shell extension
cmake --build build --target VdsShellExtension

# Or build everything
cmake --build build
```

### Deploy and Test

```bash
# Use retry.bat to deploy (stops explorer, copies DLL, restarts)
retry.bat

# Or manually:
taskkill /f /im explorer.exe
copy build\examples\WindowsPreviewHandler\VdsShellExtension.dll C:\path\to\registered\location
start explorer.exe
```

### Test Harness

A standalone test harness exists for debugging without Explorer:
```bash
cmake --build build && .\build\examples\WindowsPreviewHandler\VdsRenderTest.exe --benchmark c:\path\to\test.vds
```

## Issues Fixed During Development

1. **MULTI_THREADED mode still showing after enabling flag**
   - Cause: CMake definition only on `openvds_objects`, not propagating
   - Fix: Added PUBLIC definition to `openvds` target and PRIVATE to `VdsShellExtension`

2. **Hang at "Requesting..." with no debug output**
   - Cause: Debug fprintf not flushing on Windows
   - Fix: Added `fflush(stderr)` after each `fprintf`

3. **Deadlock in AddJob at lock.lock()**
   - Cause: Synchronous Enqueue executed while holding m_mutex, ProcessPageInJob tried to acquire locks
   - Fix: Release lock before Enqueue when `OPENVDS_SINGLE_THREADED` defined

4. **Preview handler crash after rapid scrolling**
   - Cause: Re-entrancy - second render request arrived before first completed (18ms apart in logs)
   - Fix: Added `std::mutex m_renderMutex` with `lock_guard` in `RenderSlice()`

## Log Files

Located in `%USERPROFILE%\AppData\Local\Temp\`:
- `openvds-thumbnails.log` - thumbnail handler logs
- `openvds-previewpane.log` - preview handler logs

Also copied to `examples/WindowsPreviewHandler/logs/` for analysis.

## Test VDS File

The test file is a 368x368x368 3D VDS. User has local test files at `c:\Shared\`.

## Key Files Reference

**OpenVDS threading**:
- `common/ThreadPool/ThreadPool.h` - thread pool with single-threaded stub
- `src/OpenVDS/VDS/VolumeDataRequestProcessor.cpp` - where thread pool is used

**Preview Handler**:
- `examples/WindowsPreviewHandler/VdsRenderer.cpp` - rendering with mutex
- `examples/WindowsPreviewHandler/VdsRenderer.h` - renderer class with mutex member
- `examples/WindowsPreviewHandler/VdsShellExtension.cpp` - COM handlers
- `examples/WindowsPreviewHandler/VdsRenderTest.cpp` - standalone test harness
