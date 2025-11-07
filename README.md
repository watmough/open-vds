## OpenVDS
Volume Data Store (VDS) is a storage format for fast random access to multi-dimensional (up to 6D) volumetric data. The VDS format has been developed by Bluware Inc. and has seen extensive industrial deployments over the last two decades. The format is contributed by Bluware Inc. to the Open Group Open Subsurface Data Universe Forum (OSDU) (The Open Group, u.d.).

An open-source reference software implementation which can read and write VDS has been contributed by Bluware Inc. to the OSDU. This implementation is named OpenVDS and supports several programming languages. It can be included in software products with no encumberments towards Bluware Inc.

VDS has been designed to handle extremely large volumes, up to petabytes in size, with variable sized compressed bricks. The VDS format is very flexible and can store any kind data representable as arrays with key/value-pair metadata. In particular, data commonly used in seismic processing can be stored along with all necessary metadata. This makes it possible to go from legacy formats to VDS and back, while retaining all metadata.

VDS files may be used to represent E&P data types such as regularized single-Z horizons/height-maps (2D), seismic lines (2D), pre-stack volumes (3D-5D), post-stack volumes (3D), geobody volumes (3D-5D), and attributes volumes of any dimensionality up to 6D.

The format has been designed primarily to support random access and on-demand fetching of data, this enables applications that are responsive and interactive as well as efficient I/O for high-performance computing or machine learning workloads.

The OpenVDS implementation is made up of the following components:
- VolumeDataAccess C++ API for direct access to volume data stored in a VDS
- Python bindings for the VolumeDataAccess API
- Java bindings for the VolumeDataAccess API
- SEGYImport tool (import a SEG-Y file to a VDS)
- SEGYExport tool (export a SEG-Y file from a VDS)
- VDSInfo tool (transfer from object storage)
- VDSCopy tool (copy to/from any VDS file or supported object store)

In order to implement these components there are a number of internal components:
- Decompression (Zip, Run-length encoding, Bluware Inc. proprietary Wavelet compression)
- VolumeDataLayout (manages how the volume is divided into chunks)
- IOManagers for connecting to the various cloud providers' object storage solutions
- File (UTF-8 filenames, thread-safe read/write, possibility to create memory-mapped file views)

