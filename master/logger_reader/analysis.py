"""Analysis of long recordings: per-second summaries, whole-session overviews, raw windows, spectra.

A logger records 4000 Hz x 3 axes for days (one hour is ~14 M frames, 130 MB), far too much to
load at once. So each file is scanned once into a per-second summary (mean, AC RMS, min, max,
clipping per axis, plus where each second starts in the file), cached on this PC. Overviews of a
whole session come from the summaries; raw samples are read only for a window you zoom into,
straight from the right byte range of the right files.

    from logger_reader import find_sessions
    from logger_reader.analysis import SessionView, SummaryCache

    view = SessionView(find_sessions("E:/logs")[0], SummaryCache())
    view.prepare()                                   # first time: scans every file (cached after)
    ov = view.overview(view.t_start, view.t_end, bins=1000)
    win = view.window(t0, t0 + 60)                   # raw samples for one minute
    f, amp = amplitude_spectrum(win.values[:, 0], win.rate_hz)
"""

from __future__ import annotations

import hashlib
import json
import os
import threading
import zlib
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable

import numpy as np

from . import dlog
from .reader import DlogFile, SessionFiles, utc_from_mono

SUMMARY_VERSION = 1
MAX_WINDOW_S = 1800  # raw windows longer than this would need GBs of memory


# ------------------------------------------------------------------ fast scanning


class _Corrupt(Exception):
    pass


@dataclass
class StreamScan:
    """One stream's blocks and frames from a scan (or part of a file)."""
    stream: dict
    rec_pos: np.ndarray | None     # file offset of each DATA record (None: robust fallback)
    n_frames: np.ndarray
    t_mono_us: np.ndarray          # per block
    flags: np.ndarray              # per block
    raw: np.ndarray                # (frames, channels)

    def frame_times(self) -> np.ndarray:
        """Device time of every frame (µs)."""
        rate = float(self.stream["rate_hz"])
        n = self.n_frames.astype(np.int64)
        total = int(n.sum())
        if not total:
            return np.empty(0, dtype=np.int64)
        starts = np.repeat(np.cumsum(n) - n, n)
        within = np.arange(total, dtype=np.int64) - starts
        return np.repeat(self.t_mono_us, n) + np.round(within * (1e6 / rate)).astype(np.int64)

    def frame_flags(self) -> np.ndarray:
        return np.repeat(self.flags, self.n_frames.astype(np.int64))

    def scaled(self) -> np.ndarray:
        ch = self.stream["channels"]
        scale = np.array([c.get("scale", 1.0) for c in ch])
        offset = np.array([c.get("offset", 0.0) for c in ch])
        return self.raw * scale + offset


@dataclass
class Scan:
    metadata: dict
    markers: list[tuple[int, dict]]
    streams: dict[str, StreamScan]
    notes: list[str] = field(default_factory=list)

    def time_syncs(self) -> list[tuple[int, int]]:
        syncs = []
        t = self.metadata.get("time", {})
        if t.get("synced") and "utc_us" in t and "mono_us" in t:
            syncs.append((int(t["mono_us"]), int(t["utc_us"])))
        for mono, body in self.markers:
            if body.get("type") == "time_sync" and "utc_us" in body:
                syncs.append((int(mono), int(body["utc_us"])))
        return syncs


