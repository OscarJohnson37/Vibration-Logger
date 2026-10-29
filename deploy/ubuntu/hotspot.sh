#!/usr/bin/env bash
# Creates the loggers' WiFi network on this PC with NetworkManager: a persistent 2.4 GHz WPA2
# hotspot that comes up at boot. "shared" mode gives the PC 10.42.0.1 and runs DHCP for the
# loggers, so the broker (1883), NTP (123) and dashboard (8080) are all at 10.42.0.1.
# It's kept up for good: unlimited NetworkManager retries, a once-a-minute watchdog
# (logger-hotspot-watchdog.timer) that brings it back if it's down, and no suspend on this PC.
#
#   HOTSPOT_PASSWORD='at-least-8-chars' bash deploy/ubuntu/hotspot.sh [SSID] [WIFI_INTERFACE]
#
# The ESP32 is 2.4 GHz only, hence band bg and a fixed channel. Most WiFi adapters can't run a
# hotspot and join another network at the same time: give the PC its internet over Ethernet (or
# a second USB WiFi adapter) if it needs internet.
set -euo pipefail

SSID="${1:-logger-net}"
IFACE="${2:-$(nmcli -t -f DEVICE,TYPE device | awk -F: '$2=="wifi"{print $1; exit}')}"
CHANNEL="${HOTSPOT_CHANNEL:-6}"
PASSWORD="${HOTSPOT_PASSWORD:?set HOTSPOT_PASSWORD (8-63 characters)}"
NAME="logger-hotspot"

if [[ -z "$IFACE" ]]; then
    echo "no WiFi interface found (nmcli device)" >&2
    exit 1
fi
if (( ${#PASSWORD} < 8 )); then
    echo "HOTSPOT_PASSWORD must be at least 8 characters" >&2
    exit 1
fi

nmcli connection delete "$NAME" > /dev/null 2>&1 || true
nmcli connection add type wifi ifname "$IFACE" con-name "$NAME" ssid "$SSID" \
    connection.autoconnect yes connection.autoconnect-priority 100 connection.autoconnect-retries 0 \
    802-11-wireless.mode ap 802-11-wireless.band bg 802-11-wireless.channel "$CHANNEL" \
    802-11-wireless.powersave 2 \
    wifi-sec.key-mgmt wpa-psk wifi-sec.proto rsn wifi-sec.pairwise ccmp wifi-sec.group ccmp \
    wifi-sec.psk "$PASSWORD" \
    ipv4.method shared ipv4.addresses 10.42.0.1/24 ipv6.method disabled
nmcli connection up "$NAME"

# Watchdog: once a minute, bring the hotspot back if it isn't active (driver hiccup, a WiFi
# profile grabbing the adapter, someone switching it off by accident)
sudo tee /etc/systemd/system/logger-hotspot-watchdog.service > /dev/null <<EOF
[Unit]
Description=Bring the loggers' WiFi hotspot back up if it's down
After=NetworkManager.service

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'nmcli -t -f NAME connection show --active | grep -qx "$NAME" || nmcli connection up "$NAME"'
EOF
sudo tee /etc/systemd/system/logger-hotspot-watchdog.timer > /dev/null <<EOF
[Unit]
Description=Check the loggers' WiFi hotspot every minute

[Timer]
OnBootSec=1min
OnUnitActiveSec=1min

[Install]
WantedBy=timers.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now logger-hotspot-watchdog.timer

# The master must never sleep: the hotspot, broker and dashboard all stop when it does
sudo systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target > /dev/null

echo
echo "Hotspot '$SSID' on $IFACE, channel $CHANNEL, master at 10.42.0.1."
echo "Give each logger these WiFi details (wifi.networks), e.g. over USB:"
echo "  {\"cmd\":\"set_config\",\"args\":{\"changes\":{\"wifi\":{\"networks\":[{\"ssid\":\"$SSID\",\"password\":\"…\"}]}}}}"
