# VDS Windows Shell Extension

This project provides Windows Explorer integration for OpenVDS (.vds) files through preview and thumbnail handlers.

## Features

### Preview Handler
- **Full preview** in Windows Explorer's preview pane (Alt+P)
- **Two view modes**:
  - **Slice View**: Visual rendering of VDS slices with blue-white-red colormap
  - **Metadata View**: Scrollable text showing dimensions, channels, compression info
- **Interactive navigation**:
  - Arrow keys to navigate slices or scroll metadata
  - Tab/V to switch between view modes
  - Left/Right arrows to cycle through dimensions (3D data)
  - Mouse wheel support
- **Adaptive rendering**: Preserves aspect ratio and scales to fit preview pane

### Thumbnail Handler
- **Automatic thumbnails** in Windows Explorer (icons, tiles, content view)
- Renders middle slice of the VDS file
- Scales appropriately for different thumbnail sizes

## Building

### Prerequisites
- Windows 10 or later
- Visual Studio 2019 or later
- CMake 3.16 or later
- OpenVDS library built

### Build Steps

1. Configure and build the OpenVDS project:
   ```cmd
   cmake --preset Release
   cmake --build build --target VdsShellExtension --config Release
   ```

2. The DLL will be built to:
   ```
   build/examples/WindowsPreviewHandler/Release/VdsShellExtension.dll
   ```

## Installation

### Step 1: Copy DLL to Permanent Location

Copy `VdsShellExtension.dll` and `openvds.dll` to a permanent location, for example:
```cmd
C:\Program Files\OpenVDS\VdsShellExtension.dll
C:\Program Files\OpenVDS\openvds.dll
```

**Important**: Do not register the DLL from the build directory, as it may be deleted during rebuilds.

### Step 2: Edit Registration Script

1. Open `register.reg` in a text editor
2. Replace all instances of `C:\\Path\\To\\VdsShellExtension.dll` with your actual DLL path
3. Use double backslashes (`\\`) in the path, e.g., `C:\\Program Files\\OpenVDS\\VdsShellExtension.dll`

### Step 3: Register the Handlers

1. **Right-click** on the edited `register.reg` file
2. Select **"Merge"** or **"Open with Registry Editor"**
3. Click **"Yes"** when prompted to confirm

### Step 4: Restart Windows Explorer

After registration, restart Windows Explorer to load the handlers:

```cmd
taskkill /f /im explorer.exe && start explorer.exe
```

Or simply log off and log back on.

## Usage

### Preview Handler

1. Open Windows Explorer
2. Enable the preview pane: Press **Alt+P** or View menu → Preview pane
3. Select a `.vds` file
4. The preview will appear in the right pane

**Keyboard shortcuts** (after clicking in preview):
- **V** or **Tab** - Toggle between Slice and Metadata views
- **↑/↓** - Navigate slices or scroll metadata
- **←/→** - Cycle through dimensions (inline/crossline/time for 3D data)
- **PgUp/PgDn** - Fast navigation/scrolling
- **Mouse wheel** - Navigate or scroll

### Thumbnail Handler

Thumbnails appear automatically in:
- Icon views (Small, Medium, Large, Extra Large icons)
- Tiles view
- Content view

## Troubleshooting

### Thumbnails Not Appearing

1. **Clear thumbnail cache**:
   ```cmd
   del /f /s /q /a %LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db
   taskkill /f /im explorer.exe && start explorer.exe
   ```

2. **Verify registration**:
   - Open Registry Editor (regedit.exe)
   - Navigate to `HKEY_CLASSES_ROOT\.vds\ShellEx`
   - Verify both handler GUIDs are present

3. **Check DLL path**:
   - Ensure the DLL path in the registry is correct and accessible
   - Ensure `openvds.dll` is in the same directory or in PATH

### Preview Not Working

1. **Test in isolation**:
   - Create a test folder with a single .vds file
   - Restart Explorer
   - Try previewing the file

2. **Check preview host**:
   - Open Task Manager while previewing
   - Look for `prevhost.exe` process
   - If it's crashing, check Windows Event Viewer for details

