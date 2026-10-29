"""Tests for the DLOG reader/writer against protocol/FILE_FORMAT.md."""

import io
import struct

import numpy as np
import pytest

from logger_reader import DlogError, DlogFile, DlogWriter, Session
from logger_reader import dlog
from logger_reader.reader import utc_from_mono

RATE = 1000


def metadata(boot=7, index=0, synced=False, dtype="i24", mounting=None):
    accel = {
        "id": 1, "name": "accel", "feature": "accel", "rate_hz": RATE, "dtype": dtype,
        "raw_min": -524288, "raw_max": 524287,
        "channels": [{"name": n, "unit": "g", "scale": 1.5625e-5, "offset": 0} for n in "xyz"],
    }
    if mounting:
        accel["mounting"] = mounting
    md = {
        "format": "dlog", "format_version": "1.0",
        "device": {"id": "9070692f3338", "serial": "VL-0001", "type": "vibration",
                   "hardware": "TinyS3[D]", "firmware": "test"},
        "boot": {"id": boot}, "file": {"index": index},
        "time": {"synced": False, "mono_us": 0},
        "streams": [accel, {
            "id": 2, "name": "env", "feature": "env", "rate_hz": 1, "dtype": "f32",
            "channels": [{"name": "temperature", "unit": "degC", "scale": 1, "offset": 0}],
        }],
    }
    if synced:
        md["time"] = {"synced": True, "mono_us": 0, "utc_us": 1_791_270_000_000_000, "source": "ntp"}
    return md


def sine_blocks(n_blocks=4, frames=250, start_frame=0):
    """Known test signal: x = 10 Hz sine, y = ramp, z = constant 1 g (64000 counts)."""
    out = []
    for b in range(n_blocks):
        first = start_frame + b * frames
        i = np.arange(first, first + frames)
        x = np.round(100000 * np.sin(2 * np.pi * 10 * i / RATE)).astype(np.int64)
        y = (i % 1000) - 500
        z = np.full(frames, 64000)
        out.append((first, np.column_stack([x, y, z])))
    return out


def write_file(blocks=None, md=None, end=True, markers=()):
    buf = io.BytesIO()
    w = DlogWriter(buf, md or metadata())
    for first, frames in blocks if blocks is not None else sine_blocks():
        w.data(1, frames, first, first * 1000)  # 1000 Hz: frame n at n ms
    for t, body in markers:
        w.marker(t, body)
    if end:
        w.end()
    return buf.getvalue()


# ------------------------------------------------------------------ encoding

@pytest.mark.parametrize("dtype,values", [
    ("i24", [-(1 << 23), -1, 0, 1, (1 << 23) - 1, -524288, 524287]),
    ("i16", [-32768, -1, 0, 32767]),
    ("i32", [-(1 << 31), 0, (1 << 31) - 1]),
    ("f32", [-1.5, 0.0, 3.25]),
])
def test_sample_roundtrip(dtype, values):
    frames = np.array(values).reshape(-1, 1)
    out = dlog.decode_samples(dlog.encode_samples(frames, dtype), dtype, 1)
    np.testing.assert_array_equal(out, frames)


def test_i24_is_little_endian_packed():
    assert dlog.encode_samples(np.array([[0x123456]]), "i24") == b"\x56\x34\x12"
    assert dlog.encode_samples(np.array([[-2]]), "i24") == b"\xfe\xff\xff"


def test_record_layout_matches_spec():
    rec = dlog.encode_record(dlog.REC_END, b"")
    assert rec[:2] == b"\xa5\x5a"
    assert rec[2] == 0x7E and rec[3] == 0
    assert struct.unpack_from("<I", rec, 4)[0] == 0
    import zlib
    assert struct.unpack_from("<I", rec, 8)[0] == zlib.crc32(rec[2:8])
    assert dlog.encode_preamble() == b"\x89DLG\r\n\x1a\n\x01\x00\x01\x00\x00\x00\x00\x00"  # v1.1


# ------------------------------------------------------------------ reading

def test_roundtrip_values_and_times():
    f = DlogFile(write_file())
    assert f.issues.clean_end and f.issues.summary() == "none"
    assert f.device["serial"] == "VL-0001"
    sd = f.frames("accel")
    assert sd.n == 1000
    expected = np.concatenate([b for _, b in sine_blocks()])
    np.testing.assert_array_equal(sd.raw, expected)
    np.testing.assert_array_equal(sd.t_mono_us, np.arange(1000) * 1000)
    df = f.to_dataframe("accel")
    assert list(df.columns[:3]) == ["x", "y", "z"]
    assert df["z"].iloc[0] == pytest.approx(1.0)              # 64000 * 1.5625e-5
    assert df.index.name == "t_mono_s"                        # not synced
    assert df.attrs["units"]["x"] == "g"


