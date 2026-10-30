#!/bin/bash
# Wolf "NHL Legacy" Moonlight app: replaces the GoW Steam image's /opt/gow/startup-app.sh so sway
# runs the game directly (no Steam). Mounted read-only by the app in /etc/wolf/cfg/config.toml.
set -e
source /opt/gow/bash-lib/utils.sh
source /opt/gow/launch-comp.sh
export SWAY_STOP_ON_APP_EXIT=yes  # end the stream session when the game exits
launcher /home/retro/nhl-linux/nhl_play.sh
