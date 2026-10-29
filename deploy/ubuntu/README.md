# Ubuntu PC (master) setup

The master is an Ubuntu PC at the site. It runs the loggers' WiFi hotspot itself and provides
everything the loggers expect from the master (protocol/MQTT.md, Network):

| Service | Port | What for |
|---|---|---|
| NetworkManager hotspot (shared mode) | DHCP | The loggers' 2.4 GHz network; the PC is **10.42.0.1** |
| Mosquitto | 1883/tcp | MQTT broker |
| Avahi | 5353/udp | Advertises the broker as `_mqtt._tcp`, so loggers find it with no IP configured |
| chrony | 123/udp | NTP: the loggers' time source (they use the broker's host by default) |
| logger-master | 8080/tcp | Fleet service and dashboard |

## 1. Hotspot

```bash
HOTSPOT_PASSWORD='choose-8+-chars' bash deploy/ubuntu/hotspot.sh logger-net
```

Creates a persistent NetworkManager connection `logger-hotspot`: WPA2, 2.4 GHz (`band bg`),
channel 6 (`HOTSPOT_CHANNEL` to change), up at every boot. Check with `nmcli connection show
--active` and `ip addr` (10.42.0.1 on the WiFi interface).

It stays on without anyone touching it:

- It doesn't need any other network and never switches itself off when no one is connected.
- NetworkManager retries forever (`autoconnect-retries 0`) instead of giving up after 4 tries.
- `logger-hotspot-watchdog.timer` checks every minute and brings it back up if it's down
  (`systemctl list-timers logger-hotspot-watchdog.timer`).
- Suspend and hibernate are disabled on this PC (`systemctl unmask sleep.target suspend.target
  hibernate.target hybrid-sleep.target` undoes it). Also set the BIOS to power on after a power
  cut, so the master comes back by itself.
- Avoid saving other WiFi networks on this PC with autoconnect on: one could take the adapter
  when the hotspot drops. The watchdog takes it back within a minute either way.

- Most WiFi adapters can't host a hotspot and join another network at once. If the PC needs
  internet, use Ethernet (or a second USB WiFi adapter).
- Later, a dedicated router can replace the hotspot: delete the connection
  (`nmcli connection delete logger-hotspot`), plug the PC into the router, and add the router's
  network to the loggers' `wifi.networks`. Nothing else changes. Add the router's subnet to
  `/etc/chrony/conf.d/logger-master.conf` (`allow`).

## 2. Services

```bash
MQTT_PASSWORD='choose-one' bash deploy/ubuntu/setup.sh
```

Installs and configures Mosquitto (username `logger`, `MQTT_USER` to change), the mDNS advert,
chrony, the firewall rules (if ufw is active), and the `logger-master` systemd service
(a venv in `/opt/logger-master`, running as you, registry at `registry/devices.yaml` in this
checkout). Without `MQTT_PASSWORD` the broker accepts anyone: fine on the bench, not on site.
Run it again after pulling changes.

Dashboard: http://10.42.0.1:8080 from any device on the hotspot (a phone works), or
http://localhost:8080 on the PC.

## 3. Time

- **With internet** (Ethernet): chrony syncs from its normal upstream servers and passes the time on.
- **Without internet:** `local stratum 10` keeps chrony serving the PC's own clock, which its
  RTC keeps across power-offs. Set it right once (`timedatectl`) and it stays within seconds a
  day. The dashboard warns if the master's clock disagrees with the browser's by more than 5 s.
- Loggers sync at connect and hourly. **Set clocks from master** on the dashboard sends the
  PC's time over MQTT instead, for when NTP isn't reaching a logger.

## 4. Connect a logger

Each logger needs the hotspot's details, and the broker credentials if set. Over USB (the
logger's serial console), once per unit:

```
{"cmd":"set_config","args":{"changes":{"wifi":{"networks":[{"ssid":"logger-net","password":"…"}]},"core":{"mqtt_user":"logger","mqtt_password":"…"}}}}
```

It then joins the hotspot, finds the broker by mDNS, syncs its clock, and appears on the
dashboard (and in `registry/devices.yaml`).

## Updating loggers

New firmware goes on over WiFi from the dashboard's **Firmware** page: upload the
`firmware.bin` built on a development PC (`pio run -e vibration`), then **Update** the
loggers. Each one downloads it from this PC (port 8080, so keep that open), restarts, and rolls
itself back if the new firmware isn't healthy within 5 minutes. Loggers that are recording
refuse until they're stopped. Images are kept in `registry/firmware/` in this checkout
(git-ignored; back it up with the registry). See master/README.md, "Firmware updates".

Updating the master itself is separate: `git pull` and run `setup.sh` again (needs internet,
so Ethernet).

## Checks

```bash
systemctl status mosquitto chrony avahi-daemon logger-master
journalctl -u logger-master -f
avahi-browse -rt _mqtt._tcp                 # the broker advert
chronyc clients                              # loggers using the NTP server
mosquitto_sub -u logger -P '…' -v -t 'loggers/#'
```
