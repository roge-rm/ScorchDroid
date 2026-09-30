#!/bin/bash
# Taps the first control on the emulator's screen with this text, for
# driving the app from a script:  tools/emulator_tap.sh "Single Player"
# The emulator is Pixel_5-2 on emulator-5556 (Pixel_5-1 is left free).
export PATH=$PATH:~/Android/Sdk/platform-tools
D="adb -s emulator-5556"
$D shell uiautomator dump /sdcard/ui.xml >/dev/null 2>&1
b=$($D shell cat /sdcard/ui.xml | grep -o "text=\"$1\"[^>]*bounds=\"[^\"]*\"" | head -1 | grep -o 'bounds="[^"]*"' | grep -o '[0-9]*' | tr '\n' ' ')
[ -z "$b" ] && { echo "not found: $1"; exit 1; }
set -- $b
$D shell input tap $(( ($1+$3)/2 )) $(( ($2+$4)/2 )); echo "tapped $(( ($1+$3)/2 )),$(( ($2+$4)/2 ))"
