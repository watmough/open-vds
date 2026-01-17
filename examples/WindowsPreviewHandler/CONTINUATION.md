# Preview Handler Fix - Continuation Notes

## Problem Summary

The Windows Preview Handler shows blank/zero data while thumbnails work correctly.

**Root Cause**: COM threading/marshaling deadlock

1. Preview handlers run in `prevhost.exe` (isolated process for security)
2. The shell marshals an IStream to the preview handler's thread
3. OpenVDS uses a thread pool (`VolumeDataRequestProcessor`) for data requests
4. When `WaitForCompletion()` blocks the main thread, worker threads try to call the IStream
5. IStream calls must marshal back to the main thread's COM apartment
6. Main thread is blocked → **DEADLOCK**

**Why thumbnails work**: They run in `explorer.exe` directly, no COM marshaling needed.

## Failed Approach: GIT Wrapper

We tried using COM's Global Interface Table (GIT) to make the IStream accessible from worker threads. Files created:
- `utils/GITStreamWrapper.h` - IStream wrapper using GIT

**Result**: Still deadlocks. GIT provides thread-local proxies, but those proxies still need to marshal calls back to the original apartment where the IStream lives.

## Planned Solution: Single-Threaded OpenVDS Path

Create a synchronous/single-threaded code path in OpenVDS that reads data on the calling thread instead of using the thread pool.

### Steps

1. **Remove GIT wrapper code** from VdsRenderer
   - Revert `VdsRenderer.h` - remove `m_wrappedStream`, remove GIT include
   - Revert `VdsRenderer.cpp` - remove GIT wrapper creation in `Initialize()`
   - Can delete `utils/GITStreamWrapper.h` or keep for reference

2. **Create test program** to verify single-threaded OpenVDS reads
   - New file: `examples/WindowsPreviewHandler/SingleThreadTest.cpp`
   - Opens a VDS file via IStream
   - Requests a slice WITHOUT using thread pool
   - Verifies data is non-zero

3. **Modify OpenVDS** to support synchronous reads
   - Key file: `src/OpenVDS/VDS/VolumeDataRequestProcessor.cpp`
   - Currently uses: `m_threadPool.Enqueue([job, pageAccessor, processor] { ... });`
   - Need option to execute job synchronously on calling thread
   - Possible approaches:
     - Add `RequestVolumeSubsetSync()` API
     - Add flag to `IStreamOpenOptions` to disable threading
     - Add global/per-handle config for synchronous mode

4. **Update VdsRenderer** to use synchronous API
   - Pass appropriate options when opening VDS
   - May need to update `RenderSlice()` to use sync API

### Key Files to Examine

**OpenVDS threading**:
- `src/OpenVDS/VDS/VolumeDataRequestProcessor.cpp` - where thread pool is used
- `src/OpenVDS/VDS/VolumeDataAccessManagerImpl.cpp` - request handling
- `src/OpenVDS/OpenVDS/OpenVDS.h` - public API

**Preview Handler**:
- `examples/WindowsPreviewHandler/VdsRenderer.cpp` - uses `RequestVolumeSubset`
- `examples/WindowsPreviewHandler/VdsShellExtension.cpp` - COM handlers

### Log Files (for debugging)

Located in `%USERPROFILE%\AppData\LocalLow\Temp\`:
- `openvds-thumbnails.log` - thumbnail handler logs
- `openvds-previewpane.log` - preview handler logs
- `openvds-git.log` - GIT wrapper debug logs (from failed approach)

### Current State of Code

- `VdsRenderer.cpp` has GIT wrapper code in `Initialize()`
- `VdsRenderer.h` has `m_wrappedStream` member and GIT include
- `utils/GITStreamWrapper.h` exists with debug logging
- Build works: `ninja -C build VdsShellExtension`

### Test VDS File

The test file is a 368x368x368 3D VDS (~12MB). Path TBD - user has local test files.

## Questions to Resolve

1. Where exactly in OpenVDS should synchronous mode be implemented?
2. Should it be a compile-time option, runtime flag, or API variant?
3. Performance implications for large files (acceptable for preview use case)
4. Should we support partial sync (main thread reads, background decompression)?

## References

- [Preview Handler Threading](https://learn.microsoft.com/en-us/windows/win32/shell/preview-handlers)
- OpenVDS `VolumeDataRequestProcessor` uses `ThreadPool::Enqueue()`
- COM apartment threading causes the marshaling requirement