def test_utc_index_when_synced():
    df = DlogFile(write_file(md=metadata(synced=True))).to_dataframe("accel")
    assert str(df.index.tz) == "UTC"
    assert df.index[0].value // 1000 == 1_791_270_000_000_000
    assert (df.index[1] - df.index[0]).value == 1_000_000     # 1 ms in ns


def test_time_sync_marker_backfills_earlier_samples():
    data = write_file(markers=[(500_000, {"type": "time_sync", "utc_us": 1_791_270_000_500_000})])
    df = DlogFile(data).to_dataframe("accel")
    assert df.index[0].value // 1000 == 1_791_270_000_000_000  # frame 0, before the sync


def test_drift_between_syncs_is_interpolated():
    syncs = [(0, 1_000_000), (10_000_000, 11_000_100)]          # clock lost 100 us over 10 s
    utc = utc_from_mono(np.array([0, 5_000_000, 10_000_000, 20_000_000]), syncs)
    assert list(utc) == [1_000_000, 6_000_050, 11_000_100, 21_000_100]


def test_truncated_file_keeps_complete_records():
    data = write_file()
    cut = data[: len(data) - 500]                               # power cut mid-block
    f = DlogFile(cut)
    assert not f.issues.clean_end
    assert f.issues.truncated_bytes > 0
    assert f.frames("accel").n == 750                           # last block lost


def test_corruption_mid_file_is_skipped():
    data = bytearray(write_file())
    second_block = data.index(b"\xa5\x5a\x02", data.index(b"\xa5\x5a\x02") + 1)
    data[second_block + 40] ^= 0xFF                             # flip a byte in block 2's samples
    f = DlogFile(bytes(data))
    assert f.issues.crc_errors == 1
    sd = f.frames("accel")
    assert sd.n == 750
    assert len(sd.gaps) == 1 and sd.gaps[0].missing == 250 and sd.gaps[0].at_frame == 500
    assert f.issues.clean_end                                    # END after the corruption still found


def test_gap_from_frame_counter():
    blocks = sine_blocks(2) + sine_blocks(1, start_frame=600)   # frames 500..599 never written
    sd = DlogFile(write_file(blocks)).frames("accel")
    assert [(g.at_frame, g.missing) for g in sd.gaps] == [(600, 100)]


def test_flags_are_counted():
    buf = io.BytesIO()
    w = DlogWriter(buf, metadata())
    (first, frames), = sine_blocks(1)
    w.data(1, frames, first, 0, flags=dlog.FLAG_CLIPPED | dlog.FLAG_SUSPECT)
    sd = DlogFile(buf.getvalue()).frames("accel")
    assert sd.clipped_blocks == 1 and sd.suspect_blocks == 1


def test_unknown_record_type_is_skipped():
    data = write_file(end=False) + dlog.encode_record(0x90, b"future") + dlog.encode_record(dlog.REC_END, b"")
    f = DlogFile(data)
    assert f.issues.unknown_records == 1 and f.issues.clean_end
    assert f.frames("accel").n == 1000


def test_multiple_streams_and_markers():
    buf = io.BytesIO()
    w = DlogWriter(buf, metadata())
    w.data(2, np.array([[21.5], [21.75]]), 0, 0)
    w.marker(123, {"type": "note", "text": "pump started"})
    f = DlogFile(buf.getvalue())
    assert list(f.to_dataframe("env")["temperature"]) == [21.5, 21.75]
    assert f.markers == [(123, {"type": "note", "text": "pump started"})]


def test_mounted_axes():
    md = metadata(mounting={"axis_map": [[0, 0, 1], [1, 0, 0], [0, 1, 0]],
                            "axis_names": ["vertical", "axial", "horizontal"]})
    df = DlogFile(write_file(md=md)).to_dataframe("accel", mounted=True)
    assert list(df.columns[:3]) == ["vertical", "axial", "horizontal"]
    assert df["vertical"].iloc[0] == pytest.approx(1.0)


def test_session_joins_files_and_shares_syncs():
    a = write_file(sine_blocks(2), md=metadata(index=0), end=True)
    b = write_file(sine_blocks(2, start_frame=500), md=metadata(index=1),
                   markers=[(900_000, {"type": "time_sync", "utc_us": 1_791_270_000_900_000})])
    s = Session([DlogFile(b), DlogFile(a)])                      # order doesn't matter
    df = s.to_dataframe("accel")
    assert len(df) == 1000 and df.attrs["gaps"] == []
    assert df.index[0].value // 1000 == 1_791_270_000_000_000   # file 0 timed by file 1's sync


def test_session_rejects_mixed_boots():
    with pytest.raises(ValueError):
        Session([DlogFile(write_file(md=metadata(boot=1))), DlogFile(write_file(md=metadata(boot=2)))])


def test_not_a_dlog_file():
    with pytest.raises(DlogError):
        DlogFile(b"hello world, this is not a dlog file")
