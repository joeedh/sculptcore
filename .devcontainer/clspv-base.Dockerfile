# clspv base image — published to ghcr.io/joeedh/clspv-base:<CLSPV_COMMIT>
#
# clspv pins LLVM at a specific commit on llvm-project main (see
# https://github.com/google/clspv/blob/main/deps.json) and applies a local
# patch. There is no supported way to link it against a prebuilt LLVM at
# arbitrary commits, so the build compiles LLVM + Clang from source — the
# 10-30 minute long pole of the sculptcore devcontainer image.
#
# Splitting clspv into its own base image and publishing it tagged by
# CLSPV_COMMIT turns that cost into "once per pin bump" instead of "every
# clean rebuild." The main devcontainer Dockerfile does:
#
#     COPY --from=ghcr.io/joeedh/clspv-base:<sha> /opt/clspv/bin/clspv \
#          /usr/local/bin/clspv
#
# Build context for this Dockerfile is the sculptcore repo root, so
# `ci/versions.env` is available for the CLSPV_COMMIT pin.

# ---------------------------------------------------------------------------
# Stage 1: build clspv (and the LLVM/Clang it embeds).
# ---------------------------------------------------------------------------
FROM debian:trixie AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl wget git xz-utils \
        build-essential pkg-config python3 python3-pip python-is-python3 \
        cmake ninja-build gnupg lsb-release \
    && rm -rf /var/lib/apt/lists/*

# Install clang from apt.llvm.org. Building LLVM with clang is meaningfully
# faster than with gcc, and the LLVM_VERSION pin from versions.env is what
# the main devcontainer uses, so reusing it keeps the two consistent.
COPY ci/versions.env /tmp/versions.env
RUN . /tmp/versions.env && \
    curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh && \
    bash /tmp/llvm.sh "${LLVM_VERSION}" && \
    apt-get install -y --no-install-recommends \
        "clang-${LLVM_VERSION}" "lld-${LLVM_VERSION}" && \
    update-alternatives --install /usr/bin/clang   clang   "/usr/bin/clang-${LLVM_VERSION}"   100 && \
    update-alternatives --install /usr/bin/clang++ clang++ "/usr/bin/clang++-${LLVM_VERSION}" 100 && \
    update-alternatives --install /usr/bin/ld.lld  ld.lld  "/usr/bin/lld-${LLVM_VERSION}"     100 && \
    rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

# Fetch clspv at the pinned commit (deps.json then pulls llvm-project at
# clspv's pinned LLVM commit) and build only the `clspv` target. Use
# fetch-by-SHA so any reachable commit works.
RUN . /tmp/versions.env && \
    git init /tmp/clspv && cd /tmp/clspv && \
    git remote add origin https://github.com/google/clspv.git && \
    git -c protocol.version=2 fetch --depth=1 origin "${CLSPV_COMMIT}" && \
    git checkout FETCH_HEAD && \
    python3 utils/fetch_sources.py --shallow --ci && \
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DLLVM_USE_LINKER=lld && \
    cmake --build build --target clspv && \
    install -D -m0755 build/bin/clspv /opt/clspv/bin/clspv && \
    rm -rf /tmp/clspv

# ---------------------------------------------------------------------------
# Stage 2: ship just the binary. Consumers `COPY --from=` it directly; no
# runtime image surface means no apt churn, no shell, no CVEs from base.
# ---------------------------------------------------------------------------
FROM scratch
COPY --from=build /opt/clspv/bin/clspv /opt/clspv/bin/clspv
