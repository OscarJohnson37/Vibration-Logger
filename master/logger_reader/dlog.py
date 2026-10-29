"""Low-level DLOG v1 encoding and decoding (see protocol/FILE_FORMAT.md).

Everything here works on bytes: framing, CRCs, record payloads and sample packing.
reader.py builds the file/stream view on top of this.
"""

from __future__ import annotations

import json
import struct
import zlib
from dataclasses import dataclass, field
from typing import BinaryIO, Iterator

import numpy as np

MAGIC = b"\x89DLG\r\n\x1a\n"
VERSION_MAJOR = 1
VERSION_MINOR = 1  # 1.1: sessions
PREAMBLE = struct.Struct("<8sHHI")  # magic, major, minor, reserved
SYNC = b"\xa5\x5a"
RECORD_HEADER = struct.Struct("<2sBBI")  # sync, type, flags, length
CRC = struct.Struct("<I")
MAX_PAYLOAD = 1 << 20

REC_METADATA = 0x01
REC_DATA = 0x02
REC_MARKER = 0x03
REC_END = 0x7E

DATA_HEADER = struct.Struct("<HHIQQ")  # stream_id, flags, n_frames, first_frame, t_mono_us
MARKER_HEADER = struct.Struct("<Q")    # t_mono_us

FLAG_CLIPPED = 1 << 0
FLAG_GAP = 1 << 1
FLAG_SUSPECT = 1 << 2

# dtype name -> (bytes per value, numpy dtype for non-packed types)
DTYPES: dict[str, tuple[int, str | None]] = {
    "i16": (2, "<i2"),
    "i24": (3, None),
    "i32": (4, "<i4"),
    "u16": (2, "<u2"),
    "u32": (4, "<u4"),
    "f32": (4, "<f4"),
}


class DlogError(Exception):
    """The data is not a readable DLOG file."""


def crc32(data: bytes | memoryview) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


# ---------------------------------------------------------------- samples

def dtype_size(dtype: str) -> int:
    try:
        return DTYPES[dtype][0]
    except KeyError:
        raise DlogError(f"unknown dtype {dtype!r}") from None


