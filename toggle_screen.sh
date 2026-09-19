#!/bin/bash

# Get the raw status (e.g., "display_power=1")
status=$(vcgencmd display_power)

# Check if the status contains "=1" (meaning ON)
if [[ "$status" == *"=1"* ]]; then
    # It's ON, so turn it OFF
    vcgencmd display_power 0
else
    # It's OFF (or unknown), so turn it ON
    vcgencmd display_power 1
fi
