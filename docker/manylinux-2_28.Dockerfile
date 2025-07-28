FROM quay.io/pypa/manylinux_2_28_x86_64

RUN dnf -y upgrade
RUN dnf -y install dnf-plugins-core epel-release
RUN dnf -y config-manager --set-enabled powertools && dnf -y update

RUN dnf -y groupinstall "Development Tools"
RUN dnf -y install cmake ninja-build perl-IPC-Cmd ccache

RUN dnf -y install doxygen java-1.8.0-openjdk-devel libxml2-devel zlib-devel openssl-devel libcurl-devel libuv libuuid-devel

COPY python/requirements-dev-with-docs.txt /tmp/requirements-dev.txt

RUN /opt/python/cp38-cp38/bin/python -m pip install -r /tmp/requirements-dev.txt
RUN /opt/python/cp39-cp39/bin/python -m pip install -r /tmp/requirements-dev.txt
RUN /opt/python/cp310-cp310/bin/python -m pip install -r /tmp/requirements-dev.txt
RUN /opt/python/cp311-cp311/bin/python -m pip install -r /tmp/requirements-dev.txt
RUN /opt/python/cp312-cp312/bin/python -m pip install -r /tmp/requirements-dev.txt
RUN /opt/python/cp313-cp313/bin/python -m pip install -r /tmp/requirements-dev.txt

ENV CMAKE_BUILD_PARALLEL_LEVEL=2