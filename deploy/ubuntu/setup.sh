#!/usr/bin/env bash
# Sets up an Ubuntu PC as the logger master: Mosquitto, mDNS advert, NTP server, fleet service.
# Run from anywhere inside the repo checkout, as your normal user (it uses sudo):
#
#   MQTT_PASSWORD='choose-one' bash deploy/ubuntu/setup.sh    # broker with a password (recommended)
#   bash deploy/ubuntu/setup.sh                            # no password: bench/development only
#
# The WiFi hotspot is separate: deploy/ubuntu/hotspot.sh. Safe to run again after changes.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SVC_USER="${SUDO_USER:-$USER}"
VENV="/opt/logger-master/venv"
MQTT_USER="${MQTT_USER:-logger}"
MQTT_PASSWORD="${MQTT_PASSWORD:-}"

echo "== packages"
sudo apt-get update
sudo apt-get install -y mosquitto mosquitto-clients avahi-daemon chrony python3-venv

echo "== mosquitto"
if [[ -n "$MQTT_PASSWORD" ]]; then
    sudo install -m 644 "$HERE/mosquitto.conf" /etc/mosquitto/conf.d/logger.conf
    sudo rm -f /etc/mosquitto/logger.passwd
    sudo mosquitto_passwd -b -c /etc/mosquitto/logger.passwd "$MQTT_USER" "$MQTT_PASSWORD"
    sudo chown mosquitto: /etc/mosquitto/logger.passwd
    sudo chmod 600 /etc/mosquitto/logger.passwd
else
    echo "   WARNING: no MQTT_PASSWORD given: anyone on the hotspot can control the loggers."
    sed -e 's/^allow_anonymous false/allow_anonymous true/' -e '/^password_file/d' "$HERE/mosquitto.conf" \
        | sudo tee /etc/mosquitto/conf.d/logger.conf > /dev/null
fi
sudo systemctl enable --now mosquitto
sudo systemctl restart mosquitto

echo "== mDNS (_mqtt._tcp)"
sudo install -m 644 "$HERE/mqtt.service" /etc/avahi/services/mqtt.service
sudo systemctl enable --now avahi-daemon
sudo systemctl reload avahi-daemon || sudo systemctl restart avahi-daemon
if grep -Eq '^\s*(allow|deny)-interfaces' /etc/avahi/avahi-daemon.conf; then
    echo "   NOTE: avahi-daemon.conf restricts interfaces; make sure the hotspot interface is allowed."
fi

echo "== NTP server (chrony)"
sudo install -d /etc/chrony/conf.d
sudo install -m 644 "$HERE/chrony.conf" /etc/chrony/conf.d/logger-master.conf
if ! grep -q '^confdir /etc/chrony/conf.d' /etc/chrony/chrony.conf; then
    echo "confdir /etc/chrony/conf.d" | sudo tee -a /etc/chrony/chrony.conf > /dev/null
fi
sudo systemctl enable --now chrony
sudo systemctl restart chrony

echo "== firewall"
if command -v ufw > /dev/null && sudo ufw status | grep -q "Status: active"; then
    sudo ufw allow 1883/tcp comment "logger MQTT"
    sudo ufw allow 123/udp comment "logger NTP"
    sudo ufw allow 8080/tcp comment "logger dashboard"
    sudo ufw allow 5353/udp comment "mDNS"
else
    echo "   ufw not active: nothing to do"
fi

echo "== fleet service"
sudo install -d -o "$SVC_USER" /opt/logger-master
[[ -x "$VENV/bin/python" ]] || python3 -m venv "$VENV"
"$VENV/bin/pip" install --upgrade pip > /dev/null
"$VENV/bin/pip" install -e "$REPO/master[server]"
mkdir -p "$REPO/registry"
printf 'LOGGER_MQTT_USER=%s\nLOGGER_MQTT_PASSWORD=%s\n' "$([[ -n "$MQTT_PASSWORD" ]] && echo "$MQTT_USER")" "$MQTT_PASSWORD" \
    | sudo tee /etc/logger-master.env > /dev/null
sudo chown "root:$SVC_USER" /etc/logger-master.env
sudo chmod 640 /etc/logger-master.env
sed -e "s|@USER@|$SVC_USER|g" -e "s|@VENV@|$VENV|g" -e "s|@REPO@|$REPO|g" "$HERE/logger-master.service" \
    | sudo tee /etc/systemd/system/logger-master.service > /dev/null
sudo systemctl daemon-reload
sudo systemctl enable --now logger-master
sudo systemctl restart logger-master

echo
echo "Done. Dashboard: http://localhost:8080 (on the hotspot: http://10.42.0.1:8080)"
[[ -n "$MQTT_PASSWORD" ]] && echo "Loggers need core.mqtt_user=$MQTT_USER and core.mqtt_password set to the same password."
echo "Check:  systemctl status mosquitto chrony avahi-daemon logger-master"
echo "        avahi-browse -rt _mqtt._tcp      chronyc clients      mosquitto_sub -v -t 'loggers/#' ..."
