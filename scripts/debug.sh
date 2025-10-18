#!/bin/bash

# Join all arguments into a single message string
msg="SonShell debug script.\nArgs: $*"

# always print out the debug msg to stdout
echo "$msg"

if command -v kdialog >/dev/null; then
    kdialog --msgbox "$msg"
#elif command -v zenity >/dev/null; then
#    zenity --info --text="$msg"
#elif command -v dialog >/dev/null; then
#    dialog --msgbox "$msg" 8 60
#elif command -v whiptail >/dev/null; then
#    whiptail --msgbox "$msg" 8 60
fi
