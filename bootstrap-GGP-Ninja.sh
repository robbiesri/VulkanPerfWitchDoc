#!/bin/bash

git submodule update --init --recursive

mkdir buildGGPNinja
cd buildGGPNinja
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$GGP_SDK_PATH/cmake/ggp.cmake" -DCMAKE_BUILD_WITH_INSTALL_RPATH=TRUE ..