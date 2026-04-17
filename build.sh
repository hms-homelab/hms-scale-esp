#!/bin/bash

# Giraffe Scale ESP-IDF Build Script

set -e

echo "🦒 Building Giraffe Scale Gateway..."

# Check if ESP-IDF is installed
if [ -z "$IDF_PATH" ]; then
    echo "❌ ESP-IDF not found. Please install ESP-IDF and source export.sh"
    echo "   Example: . $HOME/esp/esp-idf/export.sh"
    exit 1
fi

# Configure project (only needed first time or after changes)
if [ "$1" = "menuconfig" ]; then
    echo "📝 Opening configuration menu..."
    idf.py menuconfig
    exit 0
fi

# Clean build if requested
if [ "$1" = "clean" ]; then
    echo "🧹 Cleaning build directory..."
    idf.py fullclean
    exit 0
fi

# Build
echo "🔨 Building project..."
idf.py build

# Flash if requested
if [ "$1" = "flash" ]; then
    echo "📤 Flashing to device..."
    idf.py -p /dev/cu.usbmodem* flash
fi

# Monitor if requested
if [ "$1" = "monitor" ]; then
    echo "📊 Starting serial monitor..."
    idf.py -p /dev/cu.usbmodem* monitor
fi

# Flash and monitor if requested
if [ "$1" = "flash-monitor" ]; then
    echo "📤 Flashing and monitoring..."
    idf.py -p /dev/cu.usbmodem* flash monitor
fi

echo "✅ Build complete!"
