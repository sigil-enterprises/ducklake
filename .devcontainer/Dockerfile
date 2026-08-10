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
#   - ubuntu-22.04 base, but with GCC 14 (see the compiler note below - upstream
#     builds Linux in manylinux_2_28 with GCC 14.2.1, and this branch needs it)
#   - vcpkg pinned at 84bab45d415d22042bd0b9081aea57f362da3f35
#   - ccache
# The vcpkg commit is pinned to the SAME sha as CI on purpose: a local build that
# resolves a different `roaring` than CI is a local build that proves nothing.
#
# CI uses TWO vcpkg shas, and the choice here matters. The GCC-12 compatibility
# job pins a42af01b..., but that job builds the extension with NO extension test
# deps. The relassert job pins 84bab45d..., which is also the `builtin-baseline`
# that duckdb's own merge_vcpkg_deps.py writes into the generated manifest.
#
# So a42af01b cannot build with BUILD_EXTENSION_TEST_DEPS=full at all: the merged
# manifest asks for curl 8.17.0 / openssl 3.6.0 / roaring 4.5.0 and that older
# version database has no entry for any of them. We need the full deps, because
# httpfs is what supplies the real mbedtls crypto module for encrypted Parquet -
# without it the proof has to fall back to `force_mbedtls_unsafe`, a setting this
# repo otherwise declares forbidden.
FROM ubuntu:22.04

ARG VCPKG_COMMIT=84bab45d415d22042bd0b9081aea57f362da3f35
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

# GCC 14, and it is load-bearing on the v1.5.3 branch - not a preference.
#
# duckdb v1.5.3 pins `set(CMAKE_CXX_STANDARD "11" ...)` in its top-level
# CMakeLists (duckdb `main` pins 17). Under -std=c++11, GCC 11.4 and GCC 12.3
# both REJECT two upstream DuckLake files that return a derived
# `duckdb::unique_ptr` where the base type is declared:
#
#   src/metadata_manager/quack_metadata_manager.cpp:49
#   src/functions/ducklake_flush_inlined_data.cpp:760
#
# `duckdb::unique_ptr` inherits its constructors (`using original::original;`),
# and the derived-to-base conversion those two returns rely on resolves under
# GCC 14 but not under 11 or 12. Measured, not assumed: the identical failure
# reproduces on PRISTINE upstream v1.5-variegata with no fork changes at all,
# and the same file compiles clean once GCC 14 is used.
#
# Upstream's Linux CI does not hit this because it builds inside
# quay.io/pypa/manylinux_2_28_x86_64 with GCC 14.2.1. Ubuntu 22.04's default
# toolchain is simply too old for this branch, so we take 14 from the toolchain
# PPA and pin the compiler explicitly rather than inheriting `cc`/`c++`.
# gpg-agent + dirmngr are needed explicitly: add-apt-repository imports the PPA
# signing key through gpg, and --no-install-recommends drops both.
RUN apt-get update && apt-get install -y --no-install-recommends \
      software-properties-common \
      gpg-agent \
      dirmngr \
 && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
 && apt-get update && apt-get install -y --no-install-recommends \
      gcc-14 \
      g++-14 \
 && rm -rf /var/lib/apt/lists/*

ENV CC=/usr/bin/gcc-14
ENV CXX=/usr/bin/g++-14

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

# gcovr, for the opt-in coverage build (DUCKLAKE_ENABLE_COVERAGE in the
# top-level CMakeLists.txt). Pinned, and taken from pip rather than apt.
#
# apt is not an option here: `apt-cache policy gcovr` on this image offers
# 5.0-2, a 2021 release that predates GCC 14 by three years and cannot be
# trusted to read its gcov output. (It also spells the flag `--xml`, not
# `--cobertura` - cosmetic next to the format problem, but it means a copied
# command line fails outright rather than quietly.)
#
# It must be driven with `--gcov-executable gcov-14`. gcov and the compiler that
# wrote the .gcno share a format version, and the default `gcov` here is 11:
# pointed at GCC 14 notes it SEGFAULTS on every .gcda and gcovr then writes no
# report at all. That is the good outcome - loud. `--gcov-ignore-errors` is what
# would turn it into a well-formed, entirely empty report, so do not reach for it.
RUN pip3 install --no-cache-dir 'gcovr==7.2'

WORKDIR /app

ENTRYPOINT ["make"]
CMD ["--help"]
