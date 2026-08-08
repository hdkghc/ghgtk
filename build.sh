#!/bin/bash
# build.sh - Build script for ghgtk

echo "Building ghgtk..."

# Check dependencies
for pkg in gtk+-3.0 webkit2gtk-4.1; do
    if ! pkg-config --exists $pkg; then
        echo "Error: $pkg not found"
        echo "Install: sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev"
        exit 1
    fi
done

# Check json-c
if ! pkg-config --exists json-c; then
    echo "Error: json-c not found"
    echo "Install: sudo apt install libjson-c-dev"
    exit 1
fi

# Check cmark
if ! pkg-config --exists cmark; then
    echo "Error: cmark not found"
    echo "Install: sudo apt install libcmark-dev"
    exit 1
fi

# Build
g++ -o ghgtk ghgtk.cpp \
    `pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1` \
    -ljson-c -lcmark -std=c++17 -pthread -O2

if [ $? -eq 0 ]; then
    echo "Build successful! Run: ./ghgtk"
else
    echo "Build failed."
    exit 1
fi
