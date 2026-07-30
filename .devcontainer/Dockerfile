# Devcontainer for the PRIVATE sigil-enterprises fork of duckdb/ducklake.
#
# NOT an upstream file. It exists only so builds and tests run in a container
# instead of on an operator's machine, and it must never be cherry-picked into
# the public upstream-only fork.
#
# This deliberately does NOT adopt a tc layer. A layer owns Makefile and CI
# wiring, and this tree's Makefile is upstream's - it includes
# extension-ci-tools/makefiles/duckdb_extension.Makefile and is not ours to
# overwrite. So the image is hand-built to mirror what the fork's own CI already
# does, and nothing else.
#
# Mirrors .github/workflows/MainDistributionPipeline.yml:
#   - ubuntu-22.04 runners
#   - vcpkg pinned at a42af01b72c28a8e1d7b48107b33e4f286a55ef6
#   - ccache
# The vcpkg commit is pinned to the SAME sha as CI on purpose: a local build that
# resolves a different `roaring` than CI is a local build that proves nothing.
FROM ubuntu:22.04

ARG VCPKG_COMMIT=a42af01b72c28a8e1d7b48107b33e4f286a55ef6
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      ccache \
      cmake \
      curl \
      git \
      ninja-build \
      pkg-config \
      python3 \
      python3-pip \
      tar \
      unzip \
      zip \
 && rm -rf /var/lib/apt/lists/*

# vcpkg at the exact CI commit. Bootstrapped at build time so the toolchain file
# exists in the image and a first build does not pay for it.
#
# VCPKG_FORCE_SYSTEM_BINARIES is REQUIRED on arm64 - vcpkg refuses to bootstrap
# there otherwise, because it cannot fetch its prebuilt cmake/ninja for that
# architecture. We install both from apt above, which is what the flag tells it
# to use.
#
# ARCHITECTURE NOTE, and it matters: this image builds NATIVE. On an Apple
# Silicon workstation that is linux/arm64, while CI builds linux/amd64. That is
# a deliberate trade - forcing linux/amd64 here would run the whole DuckDB build
# under QEMU and turn a 30-70 minute build into an all-day one, for a binary
# nobody ships. The shipped extension comes from CI, which is x86_64 and remains
# the only artifact source. This container is for developing and testing the
# code, not for producing what goes to Nexus. Anything that could plausibly be
# architecture-sensitive has to be confirmed on a CI run, not here.
ENV VCPKG_FORCE_SYSTEM_BINARIES=1

RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
 && git -C /opt/vcpkg checkout ${VCPKG_COMMIT} \
 && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_TOOLCHAIN_PATH=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    PATH=/opt/vcpkg:/usr/lib/ccache:${PATH} \
    CCACHE_DIR=/ccache \
    GEN=ninja

WORKDIR /app

ENTRYPOINT ["make"]
CMD ["--help"]
