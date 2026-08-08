#!/bin/bash
# build.sh - Build script for ghgtk

echo "Building ghgtk..."

# Check dependencies
for pkg in gtk+-3.0 webkit2gtk-4.1 json-c; do
    if ! pkg-config --exists $pkg; then
        echo "Error: $pkg not found"
        exit 1
    fi
done

# Build with cmark-gfm
g++ -o ghgtk ghgtk.cpp \
    `pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1` \
    -lcmark -ljson-c -std=c++17 -pthread -O2

if [ $? -eq 0 ]; then
    echo "Build successful! Run: ./ghgtk"
else
    echo "Build failed."
    exit 1
fi