def _scan_records(buf: memoryview, pos: int, base: int, metadata: dict | None, end: int | None = None) -> Scan:
    """Sequential records from pos, assuming a healthy file (raises _Corrupt otherwise). base is
    buf's offset in the file, so rec_pos values are file offsets. A record cut off at the end
    (power cut, or the end of a byte range) is simply where the scan stops."""
    n = len(buf) if end is None else end
    unpack_rec = dlog.RECORD_HEADER.unpack_from
    unpack_data = dlog.DATA_HEADER.unpack_from
    unpack_crc = dlog.CRC.unpack_from
    crc32 = zlib.crc32
    sync_ok = dlog.SYNC
    hdr = dlog.RECORD_HEADER.size
    dh = dlog.DATA_HEADER.size
    per: dict[int, tuple[list, list, list, list, list, list]] = {}
    markers: list[tuple[int, dict]] = []
    while pos + hdr + 4 <= n:
        sync, rtype, _f, length = unpack_rec(buf, pos)
        if sync != sync_ok or length > dlog.MAX_PAYLOAD:
            raise _Corrupt(f"bad record at {base + pos}")
        stop = pos + hdr + length + 4
        if stop > n:
            break
        if crc32(buf[pos + 2:pos + hdr + length]) != unpack_crc(buf, stop - 4)[0]:
            raise _Corrupt(f"CRC error at {base + pos}")
        if rtype == dlog.REC_DATA:
            sid, fl, nf, _first, t = unpack_data(buf, pos + hdr)
            lists = per.get(sid)
            if lists is None:
                lists = per[sid] = ([], [], [], [], [], [])
            lists[0].append(base + pos)
            lists[1].append(nf)
            lists[2].append(t)
            lists[3].append(fl)
            lists[4].append(buf[pos + hdr + dh:pos + hdr + length])
        elif rtype == dlog.REC_MARKER:
            markers.append(dlog.parse_marker(buf[pos + hdr:pos + hdr + length]))
        elif rtype == dlog.REC_METADATA and metadata is None:
            metadata = dlog.parse_metadata(buf[pos + hdr:pos + hdr + length])
        pos = stop
    if metadata is None:
        raise _Corrupt("no METADATA")
    streams: dict[str, StreamScan] = {}
    for s in metadata.get("streams", []):
        lists = per.get(s["id"])
        ch = len(s["channels"])
        if not lists:
            streams[s["name"]] = StreamScan(s, np.empty(0, np.int64), np.empty(0, np.int64), np.empty(0, np.int64),
                                            np.empty(0, np.uint16), np.empty((0, ch)))
            continue
        raw = dlog.decode_samples(b"".join(lists[4]), s["dtype"], ch)
        streams[s["name"]] = StreamScan(s, np.array(lists[0], dtype=np.int64), np.array(lists[1], dtype=np.int64),
                                        np.array(lists[2], dtype=np.int64), np.array(lists[3], dtype=np.uint16), raw)
    return Scan(metadata, markers, streams)


def scan_file(path: str | Path) -> Scan:
    """Whole file: the fast path, or the robust reader (resyncs past corruption) if it isn't clean."""
    data = Path(path).read_bytes()
    buf = memoryview(data)
    dlog.check_preamble(buf)
    try:
        return _scan_records(buf, dlog.PREAMBLE.size, 0, None)
    except _Corrupt as e:
        f = DlogFile(data, str(path))
        streams = {}
        for s in f.streams:
            blk = f._blocks.get(s["id"])
            ch = len(s["channels"])
            if blk is None or not blk.samples:
                raw = np.empty((0, ch))
                arrays = [np.empty(0, np.int64)] * 2 + [np.empty(0, np.uint16)]
            else:
                raw = np.concatenate(blk.samples)
                arrays = [np.array(blk.n_frames, np.int64), np.array(blk.t_mono_us, np.int64),
                          np.array(blk.flags, np.uint16)]
            streams[s["name"]] = StreamScan(s, None, arrays[0], arrays[1], arrays[2], raw)
        return Scan(f.metadata, f.markers, streams, [f"robust read: {e}; {f.issues.summary()}"])


def scan_range(path: str | Path, start: int, stop: int, metadata: dict) -> Scan:
    """Records in the byte range [start, stop) of a file (start must be a record start)."""
    with open(path, "rb") as fh:
        fh.seek(start)
        data = fh.read(max(0, stop - start))
    return _scan_records(memoryview(data), 0, start, metadata)


# ------------------------------------------------------------------ per-file summaries


def _reduce(values: np.ndarray, starts: np.ndarray) -> dict[str, np.ndarray]:
    """Per-bucket stats of one channel (buckets start at `starts`, frames in time order)."""
    v = values.astype(np.float64)
    n = np.diff(np.append(starts, len(v)))
    s1 = np.add.reduceat(v, starts)
    mean = s1 / n
    # AC RMS from the centred sum of squares (stable even with a 1 g offset)
    centred = v - np.repeat(mean, n)
    var = np.add.reduceat(centred * centred, starts) / n
    return {"mean": mean, "rms": np.sqrt(var), "min": np.minimum.reduceat(v, starts),
            "max": np.maximum.reduceat(v, starts)}


