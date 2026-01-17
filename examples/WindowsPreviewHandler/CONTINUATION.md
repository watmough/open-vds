# Preview Handler Fix - Continuation Notes

## Current Status: READY FOR TESTING

The single-threaded OpenVDS implementation is complete. A mutex was added to prevent concurrent render requests that caused crashes during rapid scrolling. The DLL has been built and is ready for deployment.

**Next Step**: Run `retry.bat` to deploy and test the shell extension.

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
