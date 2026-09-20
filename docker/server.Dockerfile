# The ScorchDroid dedicated server.
#
# Built from the same portable engine the Android app compiles - upstream
# Scorched3D's src/common + src/server, this port's porting/ shims, and the
# vendored expat/lua/libpng/libjpeg-turbo. No SDL, no OpenAL, no meson: the
# whole point of this port's dependency diet is that a server needs a C++
# compiler, CMake and zlib and nothing else.
#
# Build context is the repository root, and it must be a checkout with its
# submodules present:
#     git clone --recurse-submodules https://github.com/roge-rm/ScorchDroid.git

# Trixie rather than bookworm, and the same on both stages so the C++ ABI
# matches: upstream's Vector.hpp calls std::sinf/std::cosf/std::sqrtf, and
# libstdc++ only gained the C++17 float overloads in GCC 13. Bookworm's GCC
# 12 fails to compile it. The NDK's clang and any recent host GCC are fine,
# which is why nothing else in this repo hit it.
FROM debian:trixie-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential cmake git zlib1g-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# dedicated-server/ is its own CMake project precisely so a server image
# never compiles host-tests' 5000-line test binary.
RUN cmake -S dedicated-server -B /build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build --target dedicated_server -j"$(nproc)" \
 && strip /build/dedicated_server


FROM debian:trixie-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends zlib1g \
 && rm -rf /var/lib/apt/lists/*

# A fixed uid, shared with the web image: the two containers pass a Unix
# socket between them through a volume, and the socket's permissions are the
# only access control it has.
RUN groupadd --gid 10001 scorchdroid \
 && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin scorchdroid

COPY --from=build /build/dedicated_server /opt/scorchdroid/bin/dedicated-server
# Upstream's data/ - weapons, landscapes, meshes, tanks, the lot (~88MB).
# The server reads it with plain fopen() on paths relative to its working
# directory, so it lives beside the binary rather than being extracted
# anywhere at runtime.
COPY --from=build /src/third_party/scorched3d/data /opt/scorchdroid/data
# The seed a fresh /config is written from.
COPY --from=build /src/dedicated-server/dedicated_server.xml /opt/scorchdroid/seed.xml

# Owned at image build time so the named volumes inherit it when Docker
# first populates them - otherwise they arrive root-owned and the server
# cannot write its own config.
RUN mkdir -p /config /run/scorchdroid \
 && chown -R scorchdroid:scorchdroid /config /run/scorchdroid

ENV SCORCHDROID_DATA_ROOT=/opt/scorchdroid \
    SCORCHDROID_CONFIG=/config/server.xml \
    SCORCHDROID_SEED_CONFIG=/opt/scorchdroid/seed.xml \
    SCORCHDROID_STATE_DIR=/config/state \
    SCORCHDROID_CONTROL_SOCKET=/run/scorchdroid/control.sock \
    SCORCHDROID_PORT=27270

VOLUME ["/config", "/run/scorchdroid"]
EXPOSE 27270/tcp

USER scorchdroid

# The socket only exists once the server is actually serving, which makes
# it a better liveness signal than the process being up.
HEALTHCHECK --interval=30s --timeout=3s --start-period=60s \
    CMD test -S "$SCORCHDROID_CONTROL_SOCKET"

ENTRYPOINT ["/opt/scorchdroid/bin/dedicated-server"]
