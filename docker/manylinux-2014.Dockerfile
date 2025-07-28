FROM quay.io/pypa/manylinux2014_x86_64

RUN curl -sSL  -o ninja.zip "https://github.com/ninja-build/ninja/releases/download/v1.10.2/ninja-linux.zip"
RUN unzip ninja.zip && mv ninja /usr/local/bin/ && rm -vf ninja* && ln -s /usr/local/bin/ninja /usr/local/bin/ninja-build

RUN yum install -y vim less gdb java-1.8.0-openjdk-devel ccache perl perl-IPC-Cmd

COPY python/requirements-dev-with-docs.txt /tmp/requirements-dev.txt

RUN /opt/python/cp38-cp38/bin/python -m pip install -r /tmp/requirements-dev.txt
RUN /opt/python/cp39-cp39/bin/python -m pip install -r /tmp/requirements-dev.txt
RUN /opt/python/cp310-cp310/bin/python -m pip install -r /tmp/requirements-dev.txt
RUN /opt/python/cp311-cp311/bin/python -m pip install -r /tmp/requirements-dev.txt
RUN /opt/python/cp312-cp312/bin/python -m pip install -r /tmp/requirements-dev.txt
RUN /opt/python/cp313-cp313/bin/python -m pip install -r /tmp/requirements-dev.txt

ENV CMAKE_BUILD_PARALLEL_LEVEL=2