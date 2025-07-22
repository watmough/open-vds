# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Development Commands

### Build Commands
```bash
# Configure with CMake presets (recommended)
cmake --preset Release
cmake --preset Debug
cmake --preset DebugWithDocs

# Build with Ninja (recommended)
ninja -C out/build/Release
ninja -C out/build/Debug

# Alternative make-based build
make -j8
make install

# Key CMake options
cmake -DBUILD_PYTHON=ON ..        # Enable Python bindings (default ON)
cmake -DBUILD_JAVA=ON ..          # Enable Java bindings  
cmake -DBUILD_DOCS=ON ..          # Enable documentation build
cmake -DBUILD_TESTS=ON ..         # Enable C++ tests
cmake -DENABLE_OPENMP=ON ..       # Enable OpenMP parallelization
```

### Test Commands
```bash
# C++ tests with CTest
ctest --test-dir out/build/Release
ctest --test-dir out/build/Release --verbose
ctest --test-dir out/build/Release -R "OpenVDS"

# Python tests with pytest
python -m venv .venv && source .venv/bin/activate  # Setup environment
python -m pip install -r python/requirements-dev.txt
pytest tests/python/pytests/
pytest -v tests/python/pytests/test_create.py  # Specific test file
```

### Python Development Setup
```bash
# Create virtual environment and install dependencies
python3 -m venv .venv
source .venv/bin/activate  # Linux/macOS
.venv\Scripts\activate     # Windows
python -m pip install -r python/requirements-dev.txt

# Build Python package
python setup.py install        # Install package
python setup.py develop       # Development install
python setup.py bdist_wheel   # Build wheel
```

### Documentation Build
```bash
# Install documentation dependencies
python -m pip install -r python/requirements-dev-with-docs.txt
sudo apt install doxygen  # Or platform equivalent

# Build documentation
cmake -DBUILD_DOCS=ON ..
ninja -C out/build/Release docs
```

## Architecture Overview

OpenVDS is a Volume Data Store (VDS) implementation for fast random access to multi-dimensional volumetric data. The codebase uses a layered architecture with clear separation of concerns.

### Core Components

**VolumeDataAccess API** (`src/OpenVDS/OpenVDS/`):
- Main entry point through `OpenVDS.h`
- Provides `Open()`, `Create()`, `Close()` functions
- Handle-based API using `VDSHandle` for resource management
- Supports ReadOnly, ReadWrite, and Create access patterns

**VDS Format Implementation** (`src/OpenVDS/VDS/`):
- `VolumeDataLayoutImpl` - manages data organization and chunking
- `VolumeDataStoreVDSFile` - handles VDS file format specifics
- `VolumeDataPageAccessorImpl` - provides page-based data access with caching
- `VolumeDataRequestProcessor` - handles asynchronous request processing
- `MetadataManager` - manages key-value metadata storage

**IOManager Abstraction** (`src/OpenVDS/IO/`):
- Strategy pattern for different storage backends
- `IOManagerAWSCurl` - AWS S3 support
- `IOManagerAzure` - Azure Blob Storage support  
- `IOManagerGoogle` - Google Cloud Storage support
- `IOManagerHttp` - Generic HTTP backend
- `IOManagerInMemory` - In-memory storage for testing

**Compression System**:
- Multiple compression strategies: None, Wavelet (lossy/lossless), RLE, Zip
- Adaptive wavelet compression with tolerance-based quality control
- Implemented in `WaveletDecompress.cpp` and related files

### Language Bindings

**Python Bindings** (`python/openvds/`):
- Uses pybind11 for C++ to Python binding
- Each major class has PyXxx wrapper (e.g., `PyVolumeDataAccess.cpp`)
- Template-based code generation system
- Core module built as `core.cpp`

**Java Bindings** (`java/`):
- JNI-based bindings with hand-crafted wrappers
- Template-based code generation via `tools/javagen.py`
- Extensive type mapping system for primitives

### Data Flow Architecture

1. **Request Processing**: Client calls `RequestVolumeSubset()` or similar API
2. **Chunk Decomposition**: `VolumeDataRequestProcessor` breaks requests into chunks
3. **Page Management**: `VolumeDataPageAccessorImpl` handles chunk loading and LRU caching
4. **Storage I/O**: Appropriate IOManager performs actual storage operations
5. **Decompression**: Data decompressed using configured compression method
6. **Format Conversion**: Data converted to requested format if needed

### Key Design Patterns

- **Facade Pattern**: VolumeDataAccess API provides unified interface
- **Strategy Pattern**: IOManager hierarchy for storage backends and compression methods
- **Template Method**: Templated accessors for different dimensionalities and data types
- **Factory Pattern**: IOManager creation based on OpenOptions configuration
- **Producer-Consumer**: Asynchronous request processing with thread pool

## Development Guidelines

### Working with Storage Backends
- IOManager implementations should inherit from base `IOManager` class
- Implement `ReadObject()`, `WriteObject()`, and `ReadObjectInfo()` methods
- Handle backend-specific authentication and connection management
- Add corresponding OpenOptions subclass for configuration

### Working with Compression
- New compression methods should implement the compression interface
- Register compression types in `WaveletTypes.cpp`
- Ensure both compression and decompression paths are implemented
- Consider thread-safety for multi-threaded access

### Working with Language Bindings
- Python: Add new PyXxx wrapper class following existing patterns
- Java: Use template system to generate bindings via `javagen.py`
- Ensure proper memory management and reference counting
- Follow language-specific idioms while maintaining API consistency

### Testing Strategy
- C++ tests in `tests/` directory using CTest framework
- Python tests in `tests/python/pytests/` using pytest
- Integration tests for storage backends in `tests/io/`
- Benchmark tests available in `tests/benchmark/`

## Important File Locations

- **Main API Headers**: `src/OpenVDS/OpenVDS/`
- **Implementation Core**: `src/OpenVDS/VDS/`
- **Storage Backends**: `src/OpenVDS/IO/`
- **Python Bindings**: `python/openvds/`
- **Java Bindings**: `java/`
- **Tools**: `tools/` (SEGYImport, SEGYExport, VDSInfo, VDSCopy)
- **Examples**: `examples/`
- **Tests**: `tests/`
- **Documentation**: `docs/`

## Cloud Storage Environment Variables

The system supports multiple cloud storage backends with environment-based configuration:

- **AWS S3**: `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_DEFAULT_REGION`
- **Azure Blob**: Standard Azure SDK environment variables
- **Google Cloud**: `GOOGLE_APPLICATION_CREDENTIALS`, `GOOGLE_CLOUD_PROJECT`
- **OSDU/DELFI**: `SD_SVC_URL`, `SD_SVC_API_KEY`