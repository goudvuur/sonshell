#!/bin/bash

# Kill other running instances except the current one
# -f matches the full command line
# -o excludes the oldest instance (keeps the new one, kills others)
pkill -f -o "$0"

echo "Showing image $1"
xdg-open "$1"

