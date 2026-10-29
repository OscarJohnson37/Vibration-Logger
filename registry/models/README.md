# Model catalogue

One page per logger model: what it is, its LED guide, how to use and set up a unit, its pin map
and troubleshooting. The master dashboard shows a logger's model page on its **About** tab, and
uses the model's `expected_features` to flag units that are missing something.

- File name: `<model>.md`, matching the model number the firmware reports (`info.device.model`,
  e.g. `VL-P1.md`).
- Starts with YAML front matter between `---` lines:

```yaml
---
model: VL-P1                      # required; same as the file name
name: Portable vibration logger
type: vibration                   # logger type (info.device.type)
hardware_revs: [A]
description: One sentence for lists.
expected_features: [core, recording, sd, battery, accel, wifi, publish]   # feature instances
specs: { controller: ..., accelerometer: ... }                            # free-form key: value
---
```

- The rest is Markdown (headings, tables, lists, code blocks), written for the people using and
  maintaining the units.
- The master re-reads a page when the file changes; no restart needed.
- A new model: copy an existing page, change the front matter, and set the model number in that
  logger type's firmware (`app.setModel("VL-P2", "A")`).
