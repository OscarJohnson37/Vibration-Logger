"""Read DLOG files into pandas (see protocol/FILE_FORMAT.md).

    from logger_reader import read_dlog
    f = read_dlog("b000042_f0003.dlog")
    df = f.to_dataframe("accel")              # DatetimeIndex (UTC) if the clock was synced

Several files from the same boot (one recording split by file rolling) can be read together
with read_session(), which joins them and applies time syncs across all of them.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import numpy as np
import pandas as pd

from . import dlog


@dataclass
class Gap:
    stream: str
    at_frame: int        # first frame after the gap
    missing: int         # frames missing (negative = overlap/repeat)
    t_mono_us: int


@dataclass
class Issues:
    crc_errors: int = 0
    skipped_bytes: int = 0
    truncated_bytes: int = 0
    unknown_records: int = 0
    bad_blocks: int = 0
    clean_end: bool = False
    notes: list[str] = field(default_factory=list)

    def summary(self) -> str:
        parts = []
        if not self.clean_end:
            parts.append("no END record (not closed cleanly)")
        for name in ("crc_errors", "skipped_bytes", "truncated_bytes", "unknown_records", "bad_blocks"):
            v = getattr(self, name)
            if v:
                parts.append(f"{name.replace('_', ' ')}: {v}")
        return "; ".join(parts) if parts else "none"


@dataclass
class _Blocks:
    """All DATA blocks of one stream, in file order."""
    flags: list[int] = field(default_factory=list)
    n_frames: list[int] = field(default_factory=list)
    first_frame: list[int] = field(default_factory=list)
    t_mono_us: list[int] = field(default_factory=list)
    samples: list[np.ndarray] = field(default_factory=list)


class DlogFile:
    """One parsed DLOG file (or byte stream)."""

    def __init__(self, data: bytes, source: str = "<bytes>"):
        self.source = source
        stats = dlog.ScanStats()
        self.issues = Issues()
        self.metadata: dict = {}
        self.markers: list[tuple[int, dict]] = []
        self._blocks: dict[int, _Blocks] = {}
        self.version = dlog.check_preamble(data)

        for rec in dlog.iter_records(data, stats):
            if rec.type == dlog.REC_METADATA:
                if self.metadata:
                    self.issues.notes.append("extra METADATA record ignored")
                    continue
                self.metadata = dlog.parse_metadata(rec.payload)
            elif rec.type == dlog.REC_DATA:
                self._add_block(rec.payload)
            elif rec.type == dlog.REC_MARKER:
                self.markers.append(dlog.parse_marker(rec.payload))
            elif rec.type == dlog.REC_END:
                self.issues.clean_end = True
            else:
                stats.unknown_records += 1

        if not self.metadata:
            raise dlog.DlogError(f"{source}: no METADATA record")
        for k in ("crc_errors", "skipped_bytes", "truncated_bytes", "unknown_records"):
            setattr(self.issues, k, getattr(stats, k))
        self.issues.notes += stats.notes

    # ------------------------------------------------------------ parsing

    def _add_block(self, payload: memoryview) -> None:
        b = dlog.parse_data(payload)
        s = self.stream_by_id(b.stream_id) if self.metadata else None
        if s is None:
            self.issues.bad_blocks += 1
            self.issues.notes.append(f"DATA for unknown stream {b.stream_id}")
            return
        ch = len(s["channels"])
        size = dlog.dtype_size(s["dtype"])
        if len(b.samples) != b.n_frames * ch * size:
            self.issues.bad_blocks += 1
            self.issues.notes.append(f"DATA length mismatch in stream {s['name']}")
            return
        blk = self._blocks.setdefault(b.stream_id, _Blocks())
        blk.flags.append(b.flags)
        blk.n_frames.append(b.n_frames)
        blk.first_frame.append(b.first_frame)
        blk.t_mono_us.append(b.t_mono_us)
        blk.samples.append(dlog.decode_samples(b.samples, s["dtype"], ch))

    # ------------------------------------------------------------ metadata helpers

    @property
    def device(self) -> dict:
        return self.metadata.get("device", {})

    @property
    def boot_id(self) -> int | None:
        return self.metadata.get("boot", {}).get("id")

    @property
    def streams(self) -> list[dict]:
        return self.metadata.get("streams", [])

    def stream(self, name: str) -> dict:
        for s in self.streams:
            if s["name"] == name:
                return s
        raise KeyError(f"no stream {name!r}; streams: {[s['name'] for s in self.streams]}")

    def stream_by_id(self, sid: int) -> dict | None:
        return next((s for s in self.streams if s["id"] == sid), None)

    def time_syncs(self) -> list[tuple[int, int]]:
        """(t_mono_us, utc_us) pairs from the metadata and time_sync markers."""
        syncs = []
        t = self.metadata.get("time", {})
        if t.get("synced") and "utc_us" in t and "mono_us" in t:
            syncs.append((int(t["mono_us"]), int(t["utc_us"])))
        for mono, body in self.markers:
            if body.get("type") == "time_sync" and "utc_us" in body:
                syncs.append((int(mono), int(body["utc_us"])))
        return syncs

    # ------------------------------------------------------------ data access

    def frames(self, name: str) -> "StreamData":
        s = self.stream(name)
        blk = self._blocks.get(s["id"], _Blocks())
        return StreamData.from_blocks(s, [blk])

    def to_dataframe(self, name: str, **kwargs) -> pd.DataFrame:
        return self.frames(name).to_dataframe(self.time_syncs(), **kwargs)

    def summary(self) -> str:
        d = self.device
        lines = [
            f"{self.source}",
            f"  device   {d.get('id')}  serial={d.get('serial')}  name={d.get('name')}  type={d.get('type')}  fw={d.get('firmware')}",
            f"  boot     {self.boot_id}  file index {self.metadata.get('file', {}).get('index')}",
            f"  time     {'synced' if self.time_syncs() else 'NOT synced'} ({len(self.time_syncs())} sync points)",
        ]
        for s in self.streams:
            sd = self.frames(s["name"])
            dur = sd.n / s["rate_hz"] if s["rate_hz"] else 0
            lines.append(
                f"  stream   {s['name']:<10} {s['rate_hz']:>8g} Hz  {len(s['channels'])} ch  {s['dtype']}  "
                f"{sd.n} frames ({dur:.1f} s)  gaps={len(sd.gaps)}  clipped_blocks={sd.clipped_blocks}  "
                f"suspect_blocks={sd.suspect_blocks}"
            )
        lines.append(f"  markers  {len(self.markers)}")
        lines.append(f"  issues   {self.issues.summary()}")
        return "\n".join(lines)


@dataclass
class StreamData:
    """Frames of one stream with per-frame device timestamps."""
    stream: dict
    raw: np.ndarray                 # (n, channels) raw counts
    t_mono_us: np.ndarray           # (n,) device time of each frame
    frame_index: np.ndarray         # (n,) frame counter
    flags: np.ndarray               # (n,) block flags applying to each frame
    gaps: list[Gap]
    clipped_blocks: int
    suspect_blocks: int

    @property
    def n(self) -> int:
        return len(self.t_mono_us)

    @classmethod
    def from_blocks(cls, s: dict, blocks: Iterable[_Blocks]) -> "StreamData":
        rate = float(s["rate_hz"])
        ch = len(s["channels"])
        raws, times, idxs, flgs, gaps = [], [], [], [], []
        clipped = suspect = 0
        expected = None
        for blk in blocks:
            for flags, n, first, t0, samples in zip(blk.flags, blk.n_frames, blk.first_frame, blk.t_mono_us, blk.samples):
                if expected is not None and first != expected:
                    gaps.append(Gap(s["name"], first, first - expected, t0))
                expected = first + n
                raws.append(samples)
                times.append(t0 + np.round(np.arange(n) * 1e6 / rate).astype(np.int64))
                idxs.append(first + np.arange(n, dtype=np.int64))
                flgs.append(np.full(n, flags, dtype=np.uint16))
                clipped += bool(flags & dlog.FLAG_CLIPPED)
                suspect += bool(flags & dlog.FLAG_SUSPECT)
        if raws:
            raw = np.concatenate(raws)
            t = np.concatenate(times)
            idx = np.concatenate(idxs)
            fl = np.concatenate(flgs)
        else:
            raw = np.empty((0, ch))
            t = idx = np.empty(0, dtype=np.int64)
            fl = np.empty(0, dtype=np.uint16)
        return cls(s, raw, t, idx, fl, gaps, clipped, suspect)

    def scaled(self) -> np.ndarray:
        scale = np.array([c.get("scale", 1.0) for c in self.stream["channels"]], dtype=float)
        offset = np.array([c.get("offset", 0.0) for c in self.stream["channels"]], dtype=float)
        return self.raw * scale + offset

    def to_dataframe(self, syncs: list[tuple[int, int]] | None = None, *, scaled: bool = True,
                     mounted: bool = False, time: str = "auto", flags: bool = False) -> pd.DataFrame:
        """
        scaled:  engineering units (scale/offset applied) instead of raw counts
        mounted: apply the stream's mounting axis_map and axis_names
        time:    "auto" (UTC if synced, else seconds since boot), "utc", or "mono"
        flags:   add a 'flags' column with each frame's block flags
        """
        values = self.scaled() if scaled else self.raw
        names = [c["name"] for c in self.stream["channels"]]
        mount = self.stream.get("mounting") or {}
        if mounted and mount.get("axis_map"):
            m = np.asarray(mount["axis_map"], dtype=float)
            values = values @ m.T
            names = mount.get("axis_names") or [f"axis{i}" for i in range(m.shape[0])]
        df = pd.DataFrame(values, columns=names)
        df["t_mono_s"] = self.t_mono_us / 1e6
        if flags:
            df["flags"] = self.flags

        utc = utc_from_mono(self.t_mono_us, syncs or [])
        if time == "utc" and utc is None:
            raise ValueError("no time sync available; use time='mono'")
        if time in ("auto", "utc") and utc is not None:
            df.index = pd.to_datetime(utc, unit="us", utc=True)
            df.index.name = "time_utc"
        else:
            df.index = pd.Index(self.t_mono_us / 1e6, name="t_mono_s")
        df.attrs.update(
            stream=self.stream["name"],
            units={c["name"]: c.get("unit") for c in self.stream["channels"]},
            rate_hz=self.stream["rate_hz"],
            time_synced=utc is not None,
            gaps=[g.__dict__ for g in self.gaps],
            clipped_blocks=self.clipped_blocks,
            suspect_blocks=self.suspect_blocks,
        )
        return df


def utc_from_mono(t_mono_us: np.ndarray, syncs: list[tuple[int, int]]) -> np.ndarray | None:
    """Map device time to UTC using sync points (FILE_FORMAT.md, Time model).

    Before the first sync the first sync's offset applies (backfill); between syncs the offset is
    interpolated linearly (drift correction); after the last sync the last offset applies.
    """
    if not syncs:
        return None
    syncs = sorted(syncs)
    mono = np.array([s[0] for s in syncs], dtype=np.float64)
    offset = np.array([s[1] - s[0] for s in syncs], dtype=np.float64)
    return (t_mono_us + np.round(np.interp(t_mono_us, mono, offset))).astype(np.int64)


def read_dlog(path: str | Path) -> DlogFile:
    path = Path(path)
    return DlogFile(path.read_bytes(), str(path))


class Session:
    """Files from one device and boot, read as one continuous recording."""

    def __init__(self, files: list[DlogFile]):
        if not files:
            raise ValueError("no files")
        keys = {(f.device.get("id"), f.boot_id) for f in files}
        if len(keys) != 1:
            raise ValueError(f"files are from different devices/boots: {sorted(keys, key=str)}")
        self.files = sorted(files, key=lambda f: f.metadata.get("file", {}).get("index", 0))

    @property
    def device(self) -> dict:
        return self.files[0].device

    @property
    def markers(self) -> list[tuple[int, dict]]:
        return [m for f in self.files for m in f.markers]

    def time_syncs(self) -> list[tuple[int, int]]:
        return [s for f in self.files for s in f.time_syncs()]

    def to_dataframe(self, name: str, **kwargs) -> pd.DataFrame:
        s = self.files[0].stream(name)
        blocks = [f._blocks.get(f.stream(name)["id"], _Blocks()) for f in self.files]
        return StreamData.from_blocks(s, blocks).to_dataframe(self.time_syncs(), **kwargs)


def read_session(paths: Iterable[str | Path]) -> Session:
    return Session([read_dlog(p) for p in paths])
