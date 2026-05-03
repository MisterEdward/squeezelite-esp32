#!/bin/bash

set -euo pipefail

IMAGE="${ESP_IDF_IMAGE:-sle118/squeezelite-esp32-idfv435}"
BUILD_NUMBER="${BUILD_NUMBER:-codespaces}"
DEPTH="${DEPTH:-16}"
ARTIFACT_NAME="${ARTIFACT_NAME:-flash-artifacts.zip}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "Codespaces build started"
echo "Using Docker image: ${IMAGE}"
echo "Using BUILD_NUMBER=${BUILD_NUMBER} DEPTH=${DEPTH}"

cd "${ROOT_DIR}"

echo "Ensuring submodules are available"
git submodule update --init --recursive

echo "Pulling build image"
docker pull "${IMAGE}"

echo "Cleaning previous build directory to avoid CMake path mismatches"
rm -rf build

echo "Building firmware inside Docker"
docker run --rm \
  -v "${ROOT_DIR}:/project" \
  -w /project \
  "${IMAGE}" \
  /bin/bash -lc "idf.py build -DDEPTH=${DEPTH} -DBUILD_NUMBER=${BUILD_NUMBER}"

echo "Packaging flash artifacts"
rm -f "build/${ARTIFACT_NAME}"
zip -j "build/${ARTIFACT_NAME}" \
  build/bootloader/bootloader.bin \
  build/partition_table/partition-table.bin \
  build/ota_data_initial.bin \
  build/recovery.bin \
  build/squeezelite.bin \
  build/alerts.bin \
  partitions.csv \
  partitions-debug.csv

cat <<EOF

Build complete.

Download these files from the Codespace:
  build/${ARTIFACT_NAME}

Or individual binaries:
  build/bootloader/bootloader.bin
  build/partition_table/partition-table.bin
  build/ota_data_initial.bin
  build/recovery.bin
  build/squeezelite.bin
  build/alerts.bin

Flash layout:
  0x1000   bootloader.bin
  0x8000   partition-table.bin
  0xd000   ota_data_initial.bin
  0x10000  recovery.bin
  0x160000 squeezelite.bin
  0x350000 alerts.bin
EOF
