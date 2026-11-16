# lldstore cluster node image (Phase 11). Multi-stage: build with the full toolchain, ship only the
# binaries on a slim base. Build: docker build -t lldstore .
#
# Benchmarks run INSIDE a container are container numbers (shared kernel, cgroup limits, a virtual
# network). They are never quoted as bare-metal results (see docs/BENCHMARKS.md).
FROM ubuntu:24.04 AS build
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# gRPC is optional and not installed here: the cluster uses its own TCP protocol.
RUN cmake --preset release -G Ninja && cmake --build --preset release --target kvnode kvctl lincheck

FROM ubuntu:24.04
RUN useradd --system --create-home lldstore && mkdir -p /data && chown lldstore /data
COPY --from=build /src/build/release/apps/kvnode /src/build/release/apps/kvctl /src/build/release/apps/lincheck /usr/local/bin/
USER lldstore
VOLUME ["/data"]
EXPOSE 7001 8001 9101
ENTRYPOINT ["kvnode"]