3. **Debug mode** (for development):
   - Add this registry key temporarily:
     ```reg
     [HKEY_CLASSES_ROOT\CLSID\{9A2E5F61-7D3B-4C2F-A8E6-1C4B8E9D5F72}]
     "DisableLowILProcessIsolation"=dword:00000001
     ```
   - This runs the preview handler in-process for easier debugging
   - **Remove this key** after debugging (security risk)

4. **Attach debugger**:
   - Build Debug configuration
   - Set breakpoints in VdsShellExtension
   - Attach Visual Studio debugger to `prevhost.exe` or `explorer.exe`
   - Trigger preview

### "Failed to open VDS" Error

1. **Check IStream support**: Ensure OpenVDS was built with IStream support
2. **Test with IStreamExample**: Run the test program to verify VDS files can be opened via IStream
3. **Verify file integrity**: Try opening the .vds file with VDSInfo tool

## Uninstallation

1. **Merge the unregister script**:
   ```cmd
   regedit /s unregister.reg
   ```

2. **Restart Explorer/Kill PrevHost.exe**:
   ```cmd
   taskkill /f /im explorer.exe && start explorer.exe && taskkill /f /im prevhost.exe
   ```

3. **Delete the DLL** (optional):
   ```cmd
   del "C:\Program Files\OpenVDS\VdsShellExtension.dll"
   ```

## Technical Details

### Architecture

```
VdsShellExtension.dll
├── VdsThumbnailProvider (IThumbnailProvider, IInitializeWithStream)
├── VdsPreviewHandler (IPreviewHandler, IInitializeWithStream, IOleWindow, IObjectWithSite)
└── VdsRenderer (Helper class for rendering VDS slices to bitmaps)
```

### GUIDs

- **Thumbnail Provider**: `{8F7B3D50-5C4E-4A1F-9E5D-6B2A7F8C4D91}`
- **Preview Handler**: `{9A2E5F61-7D3B-4C2F-A8E6-1C4B8E9D5F72}`

### Threading Model

- **Apartment** threading (COM STA)
- Preview handler runs in isolated low-integrity `prevhost.exe` process
- Thumbnail provider runs in Explorer's process

### Colormap

The slice view uses a **blue-white-red** colormap commonly used in seismic visualization:
- **Blue** - Low values (negative amplitudes)
- **White** - Mid values (near zero)
- **Red** - High values (positive amplitudes)

## Development

### File Structure

- **VdsShellExtension.cpp** - COM handlers implementation
- **VdsRenderer.h/cpp** - VDS rendering helper
- **VdsShellExtension.def** - DLL exports
- **CMakeLists.txt** - Build configuration
- **register.reg** - Registration script
- **unregister.reg** - Unregistration script

### Adding Features

1. **New view modes**: Add to `ViewMode` enum in VdsPreviewHandler
2. **Different colormaps**: Modify `CreateColorizedBitmap()` in VdsRenderer
3. **Dimension selection UI**: Extend `HandleKeyDown()` to add dimension picker
4. **Multi-channel support**: Extend VdsRenderer to support channel selection

### Performance Optimization

The implementation includes several optimizations:
- **Bitmap caching**: Current slice is rendered once and cached
- **Lazy initialization**: VDS file is only opened when needed
- **Aspect-aware scaling**: Prevents unnecessary resampling

For large VDS files, consider:
- **LOD support**: Request lower resolution for preview
- **Progressive rendering**: Show low-res preview first, refine later
- **Background loading**: Use worker threads for slice extraction

## References

- [Windows Shell Preview Handlers](https://learn.microsoft.com/en-us/windows/win32/shell/preview-handlers)
- [IThumbnailProvider Interface](https://learn.microsoft.com/en-us/windows/win32/api/thumbcache/nn-thumbcache-ithumbnailprovider)
- [IPreviewHandler Interface](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ipreviewhandler)

## License

Copyright 2019 The Open Group, Bluware, Inc.

Licensed under the Apache License, Version 2.0. See LICENSE file for details.
