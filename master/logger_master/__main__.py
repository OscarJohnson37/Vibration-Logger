"""Run the master.

    python -m logger_master serve [--broker localhost] [--http-port 8080] [--registry PATH]
    python -m logger_master simulate [--broker localhost] [--count 3]
    python -m logger_master broker [--port 1883]     # development broker (amqtt); the master uses Mosquitto
"""

from __future__ import annotations

import argparse
import logging
import os
import sys
from pathlib import Path

DEFAULT_REGISTRY = Path(__file__).resolve().parents[2] / "registry" / "devices.yaml"


def cmd_serve(args: argparse.Namespace) -> int:
    import uvicorn

    from .features import Features
    from .fleet import Fleet
    from .models import Models
    from .registry import Registry
    from .views import ViewStore
    from .web import create_app

    # The model catalogue, orientation views and 3D models live next to the registry
    models = Models(args.registry.parent / "models")
    fleet = Fleet(args.broker, args.mqtt_port, username=args.mqtt_user, password=args.mqtt_password,
                  registry=Registry(args.registry, models=models), features=Features(args.features))
    app = create_app(fleet, ViewStore(args.registry.parent / "views"), models)
    print(f"dashboard: http://{'localhost' if args.host in ('0.0.0.0', '') else args.host}:{args.http_port}  "
          f"(broker {args.broker}:{args.mqtt_port}, registry {args.registry})")
    uvicorn.run(app, host=args.host, port=args.http_port, log_level="warning")
    return 0


def cmd_simulate(args: argparse.Namespace) -> int:
    from .simulate import run

    run(args.broker, args.mqtt_port, args.count, args.status_interval)
    return 0


def cmd_broker(args: argparse.Namespace) -> int:
    """A pure-Python broker for development on a PC without Mosquitto."""
    import asyncio

    try:
        from amqtt.broker import Broker
    except ImportError:
        print("the development broker needs amqtt: pip install amqtt", file=sys.stderr)
        return 1
    config = {
        "listeners": {"default": {"type": "tcp", "bind": f"0.0.0.0:{args.port}"}},
        "plugins": {"amqtt.plugins.authentication.AnonymousAuthPlugin": {"allow_anonymous": True}},
    }

    for name in ("amqtt", "transitions"):
        logging.getLogger(name).setLevel(logging.WARNING)

    async def main():
        broker = Broker(config)
        await broker.start()
        print(f"development broker on port {args.port} (Ctrl+C to stop)")
        await asyncio.Event().wait()

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="python -m logger_master")
    sub = p.add_subparsers(dest="command", required=True)

    def mqtt_args(sp):
        sp.add_argument("--broker", default="localhost", help="MQTT broker host")
        sp.add_argument("--mqtt-port", type=int, default=1883)

    s = sub.add_parser("serve", help="run the fleet service and dashboard")
    mqtt_args(s)
    s.add_argument("--mqtt-user", default=os.environ.get("LOGGER_MQTT_USER", ""))
    s.add_argument("--mqtt-password", default=os.environ.get("LOGGER_MQTT_PASSWORD", ""),
                   help="default: $LOGGER_MQTT_PASSWORD (keeps it off the command line)")
    s.add_argument("--host", default="0.0.0.0", help="address to serve the dashboard on")
    s.add_argument("--http-port", type=int, default=8080)
    s.add_argument("--registry", type=Path, default=DEFAULT_REGISTRY)
    s.add_argument("--features", type=Path, default=None, help="protocol/features folder")
    s.add_argument("-v", "--verbose", action="store_true")
    s.set_defaults(fn=cmd_serve)

    s = sub.add_parser("simulate", help="run simulated loggers")
    mqtt_args(s)
    s.add_argument("--count", type=int, default=3)
    s.add_argument("--status-interval", type=int, default=10, help="seconds between status reports")
    s.set_defaults(fn=cmd_simulate)

    s = sub.add_parser("broker", help="development MQTT broker (amqtt)")
    s.add_argument("--port", type=int, default=1883)
    s.set_defaults(fn=cmd_broker)

    args = p.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if getattr(args, "verbose", False) else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
