#!/bin/bash
# Starts a system D-Bus inside the muvm guest.
#
# Steam reads network state from NetworkManager over D-Bus. The guest has its
# own /run and thus no system bus, so the client never registers its network
# interface and the UI stays on "Waiting for network". A bare bus is enough;
# NetworkManager itself need not run.
#
# Use: muvm -x /path/muvm-guest-dbus.sh <command>   (runs as root, pre-boot)
mkdir -p /run/dbus
[ -S /run/dbus/system_bus_socket ] || /usr/bin/dbus-daemon --system --fork
exit 0
