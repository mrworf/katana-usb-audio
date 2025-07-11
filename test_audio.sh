#!/bin/bash

# Test script for Katana USB Audio driver

echo "=== Katana USB Audio Driver Test ==="

# Check if module is loaded
echo "1. Checking if driver is loaded..."
if lsmod | grep -q katana_usb_audio; then
    echo "   ✓ Driver is loaded"
else
    echo "   ✗ Driver is not loaded"
    echo "   Loading driver..."
    sudo modprobe katana_usb_audio
fi

# Check for ALSA device
echo "2. Checking ALSA devices..."
if aplay -l | grep -q "Katana"; then
    echo "   ✓ Katana device found in ALSA"
    aplay -l | grep "Katana"
else
    echo "   ✗ Katana device not found in ALSA"
fi

# Check for PulseAudio device
echo "3. Checking PulseAudio devices..."
if pactl list short sinks | grep -q "katana"; then
    echo "   ✓ Katana device found in PulseAudio"
    pactl list short sinks | grep "katana"
else
    echo "   ✗ Katana device not found in PulseAudio"
fi

# Test volume control
echo "4. Testing volume control..."
if amixer -c katana-usb-audio sget "PCM Playback Volume" > /dev/null 2>&1; then
    echo "   ✓ Volume control accessible"
    amixer -c katana-usb-audio sget "PCM Playback Volume"
else
    echo "   ✗ Volume control not accessible"
fi

# Test mute control
echo "5. Testing mute control..."
if amixer -c katana-usb-audio sget "PCM Playback Switch" > /dev/null 2>&1; then
    echo "   ✓ Mute control accessible"
    amixer -c katana-usb-audio sget "PCM Playback Switch"
else
    echo "   ✗ Mute control not accessible"
fi

# Test audio playback (if device is available)
echo "6. Testing audio playback..."
if aplay -l | grep -q "Katana"; then
    echo "   Attempting to play test tone..."
    # Generate a test tone and try to play it
    if command -v speaker-test > /dev/null; then
        speaker-test -D katana-usb-audio -c 2 -t sine -f 1000 -l 1
    else
        echo "   speaker-test not available, skipping playback test"
    fi
else
    echo "   Skipping playback test - device not available"
fi

echo "=== Test Complete ===" 