def summarise_file(path: str | Path, stream: str = "accel") -> tuple[dict, dict[str, np.ndarray]]:
    """(meta, arrays): per-second summary of one file. Arrays are indexed by device second."""
    scan = scan_file(path)
    md = scan.metadata
    st = path_stat(path)
    meta: dict = {
        "version": SUMMARY_VERSION, "path": str(Path(path).resolve()), "size": st[0], "mtime": st[1],
        "metadata": md, "markers": scan.markers, "syncs": scan.time_syncs(), "notes": scan.notes,
        "boot": md.get("boot", {}).get("id"), "index": md.get("file", {}).get("index", 0),
        "stream": None, "frames": 0, "gaps": 0, "clipped_blocks": 0,
    }
    arrays: dict[str, np.ndarray] = {}

    bat = scan.streams.get("battery")
    if bat is not None and len(bat.raw):
        arrays["battery_t"] = bat.frame_times()
        arrays["battery"] = bat.scaled().astype(np.float32)
        meta["battery_channels"] = [c["name"] for c in bat.stream["channels"]]

    s = scan.streams.get(stream)
    if s is None or not len(s.raw):
        return meta, arrays
    meta["stream"] = s.stream
    meta["frames"] = int(len(s.raw))
    meta["clipped_blocks"] = int(np.count_nonzero(s.flags & dlog.FLAG_CLIPPED))
    meta["gaps"] = int(np.count_nonzero(s.flags & dlog.FLAG_GAP))
    meta["indexed"] = s.rec_pos is not None

    t = s.frame_times()
    sec = t // 1_000_000
    starts = np.concatenate([[0], np.flatnonzero(np.diff(sec)) + 1])
    arrays["sec"] = sec[starts]
    arrays["n"] = np.diff(np.append(starts, len(sec))).astype(np.int32)
    vals = s.scaled()
    for c in range(vals.shape[1]):
        r = _reduce(vals[:, c], starts)
        for k, v in r.items():
            arrays[f"{k}{c}"] = v.astype(np.float32)
    lo, hi = s.stream.get("raw_min"), s.stream.get("raw_max")
    if lo is not None and hi is not None:
        clip = ((s.raw <= lo) | (s.raw >= hi)).any(axis=1).astype(np.int32)
        arrays["clipped"] = np.add.reduceat(clip, starts).astype(np.int32)
    gapf = (s.frame_flags() & dlog.FLAG_GAP) != 0
    arrays["gap"] = np.maximum.reduceat(gapf.astype(np.int8), starts)
    if s.rec_pos is not None:
        # Byte offset where each second starts: the record holding its first frame
        block_of_frame = np.searchsorted(np.cumsum(s.n_frames), starts, side="right")
        arrays["offset"] = s.rec_pos[block_of_frame]
        meta["file_end"] = st[0]
    return meta, arrays


def path_stat(path: str | Path) -> tuple[int, float]:
    st = os.stat(path)
    return st.st_size, st.st_mtime


# ------------------------------------------------------------------ cache


def default_cache_dir() -> Path:
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "logger-reader" / "summaries"


class SummaryCache:
    """Per-file summaries on this PC, keyed by the file's path, size and modification time."""

    def __init__(self, folder: str | Path | None = None):
        self.folder = Path(folder) if folder else default_cache_dir()

    def _key(self, path: str | Path) -> str:
        size, mtime = path_stat(path)
        h = hashlib.sha1(f"{Path(path).resolve()}|{size}|{mtime:.3f}|{SUMMARY_VERSION}".encode()).hexdigest()
        return h[:24]

    def has(self, path: str | Path) -> bool:
        try:
            k = self._key(path)
        except OSError:  # unreadable file (damaged card): never cached
            return False
        return (self.folder / f"{k}.json").exists() and (self.folder / f"{k}.npz").exists()

    def load(self, path: str | Path) -> tuple[dict, dict[str, np.ndarray]] | None:
        try:
            k = self._key(path)
            meta = json.loads((self.folder / f"{k}.json").read_text(encoding="utf-8"))
            with np.load(self.folder / f"{k}.npz") as z:
                arrays = {n: z[n] for n in z.files}
            return meta, arrays
        except (OSError, ValueError):
            return None

    def store(self, path: str | Path, meta: dict, arrays: dict[str, np.ndarray]) -> None:
        self.folder.mkdir(parents=True, exist_ok=True)
        k = self._key(path)
        np.savez(self.folder / f"{k}.npz", **arrays)
        tmp = self.folder / f"{k}.json.tmp"
        tmp.write_text(json.dumps(meta), encoding="utf-8")
        os.replace(tmp, self.folder / f"{k}.json")