def decode_samples(payload: bytes | memoryview, dtype: str, channels: int) -> np.ndarray:
    """Raw sample bytes -> array of shape (frames, channels)."""
    size, np_dtype = DTYPES[dtype]
    if np_dtype is not None:
        values = np.frombuffer(payload, dtype=np_dtype)
    else:  # i24: packed little-endian, sign-extended
        b = np.frombuffer(payload, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        values = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        values = (values ^ 0x800000) - 0x800000
    return values.reshape(-1, channels)


def encode_samples(frames: np.ndarray, dtype: str) -> bytes:
    """Array of shape (frames, channels) -> raw sample bytes."""
    size, np_dtype = DTYPES[dtype]
    flat = np.ascontiguousarray(frames).reshape(-1)
    if np_dtype is not None:
        return flat.astype(np_dtype).tobytes()
    v = flat.astype(np.int64)
    if v.size and (v.min() < -(1 << 23) or v.max() >= (1 << 23)):
        raise ValueError("value out of range for i24")
    v = (v & 0xFFFFFF).astype(np.uint32)
    out = np.empty((v.size, 3), dtype=np.uint8)
    out[:, 0] = v & 0xFF
    out[:, 1] = (v >> 8) & 0xFF
    out[:, 2] = (v >> 16) & 0xFF
    return out.tobytes()


# ---------------------------------------------------------------- records

def encode_record(rtype: int, payload: bytes) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    head = RECORD_HEADER.pack(SYNC, rtype, 0, len(payload))
    return head + payload + CRC.pack(crc32(head[2:] + payload))


def encode_preamble() -> bytes:
    return PREAMBLE.pack(MAGIC, VERSION_MAJOR, VERSION_MINOR, 0)


@dataclass
class Record:
    type: int
    offset: int
    payload: memoryview


@dataclass
class ScanStats:
    crc_errors: int = 0
    skipped_bytes: int = 0
    truncated_bytes: int = 0
    unknown_records: int = 0
    notes: list[str] = field(default_factory=list)


def check_preamble(buf: bytes | memoryview) -> tuple[int, int]:
    if len(buf) < PREAMBLE.size:
        raise DlogError("file too short for a DLOG preamble")
    magic, major, minor, _ = PREAMBLE.unpack_from(buf, 0)
    if magic != MAGIC:
        raise DlogError("not a DLOG file (bad magic bytes)")
    if major != VERSION_MAJOR:
        raise DlogError(f"unsupported DLOG major version {major}")
    return major, minor


def _record_at(buf: memoryview, pos: int) -> tuple[Record | None, str]:
    """Try to read one record at pos. Returns (record, "") or (None, reason)."""
    n = len(buf)
    if n - pos < RECORD_HEADER.size + CRC.size:
        return None, "short"
    sync, rtype, _flags, length = RECORD_HEADER.unpack_from(buf, pos)
    if sync != SYNC:
        return None, "sync"
    if length > MAX_PAYLOAD:
        return None, "length"
    end = pos + RECORD_HEADER.size + length + CRC.size
    if end > n:
        return None, "short"
    body = buf[pos + 2 : pos + RECORD_HEADER.size + length]
    (stored,) = CRC.unpack_from(buf, end - CRC.size)
    if crc32(body) != stored:
        return None, "crc"
    payload = buf[pos + RECORD_HEADER.size : pos + RECORD_HEADER.size + length]
    return Record(rtype, pos, payload), ""


def iter_records(buf: bytes | memoryview, stats: ScanStats | None = None) -> Iterator[Record]:
    """Yield valid records after the preamble, skipping corruption (FILE_FORMAT.md, Reading rules)."""
    stats = stats if stats is not None else ScanStats()
    buf = memoryview(buf)
    check_preamble(buf)
    pos = PREAMBLE.size
    n = len(buf)
    raw = bytes(buf)  # for fast sync searching
    while pos < n:
        rec, why = _record_at(buf, pos)
        if rec is not None:
            yield rec
            pos += RECORD_HEADER.size + len(rec.payload) + CRC.size
            continue
        if why == "crc":
            stats.crc_errors += 1
        # Resync: next sync marker that starts a valid record
        search = pos + 1
        while True:
            nxt = raw.find(SYNC, search)
            if nxt < 0:
                stats.truncated_bytes += n - pos
                return
            rec, _ = _record_at(buf, nxt)
            if rec is not None:
                stats.skipped_bytes += nxt - pos
                if why == "short":  # it wasn't the end after all: count it as corruption
                    stats.notes.append(f"corrupt record at offset {pos}")
                pos = nxt
                break
            search = nxt + 1


# ---------------------------------------------------------------- payloads

@dataclass
class DataBlock:
    stream_id: int
    flags: int
    n_frames: int
    first_frame: int
    t_mono_us: int
    samples: memoryview


def parse_data(payload: memoryview) -> DataBlock:
    if len(payload) < DATA_HEADER.size:
        raise DlogError("DATA record too short")
    sid, flags, n, first, t = DATA_HEADER.unpack_from(payload, 0)
    return DataBlock(sid, flags, n, first, t, payload[DATA_HEADER.size :])


def parse_marker(payload: memoryview) -> tuple[int, dict]:
    (t,) = MARKER_HEADER.unpack_from(payload, 0)
    return t, json.loads(bytes(payload[MARKER_HEADER.size :]).decode("utf-8"))


def parse_metadata(payload: memoryview) -> dict:
    return json.loads(bytes(payload).decode("utf-8"))


# ---------------------------------------------------------------- writer

class DlogWriter:
    """Writes a DLOG stream to any binary file object. Mirrors the firmware writer."""

    def __init__(self, fh: BinaryIO, metadata: dict):
        self.fh = fh
        self.metadata = metadata
        self._streams = {s["id"]: s for s in metadata.get("streams", [])}
        fh.write(encode_preamble())
        fh.write(encode_record(REC_METADATA, json.dumps(metadata, separators=(",", ":")).encode()))

    def data(self, stream_id: int, frames: np.ndarray, first_frame: int, t_mono_us: int, flags: int = 0) -> None:
        s = self._streams[stream_id]
        frames = np.asarray(frames)
        if frames.ndim != 2 or frames.shape[1] != len(s["channels"]):
            raise ValueError("frames must have shape (n, channels)")
        head = DATA_HEADER.pack(stream_id, flags, frames.shape[0], first_frame, t_mono_us)
        self.fh.write(encode_record(REC_DATA, head + encode_samples(frames, s["dtype"])))

    def marker(self, t_mono_us: int, body: dict) -> None:
        payload = MARKER_HEADER.pack(t_mono_us) + json.dumps(body, separators=(",", ":")).encode()
        self.fh.write(encode_record(REC_MARKER, payload))

    def end(self) -> None:
        self.fh.write(encode_record(REC_END, b""))
