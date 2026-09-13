# syntax=docker/dockerfile:1
#
# Image for the standalone C++ receiver (rx/).
#
# It needs only libiio + liquid-dsp at runtime. config.yml / stations.yml
# are parsed and validated by the binary itself (see
# rx/src/resolve.hpp).
#
# On liquid-dsp: built from source at tag v1.5.0, NOT Debian's libliquid1.
#   - Pinned to 1.5.0: firpfbchr multichannel regressed hard after 1.5, and
#     the polyphase channelizer is the single-thread real-time ceiling. A
#     live A/B on prod put liquid 1.8.2 at rt_percent ~150 vs ~45 for 1.5.0
#     (both AVX2) -- ~3x, a raw-throughput cliff in the filterbank, not a
#     precision regression.
#   - From source, not the distro package: Debian's libliquid1 is the
#     portable/scalar build (hot symbols are dotprod_*_run4). liquid's
#     configure (AX_EXT) auto-selects the AVX2 dotprod kernels with -mavx2
#     when the build host has AVX2 -- which for the deployed i7-14700 is
#     ~1.7x on the channelizer thread and ~40% off per-station cost, since
#     the complex FIR dot products (pre-demod / RDS / stereo BPFs) are
#     ~3-8x faster vectorised.
#   - The build asserts AVX2 made it into the .so (grep %ymm) so a scalar
#     build can't ship silently from a non-AVX2 builder.
#   - Still links FFTW (channelizer stays FFT-bound).
#
# Build context is the repo root:
#   docker build -f rx/Dockerfile -t feedmyfm-rx:local .

ARG DEBIAN_BASE=debian:bookworm-slim

# ---------------------------------------------------------------------------
# Stage 1 -- build feedmyfm-rx.
# ---------------------------------------------------------------------------
FROM ${DEBIAN_BASE} AS rx-builder
ENV DEBIAN_FRONTEND=noninteractive
# ISA for the C++ (not liquid, which is the distro build). x86-64-v3 ==
# AVX2+FMA, ~2015+; override with --build-arg MARCH=native for a
# host-pinned image.
ARG MARCH=x86-64-v3

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config git ca-certificates \
        automake autoconf libtool \
        libiio-dev libfftw3-dev \
    && rm -rf /var/lib/apt/lists/*

# liquid-dsp v1.5.0 from source, with the AVX2 dotprod kernels (see the
# header comment). Installs to /usr/local; the AVX2 assertion fails the
# build if configure fell back to portable C.
ARG LIQUID_VERSION=v1.5.0
RUN git clone --depth 1 --branch "${LIQUID_VERSION}" \
        https://github.com/jgaeddert/liquid-dsp.git /tmp/liquid \
    && cd /tmp/liquid \
    && ./bootstrap.sh \
    && ./configure --prefix=/usr/local \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig \
    && objdump -d /usr/local/lib/libliquid.so | grep -q '%ymm' \
       || (echo 'FATAL: libliquid built without AVX2 (no %ymm in dotprod)'; exit 1) \
    && rm -rf /tmp/liquid

WORKDIR /build
COPY rx ./src
RUN cmake -S ./src -B ./out -DCMAKE_BUILD_TYPE=Release -DFMRX_MARCH=${MARCH} \
        -DLIQUID_ROOT=/usr/local \
    && cmake --build ./out -j"$(nproc)"

# ---------------------------------------------------------------------------
# Stage 2 -- runtime.
# ---------------------------------------------------------------------------
FROM ${DEBIAN_BASE}
ENV DEBIAN_FRONTEND=noninteractive

# Trim docs / man / non-English locales -- headless service.
RUN mkdir -p /etc/dpkg/dpkg.cfg.d && { \
        echo "path-exclude /usr/share/doc/*"; \
        echo "path-exclude /usr/share/man/*"; \
        echo "path-exclude /usr/share/locale/*"; \
        echo "path-include /usr/share/locale/en*"; \
    } > /etc/dpkg/dpkg.cfg.d/excludes

# liquid-dsp comes from the builder stage (COPY below), not apt -- so no
# libliquid1 here. libfftw3-single3 is liquid's runtime FFT dependency.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libiio0 libfftw3-single3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=rx-builder /usr/local/lib/libliquid.so* /usr/local/lib/
RUN ldconfig

COPY --from=rx-builder /build/out/feedmyfm-rx /usr/local/bin/feedmyfm-rx

ENTRYPOINT ["feedmyfm-rx"]
CMD ["--config", "/config/config.yml", "--stations", "/config/stations.yml"]
