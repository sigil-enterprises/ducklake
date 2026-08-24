PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=ducklake
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Core extensions that we need for crucial testing
DEFAULT_TEST_EXTENSION_DEPS=
# For cloud testing we also need these extensions
FULL_TEST_EXTENSION_DEPS=httpfs;postgres_scanner

# Aws and Azure have vcpkg dependencies and therefore need vcpkg merging
ifeq (${BUILD_EXTENSION_TEST_DEPS}, full)
	USE_MERGED_VCPKG_MANIFEST:=1
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

#### Local check gate (ducklake#27, ducklake-bench#102)
# The included duckdb_extension.Makefile's format-check invokes plain
# `clang-format`, which CI gets from `pip install clang_format==11.0.1`. That
# same PyPI release's macOS wheel is `universal2` (covers arm64 fine), but its
# only Linux wheels are manylinux i686/x86_64 - no aarch64 - so plain
# `pip install` fails on this repo's own Linux arm64 devcontainer. This wraps
# the same command behind a PATH whose `format-check-tools` step resolves a
# working clang-format 11 for whatever platform it's run on - see
# scripts/ci/setup-clang-format.sh for the exact resolution order.
FORMAT_TOOLS_BIN := ${PROJ_DIR}build/format-tools/bin
FORMAT_VENV := ${PROJ_DIR}.cache/format-venv

.PHONY: format-check-tools format-check lint test check format-check-selftest

format-check-tools:
	@test -x "$(FORMAT_VENV)/bin/python" || python3 -m venv "$(FORMAT_VENV)"
	@"$(FORMAT_VENV)/bin/python" -m pip install -q --upgrade pip
	@"$(FORMAT_VENV)/bin/python" -m pip install -q cmake-format 'black==24.*' cxxheaderparser pcpp
	@${PROJ_DIR}scripts/ci/setup-clang-format.sh "$(FORMAT_TOOLS_BIN)" "$(FORMAT_VENV)/bin/python"

# Overrides the format-check recipe from duckdb_extension.Makefile. Uses the
# venv's python and puts both FORMAT_TOOLS_BIN (clang-format, resolved by
# setup-clang-format.sh) and the venv's own bin (black, cmake-format) on PATH -
# see scripts/ci/setup-clang-format.sh.
format-check: format-check-tools
	PATH="$(FORMAT_TOOLS_BIN):$(FORMAT_VENV)/bin:$$PATH" "$(FORMAT_VENV)/bin/python" duckdb/scripts/format.py --all --check --directories src test

format-check-selftest: format-check-tools
	${PROJ_DIR}scripts/ci/format-check-selftest.sh "${PROJ_DIR}"

lint: tidy-check

check: format-check lint test
