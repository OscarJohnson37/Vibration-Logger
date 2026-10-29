"""Tests for sessions in the reader (FILE_FORMAT.md v1.1, Sessions)."""

import numpy as np

from logger_reader import DlogWriter, find_sessions, read_dlog, read_metadata


def write(path, boot, index, session=None, serial="VL-0001", dev="9070692f3338", t0=0):
    md = {
        "format": "dlog", "format_version": "1.1" if session else "1.0",
        "device": {"id": dev, "serial": serial, "type": "vibration", "hardware": "x", "firmware": "t"},
        "boot": {"id": boot}, "file": {"index": index},
        "time": {"synced": True, "mono_us": 0, "utc_us": 1_791_270_000_000_000 + boot * 10**10, "source": "ntp"},
        "streams": [{"id": 1, "name": "accel", "rate_hz": 100, "dtype": "i24",
                     "channels": [{"name": n, "unit": "g", "scale": 1e-3, "offset": 0} for n in "xyz"]}],
    }
    if session:
        md["session"] = {"name": session, "source": "config"}
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as fh:
        w = DlogWriter(fh, md)
        w.data(1, np.full((100, 3), boot * 10 + index), index * 100, t0 + index * 1_000_000)
        w.end()


def test_session_name_from_metadata_and_folder(tmp_path):
    a = tmp_path / "logs" / "pump2" / "VL-0001_b000001_f0000.dlog"
    write(a, 1, 0, session="pump2")
    assert read_dlog(a).session_name == "pump2"
    assert read_metadata(a)["session"]["name"] == "pump2"

    old = tmp_path / "logs" / "9070692f3338" / "b000003_f0000.dlog"   # v1.0 layout: no session
    write(old, 3, 0)
    assert read_dlog(old).session_name is None

    copied = tmp_path / "jobs" / "fan-test" / "b000004_f0000.dlog"    # v1.0 file filed by hand
    write(copied, 4, 0)
    assert read_dlog(copied).session_name == "fan-test"


def test_find_sessions_groups_by_session_and_logger(tmp_path):
    root = tmp_path / "logs"
    write(root / "pump2" / "VL-0001_b000001_f0001.dlog", 1, 1, "pump2")
    write(root / "pump2" / "VL-0001_b000001_f0000.dlog", 1, 0, "pump2")
    write(root / "pump2" / "VL-0001_b000002_f0000.dlog", 2, 0, "pump2")       # same session after a reboot
    write(root / "pump2" / "VL-0002_b000009_f0000.dlog", 9, 0, "pump2", serial="VL-0002", dev="aaaaaaaaaaaa")
    write(root / "unnamed" / "VL-0001_b000005_f0000.dlog", 5, 0, None)
    write(root / "9070692f3338" / "b000006_f0000.dlog", 6, 0, None)
    (root / "junk.dlog").write_bytes(b"not a dlog")

    sessions = find_sessions(root)
    keys = [(s.name, s.logger) for s in sessions]
    assert keys == [("pump2", "VL-0001"), ("pump2", "VL-0002"), ("unnamed", "VL-0001")]
    s = sessions[0]
    assert s.boots == [1, 2]
    assert [p.name for p in s.files[1]] == ["VL-0001_b000001_f0000.dlog", "VL-0001_b000001_f0001.dlog"]
    assert sorted(p.name for p in sessions[2].files[5] + sessions[2].files[6]) == [
        "VL-0001_b000005_f0000.dlog", "b000006_f0000.dlog"]

    df = s.to_dataframe("accel")
    assert len(df) == 300 and list(df["boot"].unique()) == [1, 2]
    assert df.attrs["boots"] == [1, 2]
    assert str(df.index.tz) == "UTC"
