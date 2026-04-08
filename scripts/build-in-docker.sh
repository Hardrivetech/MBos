#!/usr/bin/env bash
set -euo pipefail

IMAGE=mbos-builder:latest

echo "Building Docker image ${IMAGE}..."
docker build -t "${IMAGE}" .

echo "Running build inside container (mounted to current directory)..."
docker run --rm -v "$(pwd)":/work -w /work "${IMAGE}" make

echo "Build finished. Artifacts are available in $(pwd)/build"