Licensed under [**Apache 2.0**](https://gitlab.opengroup.org/osdu/open-vds/blob/master/LICENSE)

Latest build of the [**OpenVDS-Documentation**](https://osdu.pages.opengroup.org/platform/domain-data-mgmt-services/seismic/open-vds/)

Community submitted [**VDS use-cases**](https://community.opengroup.org/osdu/platform/domain-data-mgmt-services/seismic/open-vds/-/wikis/VDS-use-cases)

### Binary Release
Bluware Inc. releases a binary distribution of OpenVDS that includes the proprietary Bluware Wavelet Compression algorithm that makes it possible
to write compressed VDS. This distribution is known as [**OpenVDS+**](https://bluware.com/data-solutions/vds/openvds/) and is available for free under a Creative Commons [**CC-BY-ND 4.0**](https://creativecommons.org/licenses/by-nd/4.0/) License, the only
restrictions being that you are not allowed to modify, extend or extract parts of the provided binaries (i.e. the compression code). You can read more about this distribution in the Bluware [**OpenVDS+ FAQ**](https://bluware.com/developers/openvds-faq/).

### Linux Build Requirements (Ubuntu 22.04 / Centos 8)
Please ensure that the following build tools are available:
- Build Essentials, Git, CMake and Ninja are required for all configurations, the mold linker is optional
  - `sudo apt install build-essential` or `sudo dnf group install "Development Tools"`
  - `sudo apt install git` or `sudo dnf install git`
  - `sudo apt install ninja-build` or `sudo dnf install ninja-build`
  - `sudo apt install mold` or `sudo dnf install mold`

OpenVDS uses the following dependencies when building the OpenVDS Python API -- it is recommended to prepare a Python Virtual Environment:
- Python 3 Development (includes required header files) and pip
  - `sudo apt install python3-dev python3-pip` or `sudo dnf install python3-devel`
  - `sudo apt install python3-pip` or `sudo dnf install python3-pip`
- Create and activate python3 virtual environment
  - `sudo apt install python3-venv` or `sudo dnf install python3-virtualenv`
  - `python3 -m venv .venv`
  - `source .venv/bin/activate`
  - `python3 -m pip install --upgrade pip`
- Python requirements (numpy, pytest, pytest-benchmark)
  - `python3 -m pip install -r python/requirements-dev.txt`

Building the OpenVDS Documentation requires:
- Doxygen
  - `sudo apt install doxygen` or `sudo dnf install doxygen`
- Python requirements (sphinx, breathe, markdown, myst-parser, linkify-it-py, sphinx-rtd-theme, sphinx-design)
  - `python3 -m pip install -r python/requirements-dev-with-docs.txt`

Building the Java bindings requires OpenJDK Development:
  - `sudo apt install openjdk-8-jdk` or `sudo dnf install java-1.8.0-openjdk-devel`

### Building
OpenVDS uses the master branch as the main development branch. It should be in
a working state, but might contain experimental features, or features targeting
the next "major version". The stable branches are the branches with names such
as 1.x, 2.x. They are the branches from where the release tags (1.x.y, 2.x.y
etc) are made. Release tags are basis for OpenVDS releases found
[here](https://community.opengroup.org/osdu/platform/domain-data-mgmt-services/seismic/open-vds/-/releases).

The easiest way to build is to use CMake presets:
```bash
cmake -G Ninja --preset Release
ninja -C out/build/Release
sudo ninja -C out/build/Release install
```
This can also easily be done from within Visual Studio Code using the CMake extension.

NOTE: CMake will download 3rdparty dependencies the first time CMake is run on
the OpenVDS repository. This will take some time, so be patient. This will not
occur on subsequent builds.

By default OpenVDS builds the Python 3 bindings, to disable building the Python 3 bindings use the -DBUILD_PYTHON=OFF cmake argument.

To install the Python bindings as a site-package run:
`python3 -m pip install .`
This will use the python executable as the target python distribution.

When building using cmake the cmake variable Python3_ROOT_DIR can be used to specify a specific python installation.
Otherwise the cmake find_package python rules will be used. Since CMake version 3.13 Python_FIND_REGISTRY can be
used to modify search order on windows. For example, to disable searching the 
registry pass the cmake option: -DPython_FIND_REGISTRY=NEVER.

#### Using the mold linker
For Linux builds, we suggest to install the `mold` linker, which both dramatically reduces time
required for linking OpenVDS, and most importantly, reduces memory requirements
sufficently to avoid OOM errors when using `ninja` build parallelism defaults.
If no other changes are required, `mold` provides a simple wrapper that replaces the default linker:
Prefix your build command with `mold -run` to transparently replace `ld` with `mold` (e.g. `mold -run ninja -C out/build/Release`).

#### Example build with extra options on Ubuntu 22.04
The following is an example of building OpenVDS on Ubuntu 22.04, using a Python virtual
environment, using pybind11 installed in the virtual environment instead of the automaticall downloaded one, using local zlib instead of the automaticall downloaded one, building documentation, building the Python API, and installing OpenVDS
to `/opt/ovds`.

```bash
export CMAKE_EXPORT_COMPILE_COMMANDS=1
rm -rf build && \
cmake -G Ninja -B build \
  -DCMAKE_MODULE_PATH=../.venv/lib/python3.10/site-packages/pybind11/share/cmake/pybind11 \
  -DCMAKE_INSTALL_PREFIX=/opt/ovds \
  -DPython_EXECUTABLE=/home/builds/Development/open-vds/.venv/bin/python \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_ZLIB=OFF \
  -DBUILD_DOCS=ON \
  -DENABLE_OPENMP=OFF \
  -DBUILD_PYTHON=ON
mold -run ninja
sudo mold -run ninja install
```

#### Windows Visual Studio
When building on Windows the Desktop C++ component for Visual Studio must be
installed. Install Visual Studio CMake integration if OpenVDS should be
compiled with the "Open a local folder" option.

Much like on Linux its possible to generate a project in a build folder and
build it using native tools.  To generate a Visual Studio solution make a build
directory in the OpenVDS folder and change current directory to the build
folder. Either use `$ cmake ..` or launch cmake-gui and generate a Visual
Studio solution.

OpenVDS also supports using the cmake integration in Visual Studio. Open Visual
Studio and use the "Open a local folder" to open the OpenVDS folder.

This works with default settings with Visual Studio 2022 and 2019, but there are some
limitations using Visual Studio 2017 that will require changes to be made to the CMakeSettings.json file:
`$ git checkout 0d7825df9c981f624b6e1197a1b90c74ddae6aa9 -- CMakeSettings.json`
`$ cmake -P CMake/Fetch3rdParty.cmake`

#### MacOS
MacOS is not an officially supported target for OpenVDS, but we try to make it work as best as we can. We recommend using Visual Studio Code and the cmake extension.

#### Emscripten
OpenVDS can be compiled with Emscripten to a javascript module. The module
exports the DeserializeVolumeData() function.

To build the module, the Emscripten SDK from https://github.com/emscripten-core
is needed. Make sure cmake is in the path. Activate the Emscripten SDK
environment, make a build folder, and run
"emcmake cmake -DCMAKE_BUILD_TYPE=Release <path to open-vds>" and
"cmake --build ." from the build folder.

#### Build options
- BUILD_PYTHON (ON|OFF)
- BUILD_DOCS (ON|OFF) Default to OFF
- ENABLE_OPENMP (ON|OFF)
- BUILD_ZLIB (ON|OFF)
- CMAKE_INSTALL_PREFIX (PATH)
- CMAKE_BUILD_TYPE (Debug|Release|RelWithDebInfo|MinSizeRel)

Build options are arguments to cmake. `$ cmake -DBUILD_PYTHON=OFF ..` would turn off building python.

#### Building documentation
The following tools are needed to build the documentation:
- Doxygen
  - On Windows download the Doxygen binary from: http://doxygen.nl/download.html
  - On Linux use `sudo apt install doxygen` or `sudo dnf install doxygen` to install doxygen
- The python packages sphinx, breathe, markdown, myst-parser, linkify-it-py, sphinx-rtd-theme and sphinx-design
  - `python3 -m pip install -r python/requirements-dev-with-docs.txt`

Add `-DBUILD_DOCS=ON` to the cmake argument list

#### Building OSDU Docker image
The OSDU Docker image is built using `python3 build_osdu_image.py` in the root
of this repository. It uses a multistage Dockerfile to build OpenVDS and tools
from scratch, and then deploy them into a minimal size image on top of an
up-to-date Ubuntu 20.04 LTS base.

This single build will support all backends, which currently consists of
- Azure Blob Storage (azure, azureSAS)
- Google Cloud Storage (gs)
- AWS S3 (s3)
- OSDU/DELFI Seismic DMS (sd)

OpenVDS' URL scheme follows `protocol://resource/sub_path`, where the protocol
refers to the backend mentioned above. For `s3` and `gs` the resource will be
the bucket, while for `azure` or `azureSAS` the resource will be the container,
and for `sd` the resource will be the tenant name.

OpenVDS relies on the storage libraries from the various Cloud Service Providers,
and will support the environment variables these libraries themselves detect.

Azure Blob Storage [Azure Storage Client Library for C++ (7.5.0)]:
- None
- See https://github.com/Azure/azure-storage-cpp for more

Google Cloud Storage:
- `GOOGLE_CLOUD_PROJECT`
- `GOOGLE_CLOUD_CPP_ENABLE_CLOG`
- `GOOGLE_APPLICATION_CREDENTIALS`
- `GOOGLE_GCLOUD_ADC_PATH_OVERRIDE`
- `GOOGLE_RUNNING_ON_GCE_CHECK_OVERRIDE`
- `CLOUD_STORAGE_ENABLE_CLOG`
- `CLOUD_STORAGE_ENABLE_TRACING`
- `CLOUD_STORAGE_TESTBENCH_ENDPOINT`
- See https://googleapis.github.io/google-cloud-cpp/0.8.0/storage/client__options_8h_source.html for more

AWS S3:
- `AWS_ACCESS_KEY_ID`
- `AWS_CA_BUNDLE`
- `AWS_DEFAULT_OUTPUT`
- `AWS_CONFIG_FILE`
- `AWS_DEFAULT_REGION`
- `AWS_MAX_ATTEMPTS`
- `AWS_METADATA_SERVICE_NUM_ATTEMPTS`
- `AWS_METADATA_SERVICE_TIMEOUT`
- `AWS_PROFILE`
- `AWS_RETRY_MODE`
- `AWS_SECRET_ACCESS_KEY`
- `AWS_SESSION_TOKEN`
- `AWS_SHARED_CREDENTIALS_FILE`
- `AWS_STS_REGIONAL_ENDPOINTS`
- `CA_BUNDLE`
- See https://docs.aws.amazon.com/credref/latest/refdocs/environment-variables.html for more

OSDU/DELFI Seismic DMS:
- `SD_SVC_URL`
- `SD_SVC_API_KEY`
- Azure Managed Identity authentication is supported via the `msienable=true` connection string parameter for Azure-hosted deployments
