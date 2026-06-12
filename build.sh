#!/bin/bash

set -e

echo "=========================="
echo " Clean"
echo "=========================="

rm -rf build

echo "=========================="
echo " Configure"
echo "=========================="

cmake -B build

echo "=========================="
echo " Build"
echo "=========================="

cmake --build build -j$(nproc)

echo "=========================="
echo " Done"
echo "=========================="