#!/bin/bash

set -e -x 
VOSK_BUILD_JOBS=${VOSK_BUILD_JOBS:-3}
docker build --build-arg VOSK_BUILD_JOBS="$VOSK_BUILD_JOBS" --file Dockerfile.win --tag alphacep/kaldi-win:latest .
docker run --rm -e VOSK_BUILD_JOBS="$VOSK_BUILD_JOBS" -v `realpath ..`:/io alphacep/kaldi-win bash /io/travis/build-wheels-win.sh
