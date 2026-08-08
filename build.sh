#!/bin/bash
# build.sh - Build script for ghgtk

echo "Building ghgtk..."

# Check dependencies
for pkg in gtk+-3.0 webkit2gtk-4.1 json-c; do
    if ! pkg-config --exists $pkg; then
        echo "Error: $pkg not found"
        echo "Install: sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev libjson-c-dev"
        exit 1
    fi
done

# Check cmark (pkg-config may not work, use cmake or direct check)
if ! pkg-config --exists cmark 2>/dev/null; then
    # Try to find cmark headers directly
    if ! [ -f /usr/include/cmark.h ] && ! [ -f /usr/include/cmark/cmark.h ]; then
        echo "Error: cmark not found"
        echo "Install: sudo apt install libcmark-dev"
        exit 1
    fi
    echo "Warning: cmark pkg-config not found, using direct linking"
    CMARK_LIBS="-lcmark"
else
    CMARK_LIBS=$(pkg-config --libs cmark)
fi

# Build
g++ -o ghgtk ghgtk.cpp \
    `pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1` \
    $CMARK_LIBS -ljson-c -std=c++17 -pthread -O2

if [ $? -eq 0 ]; then
    echo "Build successful! Run: ./ghgtk"
else
    echo "Build failed."
    exit 1
fi
