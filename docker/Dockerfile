# Reproducible DDS build/run environment. Fast DDS 2.14.6 is deliberately built
# from source with SECURITY=ON; Ubuntu's libfastrtps-dev is 2.11.x and EOL.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        binutils \
        ca-certificates \
        cmake \
        git \
        iproute2 \
        libasio-dev \
        libssl-dev \
        libtinyxml2-dev \
        openjdk-17-jdk-headless \
        openssl \
        pkg-config \
        python3-minimal \
        tcpdump \
    && rm -rf /var/lib/apt/lists/*

COPY scripts/install_fastdds.sh /tmp/install_fastdds.sh
RUN /tmp/install_fastdds.sh /opt/fastdds && rm /tmp/install_fastdds.sh

ENV PATH=/opt/fastdds/bin:${PATH}
ENV CMAKE_PREFIX_PATH=/opt/fastdds
ENV LD_LIBRARY_PATH=/opt/fastdds/lib

WORKDIR /work
