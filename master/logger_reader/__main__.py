"""Command-line tools for DLOG files.

    python -m logger_reader info FILE [FILE ...]
    python -m logger_reader export FILE --stream accel --csv out.csv [--raw] [--mounted]
    python -m logger_reader capture COM22 out.dlog [--seconds 30]
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from .dlog import MAGIC
from .reader import read_dlog


def cmd_info(args: argparse.Namespace) -> int:
    for p in args.files:
        print(read_dlog(p).summary())
    return 0


def cmd_export(args: argparse.Namespace) -> int:
    f = read_dlog(args.file)
    names = [args.stream] if args.stream else [s["name"] for s in f.streams]
    for name in names:
        df = f.to_dataframe(name, scaled=not args.raw, mounted=args.mounted)
        out = Path(args.csv) if args.csv and len(names) == 1 else Path(args.file).with_suffix(f".{name}.csv")
        df.to_csv(out)
        print(f"{name}: {len(df)} rows -> {out}")
    return 0


def cmd_capture(args: argparse.Namespace) -> int:
    """Record the firmware's USB DLOG stream (recording sink 'usb') to a file."""
    try:
        import serial  # pyserial
    except ImportError:
        print("capture needs pyserial: pip install pyserial", file=sys.stderr)
        return 1
    with serial.Serial() as port:
        port.port, port.baudrate, port.timeout = args.port, 115200, 0.2
        port.dtr = port.rts = False  # don't reset the board (native USB)
        port.open()
        port.reset_input_buffer()
        buf = b""
        start = time.monotonic()
        last_ask = 0.0
        print(f"waiting for the logger's DLOG stream (up to {args.wait:g} s)…")
        while MAGIC not in buf:
            now = time.monotonic()
            if now - start > args.wait:
                print("no DLOG stream seen. Is the logger recording (not still counting down) "
                      "with the 'usb' sink?", file=sys.stderr)
                return 1
            if now - last_ask > 2:
                # ask the firmware to restart its USB stream with a fresh header; repeated because
                # it's ignored until recording has started (e.g. during the auto-start countdown)
                port.write(b"\ndlog\n")
                last_ask = now
            buf = (buf + port.read(4096))[-65536:]
        buf = buf[buf.index(MAGIC):]
        total = len(buf)
        end = time.monotonic() + args.seconds
        with open(args.out, "wb") as out:
            out.write(buf)
            try:
                while time.monotonic() < end:
                    chunk = port.read(65536)
                    out.write(chunk)
                    total += len(chunk)
                    print(f"\r{total / 1024:8.0f} kB", end="", flush=True)
            except KeyboardInterrupt:
                pass
    print(f"\nwrote {args.out}")
    print(read_dlog(args.out).summary())
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m logger_reader", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="summarise DLOG files")
    p.add_argument("files", nargs="+")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("export", help="export streams to CSV")
    p.add_argument("file")
    p.add_argument("--stream")
    p.add_argument("--csv")
    p.add_argument("--raw", action="store_true", help="raw counts instead of engineering units")
    p.add_argument("--mounted", action="store_true", help="apply the mounting axis map")
    p.set_defaults(func=cmd_export)

    p = sub.add_parser("capture", help="record the USB DLOG stream from a logger")
    p.add_argument("port")
    p.add_argument("out")
    p.add_argument("--seconds", type=float, default=30, help="how long to record")
    p.add_argument("--wait", type=float, default=20, help="how long to wait for the stream to start")
    p.set_defaults(func=cmd_capture)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
