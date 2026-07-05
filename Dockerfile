FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libboost-system-dev libfftw3-dev libfmt-dev \
    libmbedtls-dev libyaml-cpp-dev libuhd-dev uhd-host \
    libconfig++-dev \
    ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

COPY srsRAN_4G /build/srsRAN_4G
COPY src /build/src
COPY CMakeLists.txt /build/
COPY configs /build/configs

RUN mkdir -p /build/srsRAN_4G/build && \
    cd /build/srsRAN_4G/build && \
    cmake .. -DENABLE_GUI=OFF -DENABLE_ZMQ=ON && \
    make -j$(nproc) && \
    cd /build && \
    mkdir -p /build/build && \
    cd /build/build && \
    cmake .. -DSRSRAN_ROOT=/build/srsRAN_4G && \
    make -j$(nproc) ra-spoof

FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libboost-system1.74.0 libfftw3-single3 libfmt8 \
    libmbedcrypto7 libmbedtls14 libmbedx509-1 \
    libyaml-cpp0.7 libuhd4.1.0 uhd-host \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/build/ra-spoof /usr/local/bin/ra-spoof

COPY ra_spoof/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
