#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="at-sf2-player-builder:ubuntu20.04"

docker build -f "${ROOT_DIR}/Dockerfile.ubuntu20" -t "${IMAGE_NAME}" "${ROOT_DIR}"

docker run --rm \
    -u "$(id -u):$(id -g)" \
    -v "${ROOT_DIR}:/src" \
    -w /src \
    "${IMAGE_NAME}" \
    bash -c "make clean && make && make test && make dist"

echo ""
echo "Bundle pronto em: ${ROOT_DIR}/build/ATSF2Player-linux-x86_64.zip"
echo "Para instalar, extraia diretamente em ~/.vst3/:"
echo "  unzip -o build/ATSF2Player-linux-x86_64.zip -d ~/.vst3/"
