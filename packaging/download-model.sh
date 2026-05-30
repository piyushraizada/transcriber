#!/bin/bash
#
# download-model.sh — Download the Whisper large-v3-turbo-q8_0 model
#
# This script downloads the bundled model file required by Transcriber.
# The model is approximately 1.1 GiB and is stored in the models/ directory.
#
# Usage:
#   ./packaging/download-model.sh [output_directory]
#
# If no output directory is specified, defaults to ./models
#

set -euo pipefail

MODEL_NAME="large-v3-turbo-q8_0"
MODEL_FILE="ggml-large-v3-turbo-q8_0.bin"
OUTPUT_DIR="${1:-models}"
DOWNLOAD_URL="https://huggingface.co/ggml-org/models/resolve/main/whisper.cpp/ggml-${MODEL_NAME}.bin"

echo "=== Transcriber Model Downloader ==="
echo ""

# Check if model already exists
if [ -f "${OUTPUT_DIR}/${MODEL_FILE}" ]; then
    SIZE=$(du -h "${OUTPUT_DIR}/${MODEL_FILE}" | cut -f1)
    echo "Model already exists: ${OUTPUT_DIR}/${MODEL_FILE} (${SIZE})"
    echo "To re-download, remove the file and run this script again."
    exit 0
fi

# Create output directory
mkdir -p "${OUTPUT_DIR}"

echo "Downloading Whisper model: ${MODEL_NAME}"
echo "URL: ${DOWNLOAD_URL}"
echo "Destination: ${OUTPUT_DIR}/${MODEL_FILE}"
echo ""

# Try wget first, fall back to curl
if command -v wget >/dev/null 2>&1; then
    wget --no-check-certificate --quiet --show-progress -O "${OUTPUT_DIR}/${MODEL_FILE}" "${DOWNLOAD_URL}"
elif command -v curl >/dev/null 2>&1; then
    curl -# -f -L -o "${OUTPUT_DIR}/${MODEL_FILE}" "${DOWNLOAD_URL}"
else
    echo "Error: Neither wget nor curl found. Please install one of them."
    exit 1
fi

# Verify download
if [ -f "${OUTPUT_DIR}/${MODEL_FILE}" ]; then
    SIZE=$(du -h "${OUTPUT_DIR}/${MODEL_FILE}" | cut -f1)
    echo ""
    echo "Download complete: ${OUTPUT_DIR}/${MODEL_FILE} (${SIZE})"
else
    echo "Error: Download failed or file not found."
    exit 1
fi
