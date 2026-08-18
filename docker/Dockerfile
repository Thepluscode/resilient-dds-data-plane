# Build/run environment for the DDS transport layer.
# The host toolchain is not used: DDS discovery, RTPS and multi-process failure
# injection all need a Linux network namespace we can partition and kill.
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ca-certificates \
        default-jre-headless \
        fastddsgen \
        libfastrtps-dev \
        iproute2 \
        python3-minimal \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