def _summarise_to_cache(args: tuple[str, str, str]) -> tuple[str, str | None]:
    path, cache_dir, stream = args
    try:
        meta, arrays = summarise_file(path, stream)
        SummaryCache(cache_dir).store(path, meta, arrays)
        return path, None
    except Exception as e:  # a damaged file mustn't stop the rest of the session
        return path, f"{type(e).__name__}: {e}"


def summarise_all(paths: Iterable[str | Path], cache: SummaryCache, stream: str = "accel",
                  workers: int | None = None,
                  progress: Callable[[int, int, str], None] | None = None) -> dict[str, str]:
    """Summarise every file not already cached, in parallel processes. Returns the files that
    couldn't be read ({path: error}); the rest are summarised regardless."""
    todo = [str(p) for p in paths if not cache.has(p)]
    total = len(todo)
    errors: dict[str, str] = {}
    if progress:
        progress(0, total, "")
    if not todo:
        return errors
    workers = workers or max(1, min(8, (os.cpu_count() or 2) - 1, total))
    done = 0
    if workers == 1:
        results = (_summarise_to_cache((p, str(cache.folder), stream)) for p in todo)
        for p, err in results:
            done += 1
            if err:
                errors[p] = err
            if progress:
                progress(done, total, p)
        return errors
    with ProcessPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(_summarise_to_cache, (p, str(cache.folder), stream)) for p in todo]
        for fut in as_completed(futures):
            p, err = fut.result()
            done += 1
            if err:
                errors[p] = err
            if progress:
                progress(done, total, p)
    return errors


# ------------------------------------------------------------------ whole session


@dataclass
class Window:
    """Raw samples for a time range, in engineering units."""
    t: np.ndarray            # seconds (UTC epoch, or the session's timeline when unsynced)
    values: np.ndarray       # (frames, channels)
    clipped: np.ndarray      # (frames,) bool
    rate_hz: float
    channels: list[str]
    unit: str


