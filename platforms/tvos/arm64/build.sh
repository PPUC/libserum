#!/bin/bash

set -e

./platforms/build-common.sh tvos arm64 -DBUILD_SHARED=OFF
