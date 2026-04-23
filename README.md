# OBS Input Logger

An OBS Studio plugin that records timestamped keyboard and mouse input events
to a JSONL file alongside every recording. The log is written on a background
thread with a bounded ring buffer, so capture has negligible impact on OBS
performance, and each recording gets its own uniquely-named log (never
overwrites prior runs).

## Schema

One JSON object per line. `t` is microseconds since the recording started.

    {"t": 1433000, "dev": "kb",    "type": "key",    "vk": "w",     "state": "down"}
    {"t": 2216523, "dev": "kb",    "type": "key",    "vk": "space", "state": "up"}
    {"t": 10057,   "dev": "mouse", "type": "move",   "dx": 5,   "dy": 0}
    {"t": 4820000, "dev": "mouse", "type": "button", "vk": "mouse_left", "state": "down"}
    {"t": 4900000, "dev": "mouse", "type": "wheel",  "dx": 0,   "dy": 1}

This matches `sample 1 input log.jsonl` exactly for `key` and `move` events;
`button` and `wheel` are additive extensions using the same envelope.

## Behavior

- When OBS starts a recording and the plugin is enabled (Tools menu toggle),
  input logging starts automatically.
- The log file is written next to the recording as
  `<video-basename>.inputlog.jsonl`. If that path already exists, `.1`, `.2`, …
  are appended so re-runs never overwrite.
- When the recording stops (or OBS exits), the writer thread drains the ring,
  flushes, and closes the file.
- Timestamps share a single monotonic clock (`os_gettime_ns`) started at
  `RECORDING_STARTED`, so events align with the video's t=0.

## Permissions

- **macOS**: On first run OBS will request *Input Monitoring* (and possibly
  *Accessibility*) permission. Grant both in System Settings → Privacy &
  Security. Without these the `CGEventTap` call fails and no events are
  emitted (OBS itself still records normally).
- **Windows**: Works out of the box; Low-Level hooks require no special
  permission but are incompatible with some anti-cheat software.
- **Linux**: Not implemented (see `src/hooks-linux.c`).

## Build

Follow the standard `obs-plugintemplate` flow — see
<https://obsproject.com/kb/developer-guide>. Summary:

    # macOS
    cmake --preset macos
    cmake --build --preset macos

    # Windows
    cmake --preset windows-x64
    cmake --build --preset windows-x64

The resulting `.plugin` / `.dll` goes into OBS's plugin directory as per the
template's `cpack` install step.

## Performance

- Hook callbacks perform one atomic read, one mutex-guarded ring push, and
  (every 64th event) a condvar signal. No allocations, no formatting, no I/O.
- A dedicated writer thread formats JSON and does buffered 64 KiB writes,
  waking at most every 50 ms.
- Ring buffer holds 262 144 events (~8 MiB). Overflow events are counted in
  the shutdown log line rather than blocking the hook.
