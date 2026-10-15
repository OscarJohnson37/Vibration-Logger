# master

Python tools for the master PC. Currently the DLOG reader (`logger_reader`); the fleet service
(MQTT, dashboard, registry) joins it in phase 5.

## Setup

```powershell
cd master
pip install -e ".[capture,test]"     # numpy, pandas, pyserial, pytest
python -m pytest                      # run the tests
```

## Reading data

```python
from logger_reader import read_dlog, read_session

f = read_dlog("b000042_f0003.dlog")
print(f.summary())                     # device, streams, gaps, clipping, CRC errors…
df = f.to_dataframe("accel")           # UTC DatetimeIndex if the clock was synced
df = f.to_dataframe("accel", mounted=True)   # apply the mounting axis map
df.attrs["units"], df.attrs["gaps"]

# One recording split across several files (same device and boot):
s = read_session(sorted(Path("logs/9070692f3338").glob("b000042_*.dlog")))
df = s.to_dataframe("accel")
```

Files hold raw counts; `to_dataframe` applies scaling (`scaled=False` for raw counts), the
optional mounting axis map, and the time model from FILE_FORMAT.md (time syncs applied
backwards and interpolated for drift).

## Command line

```powershell
python -m logger_reader info FILE [FILE ...]                 # summaries
python -m logger_reader export FILE --stream accel --csv out.csv
python -m logger_reader capture COM22 test.dlog --seconds 30  # record a logger's USB stream
```

`capture` needs the logger to be recording with the `usb` sink (the phase-2 vibration firmware
does this by default, 10 s after power-on). Close any serial monitor first.