class SessionView:
    """One logger's session over all its boots, from cached per-file summaries."""

    def __init__(self, session: SessionFiles, cache: SummaryCache, stream: str = "accel"):
        self.session = session
        self.cache = cache
        self.stream_name = stream
        self.paths = [p for b in session.boots for p in session.files[b]]
        self._ready = False
        self._lock = threading.Lock()
        # Files that couldn't be read: {path: why}. Ones whose header couldn't even be read
        # (find_sessions) are known up front.
        self.unreadable: dict[str, str] = {str(p): "unreadable (damaged storage?)" for p in session.unreadable}

    # ---- preparing

    def missing(self) -> list[Path]:
        return [p for p in self.paths if str(p) not in self.unreadable and not self.cache.has(p)]

    def prepare(self, progress: Callable[[int, int, str], None] | None = None, workers: int | None = None) -> None:
        self.unreadable.update(summarise_all(self.paths, self.cache, self.stream_name, workers, progress))
        self.ensure()

    def _load(self) -> None:
        files = []
        for p in self.paths:
            got = self.cache.load(p)
            if got is None:
                self.unreadable.setdefault(str(p), "not summarised (unreadable)")
                continue
            files.append(got)
        if not files:
            raise RuntimeError("no readable files in this session")
        self.files = files
        stream = next((m["stream"] for m, _ in files if m.get("stream")), None)
        if stream is None:
            raise RuntimeError(f"no {self.stream_name!r} data in this session")
        self.stream = stream
        self.rate_hz = float(stream["rate_hz"])
        self.channels = [c["name"] for c in stream["channels"]]
        self.unit = stream["channels"][0].get("unit", "")

        # Each boot's device clock maps to UTC through all of that boot's time syncs. A boot
        # with none is placed straight after the previous one, on the session's own timeline.
        by_boot: dict[int, list[int]] = {}
        for i, (m, _) in enumerate(files):
            by_boot.setdefault(m["boot"], []).append(i)
        self.boots: list[dict] = []
        self.battery = {"t": [], "values": [], "channels": []}
        self.markers: list[dict] = []
        cols: dict[str, list[np.ndarray]] = {}
        # Columns every file contributes (a file read by the robust fallback has no byte offsets)
        defaults = {"offset": -1, "clipped": 0, "gap": 0}
        names = sorted({k for _, a in files for k in a if k not in ("sec", "battery", "battery_t")} | set(defaults))
        cursor = None
        for boot in sorted(by_boot):
            idx = by_boot[boot]
            syncs = [tuple(s) for i in idx for s in files[i][0]["syncs"]]
            boot_t: list[np.ndarray] = []
            for i in idx:
                m, a = files[i]
                if "sec" not in a:
                    continue
                mono_us = a["sec"].astype(np.int64) * 1_000_000
                utc = utc_from_mono(mono_us, syncs) if syncs else None
                boot_t.append(utc / 1e6 if utc is not None else a["sec"].astype(np.float64))
            if not boot_t:
                continue
            t_all = np.concatenate(boot_t)
            shift = 0.0
            if not syncs:
                shift = (cursor if cursor is not None else 0.0) - t_all[0]
            k = 0
            for i in idx:
                m, a = files[i]
                if "sec" not in a:
                    continue
                t = boot_t[k] + shift
                k += 1
                cols.setdefault("t", []).append(t)
                cols.setdefault("file", []).append(np.full(len(t), i, dtype=np.int32))
                for name in names:
                    col = a.get(name)
                    if col is None:
                        col = np.full(len(t), defaults.get(name, np.nan))
                    cols.setdefault(name, []).append(col)
                cols.setdefault("sec", []).append(a["sec"])
            self.boots.append({"boot": boot, "t0": float(t_all[0] + shift), "t1": float(t_all[-1] + shift + 1),
                               "synced": bool(syncs), "files": len(idx)})
            cursor = float(t_all[-1] + shift + 1)
            self._battery_and_markers(boot, idx, syncs, shift)
        self.cols = {k: np.concatenate(v) for k, v in cols.items()}
        order = np.argsort(self.cols["t"], kind="stable")
        self.cols = {k: v[order] for k, v in self.cols.items()}
        self.t_start = float(self.cols["t"][0])
        self.t_end = float(self.cols["t"][-1] + 1)
        self._ready = True

    def _battery_and_markers(self, boot: int, idx: list[int], syncs: list, shift: float) -> None:
        for i in idx:
            m, a = self.files[i]
            if "battery_t" in a:
                utc = utc_from_mono(a["battery_t"], syncs) if syncs else None
                t = (utc / 1e6 if utc is not None else a["battery_t"] / 1e6) + shift
                self.battery["t"].append(t)
                self.battery["values"].append(a["battery"])
                self.battery["channels"] = m.get("battery_channels", [])
            for mono, body in m["markers"]:
                utc = utc_from_mono(np.array([mono]), syncs) if syncs else None
                t = (utc[0] / 1e6 if utc is not None else mono / 1e6) + shift
                self.markers.append({"t": float(t), "boot": boot, **body})

    @property
    def ready(self) -> bool:
        return self._ready

    def ensure(self) -> None:
        with self._lock:  # the scan finishing and a page request can both get here
            if not self._ready:
                self._load()

    def info(self) -> dict:
        self.ensure()
        c = self.cols
        return {
            "t_start": self.t_start, "t_end": self.t_end, "rate_hz": self.rate_hz, "channels": self.channels,
            "unit": self.unit, "boots": self.boots, "frames": int(c["n"].sum()),
            "seconds": int(len(c["t"])), "clipped_frames": int(c["clipped"].sum()) if "clipped" in c else 0,
            "gap_seconds": int(c["gap"].sum()) if "gap" in c else 0,
            "markers": [m for m in self.markers if m.get("type") not in ("time_sync",)],
            "range_g": (self.stream.get("sensor") or {}).get("range_g"),
            "notes": [n for m, _ in self.files for n in m.get("notes", [])],
            "unreadable": [{"file": Path(p).name, "error": e} for p, e in sorted(self.unreadable.items())],
        }

    # ---- overview from summaries

    def overview(self, t0: float, t1: float, bins: int = 1000) -> dict:
        """Per-bin values over [t0, t1): rms (energy average) and rms_max, peak (largest
        deviation from the mean), mean (gravity), min/max, clipped frames. Empty bins are null."""
        self.ensure()
        c = self.cols
        bins = int(max(10, min(bins, 5000)))
        sel = (c["t"] >= t0) & (c["t"] < t1)
        t = c["t"][sel]
        width = (t1 - t0) / bins
        b = np.minimum(((t - t0) / width).astype(np.int64), bins - 1)
        n = c["n"][sel].astype(np.float64)
        count = np.bincount(b, weights=n, minlength=bins)
        has = count > 0
        out: dict = {"t0": t0, "t1": t1, "bins": bins, "width_s": width,
                     "t": (t0 + (np.arange(bins) + 0.5) * width).tolist(), "channels": {}}

        def nan_list(a):
            return [None if not h else float(v) for v, h in zip(a, has)]

        for ci, name in enumerate(self.channels):
            mean = c[f"mean{ci}"][sel].astype(np.float64)
            rms = c[f"rms{ci}"][sel].astype(np.float64)
            lo = c[f"min{ci}"][sel].astype(np.float64)
            hi = c[f"max{ci}"][sel].astype(np.float64)
            wmean = np.bincount(b, weights=mean * n, minlength=bins) / np.where(has, count, 1)
            energy = np.bincount(b, weights=rms * rms * n, minlength=bins) / np.where(has, count, 1)
            peak = np.maximum(hi - mean, mean - lo)
            pk = np.full(bins, -np.inf)
            np.maximum.at(pk, b, peak)
            rmax = np.full(bins, -np.inf)
            np.maximum.at(rmax, b, rms)
            mn = np.full(bins, np.inf)
            np.minimum.at(mn, b, lo)
            mx = np.full(bins, -np.inf)
            np.maximum.at(mx, b, hi)
            out["channels"][name] = {"rms": nan_list(np.sqrt(energy)), "rms_max": nan_list(rmax),
                                     "peak": nan_list(pk), "mean": nan_list(wmean), "min": nan_list(mn),
                                     "max": nan_list(mx)}
        if "clipped" in c:
            out["clipped"] = np.bincount(b, weights=c["clipped"][sel], minlength=bins).astype(int).tolist()
        if self.battery["t"]:
            bt = np.concatenate(self.battery["t"])
            bv = np.concatenate(self.battery["values"])
            keep = (bt >= t0) & (bt < t1)
            step = max(1, int(keep.sum() // 2000))
            out["battery"] = {"t": bt[keep][::step].tolist(), "channels": self.battery["channels"],
                              "values": bv[keep][::step].tolist()}
        out["boots"] = [bt for bt in self.boots if bt["t1"] > t0 and bt["t0"] < t1]
        out["markers"] = [m for m in self.markers if t0 <= m["t"] < t1 and m.get("type") != "time_sync"]
        return out

    # ---- raw samples

    def window(self, t0: float, t1: float) -> Window:
        """Raw samples in [t0, t1), read from just the byte ranges that hold them."""
        self.ensure()
        if t1 - t0 > MAX_WINDOW_S:
            raise ValueError(f"window too long ({t1 - t0:.0f} s); at most {MAX_WINDOW_S} s")
        c = self.cols
        sel = np.flatnonzero((c["t"] >= t0 - 1) & (c["t"] < t1))
        parts_t, parts_v, parts_c = [], [], []
        for fi in np.unique(c["file"][sel]):
            rows = sel[c["file"][sel] == fi]
            meta, arrays = self.files[fi]
            boot_files = [i for i, (m, _) in enumerate(self.files) if m["boot"] == meta["boot"]]
            syncs = [tuple(s) for i in boot_files for s in self.files[i][0]["syncs"]]
            shift = 0.0
            if not syncs:
                shift = next(b["t0"] for b in self.boots if b["boot"] == meta["boot"]) - \
                    float(min(self.files[i][1]["sec"][0] for i in boot_files if "sec" in self.files[i][1]))
            if "offset" in c and meta.get("indexed"):
                start = int(c["offset"][rows[0]])
                # A block can straddle seconds: read up to the start of the second after next
                after = rows[-1] + 2
                same_file_after = after < len(c["t"]) and c["file"][after] == fi
                stop = int(c["offset"][after]) if same_file_after else int(meta["file_end"])
                scan = scan_range(meta["path"], start, stop, meta["metadata"])
            else:
                scan = scan_file(meta["path"])
            s = scan.streams[self.stream_name]
            tm = s.frame_times()
            utc = utc_from_mono(tm, syncs) if syncs else None
            t = (utc / 1e6 if utc is not None else tm / 1e6) + shift
            keep = (t >= t0) & (t < t1)
            parts_t.append(t[keep])
            parts_v.append(s.scaled()[keep])
            lo, hi = s.stream.get("raw_min"), s.stream.get("raw_max")
            if lo is not None and hi is not None:
                parts_c.append(((s.raw[keep] <= lo) | (s.raw[keep] >= hi)).any(axis=1))
            else:
                parts_c.append(np.zeros(int(keep.sum()), dtype=bool))
        if not parts_t:
            return Window(np.empty(0), np.empty((0, len(self.channels))), np.empty(0, bool), self.rate_hz,
                          self.channels, self.unit)
        t = np.concatenate(parts_t)
        order = np.argsort(t, kind="stable")
        return Window(t[order], np.concatenate(parts_v)[order], np.concatenate(parts_c)[order], self.rate_hz,
                      self.channels, self.unit)


# ------------------------------------------------------------------ signal processing


def window_stats(w: Window) -> dict:
    """Per channel: mean, AC RMS, peak (largest deviation from the mean), crest factor, min, max."""
    out = {}
    for ci, name in enumerate(w.channels):
        x = w.values[:, ci].astype(np.float64)
        if not len(x):
            continue
        mean = float(x.mean())
        ac = x - mean
        rms = float(np.sqrt(np.mean(ac * ac)))
        peak = float(np.max(np.abs(ac)))
        out[name] = {"mean": mean, "rms": rms, "peak": peak, "crest": peak / rms if rms else None,
                     "min": float(x.min()), "max": float(x.max())}
    out["_frames"] = int(len(w.t))
    out["_clipped_frames"] = int(w.clipped.sum())
    return out


def decimate_minmax(t: np.ndarray, x: np.ndarray, points: int) -> tuple[list, list, list]:
    """For plotting: per pixel column, the min and max (so spikes survive)."""
    n = len(x)
    if n <= points * 2:
        return t.tolist(), x.tolist(), x.tolist()
    edges = np.linspace(0, n, points + 1).astype(np.int64)
    starts = edges[:-1]
    return (t[starts].tolist(), np.minimum.reduceat(x, starts).tolist(), np.maximum.reduceat(x, starts).tolist())


def amplitude_spectrum(x: np.ndarray, rate_hz: float, nfft: int = 8192) -> tuple[np.ndarray, np.ndarray]:
    """Welch-averaged amplitude spectrum (Hann window, 50 % overlap): the amplitude of a sine at
    each frequency, in the signal's units (a 1 g sine at 50 Hz shows 1 g at 50 Hz)."""
    x = np.asarray(x, dtype=np.float64)
    x = x - x.mean()
    nfft = int(min(nfft, 1 << int(np.floor(np.log2(max(len(x), 16))))))
    win = np.hanning(nfft)
    step = nfft // 2
    segs = max(1, (len(x) - nfft) // step + 1)
    acc = np.zeros(nfft // 2 + 1)
    for k in range(segs):
        seg = x[k * step:k * step + nfft]
        if len(seg) < nfft:
            break
        acc += np.abs(np.fft.rfft((seg - seg.mean()) * win)) ** 2
    amp = 2 * np.sqrt(acc / segs) / win.sum()
    freqs = np.fft.rfftfreq(nfft, 1 / rate_hz)
    return freqs, amp


def spectrogram(x: np.ndarray, rate_hz: float, nfft: int = 1024, max_cols: int = 600) -> dict:
    """Amplitude (dB re 1 unit) over time and frequency, at most max_cols time columns."""
    x = np.asarray(x, dtype=np.float64)
    nfft = int(min(nfft, 1 << int(np.floor(np.log2(max(len(x), 16))))))
    win = np.hanning(nfft)
    total = max(1, (len(x) - nfft) // (nfft // 4) + 1)
    hop = max(nfft // 4, (len(x) - nfft) // max_cols if len(x) > nfft else nfft)
    starts = np.arange(0, max(1, len(x) - nfft + 1), hop)[:max_cols]
    cols = []
    for s in starts:
        seg = x[s:s + nfft]
        if len(seg) < nfft:
            break
        amp = 2 * np.abs(np.fft.rfft((seg - seg.mean()) * win)) / win.sum()
        cols.append(20 * np.log10(np.maximum(amp, 1e-7)))
    db = np.array(cols) if cols else np.zeros((0, nfft // 2 + 1))
    return {"db": db, "freq_max": rate_hz / 2, "col_starts": starts[:len(cols)] / rate_hz, "nfft": nfft,
            "columns_available": int(total)}
