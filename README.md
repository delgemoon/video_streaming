# VSP — Video Streaming Platform

High-performance, low-latency video platform for Ubuntu:

- **Backend** — C++23, [GStreamer](https://gstreamer.freedesktop.org/) 1.20+ media core, [Boost.Asio/Beast](https://www.boost.org/doc/libs/release/libs/beast/) coroutine HTTP server, SQLite metadata.
- **Desktop app** — Electron + React + TypeScript (electron-vite), [hls.js](https://github.com/video-dev/hls.js) player.
- **Streaming format** — **LL-HLS / CMAF** (fragmented MP4, byte-range partial segments). Content is playable *while it is still being transcoded*.

```
┌──────────────── desktop (Electron) ────────────────┐      ┌─────────────────── backend (C++) ───────────────────┐
│ Upload wizard: probe · trim · poster · ladder      │ PUT  │ Beast HTTP ── streaming upload ──► uploads/<id>/     │
│   └─ XHR streams file from disk (progress)   ──────┼─────►│   └─ JobQueue ─► GStreamer pipeline                   │
│ Library (SSE live updates)                   ◄─────┼─ SSE │        uridecodebin3 ─► tee ─┬► scale ─► enc ─► mp4mux ─► CmafPackager ─► v_1080p/
│ Player (hls.js, LL-HLS, level switch, stats) ◄─────┼─ GET │                             ├► …                              v_720p/ …
└────────────────────────────────────────────────────┘      │                             └► aac ───────────────────────► a_128k/
                                                            │ Range-capable static serving of media/<id>/*.m3u8|m4s │
                                                            └──────────────────────────────────────────────────────┘
```

## Features

**Backend**
- Single-pass **ABR ladder** transcode: one decode, N parallel encoders (2160p→240p, capped at the source height), separate AAC audio rendition (CMAF-style audio group).
- **Encoder auto-detection** by actually test-running each candidate: `nvh264enc` (NVENC) → `vah264enc`/`vaapih264enc` (VA-API) → `qsvh264enc` → `x264enc`. Falls back automatically; `--encoder x264` forces software.
- Custom **CMAF packager** (ISOBMFF parser in `src/media/mp4_box.*`): splits `moof+mdat` fragments into LL-HLS **parts** (`#EXT-X-PART` byte-ranges), groups them into 2 s keyframe-aligned segments, writes `init.mp4`, and rewrites playlists atomically after every fragment (`EVENT` → `VOD` with `#EXT-X-ENDLIST` on completion). Codec strings (`avc1.PPCCLL`, `mp4a.40.2`) and `BANDWIDTH`/`AVERAGE-BANDWIDTH` are derived from the actual output.
- **Trim** (in/out points) applied with an accurate source seek before any data reaches the muxers.
- Client-provided or auto-generated **poster thumbnails**.
- **Streaming uploads**: request bodies are written to disk in 1 MiB chunks, never buffered in memory; `Expect: 100-continue` supported.
- **Range requests** (RFC 7233) for segments, proper MIME types, HLS-aware cache headers, CORS.
- **Server-Sent Events** (`/api/events`) for job progress / video state; crash recovery re-queues interrupted jobs at startup.
- Configurable through JSON file, CLI flags and `VSP_*` environment variables.

**Desktop**
- Drag-and-drop upload wizard with **client-side pre-processing**: metadata probe, scrub-and-capture poster frame, trim in/out with range-bound preview, rendition selection, audio bitrate.
- Library with thumbnails, live progress bars and a **LIVE PREVIEW** badge the moment a transcode becomes playable.
- hls.js player in low-latency mode: automatic/manual quality, stats overlay (level, bandwidth estimate, buffer, LL-HLS latency), seamless switch from in-progress EVENT playlist to the final VOD.
- Jobs view with cancel; Settings view showing server capabilities (encoder candidates, ladder).

## Requirements

Ubuntu 24.04+ (developed on 26.04). GStreamer ≥ 1.20, GCC ≥ 13 or Clang ≥ 17 (C++23 `std::expected`), CMake ≥ 3.22, Boost ≥ 1.78, Node.js ≥ 20.

```bash
./scripts/setup-ubuntu.sh        # apt packages + Node LTS via nvm
```

For NVENC install the NVIDIA driver; `nvh264enc` ships in `gstreamer1.0-plugins-bad`. For Intel/AMD, `gstreamer1.0-vaapi` or the `va` plugin (plugins-bad) plus `libva` drivers.

## Build & run

```bash
# backend
./scripts/build-backend.sh                           # cmake + ninja + unit tests → backend/build/vsp-server, vsp-probe
backend/build/vsp-server --data-dir ./data           # http://0.0.0.0:8080

# desktop (in another terminal)
cd desktop && npm install && npm run dev             # Electron, connects to http://127.0.0.1:8080

# or both at once
./scripts/dev.sh
```

If `npm install` leaves `desktop/node_modules/electron/dist` empty (npm ≥ 11 install-script gating), run
`npm install-scripts approve electron esbuild` then `npm rebuild electron`, or extract the zip from `~/.cache/electron` into `dist/`.

Package the desktop app: `cd desktop && npm run package` (AppImage + deb in `desktop/release/`).

### Backend configuration

| JSON key / flag / env            | default   | notes |
|----------------------------------|-----------|-------|
| `host` / `--host` / `VSP_HOST`   | `0.0.0.0` | |
| `port` / `--port` / `VSP_PORT`   | `8080`    | |
| `data_dir` / `--data-dir` / `VSP_DATA_DIR` | `./data` | `uploads/`, `media/`, `vsp.sqlite3` |
| `encoder` / `--encoder` / `VSP_ENCODER` | `auto` | `auto` · `nvenc` · `vaapi` · `qsv` · `x264` |
| `concurrent_jobs` / `--jobs`     | `0`       | 0 = 1 for hardware encoders, 2 for x264 |
| `http_threads`                   | `0`       | 0 = cores/4 (min 2) |
| `segment_duration_ms`            | `2000`    | HLS segment / keyframe interval |
| `part_duration_ms`               | `1000`    | LL-HLS part duration; `0` disables parts |
| `audio_bitrate_kbps`             | `128`     | |
| `cors_origin`                    | `*`       | |
| `log_level`                      | `info`    | trace/debug/info/warn/error |

`vsp-server --config config.json` (see `backend/config.example.json`; a `config.json` in the CWD is picked up automatically).

## HTTP API

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/health` | liveness, selected encoder, queue depth |
| GET | `/api/system` | encoder candidates, ladder, packaging settings |
| POST | `/api/videos` | create: `{title, description, filename, size_bytes, options}` → video + `upload_url` |
| PUT | `/api/videos/{id}/source` | **streaming** raw body upload; on completion a transcode job is queued |
| PUT | `/api/videos/{id}/thumbnail` | JPEG poster (≤ 10 MB) |
| GET | `/api/videos[?status=]` · `/api/videos/{id}` | list / detail |
| PATCH / DELETE | `/api/videos/{id}` | edit title/description · delete video + files |
| POST | `/api/videos/{id}/transcode` | re-transcode (optionally with new `options`) |
| GET | `/api/jobs` · `/api/jobs/{id}` · POST `/api/jobs/{id}/cancel` | job tracking |
| GET | `/api/events` | SSE: `video.updated`, `video.deleted`, `job.updated`, `job.progress` |
| GET | `/media/{id}/master.m3u8` | HLS master; renditions at `/media/{id}/v_720p/index.m3u8`, `…/a_128k/index.m3u8` |

`options`: `{ trim_start_ms, trim_end_ms (0 = end), renditions: ["1080p","720p"] (empty = auto), audio_bitrate_kbps (0 = default), keep_source }`.

Example with curl:

```bash
V=$(curl -s -XPOST localhost:8080/api/videos -H 'Content-Type: application/json' \
      -d '{"title":"Demo","filename":"demo.mp4","options":{"trim_start_ms":3000}}')
ID=$(echo "$V" | jq -r .id)
curl -s -XPUT "localhost:8080/api/videos/$ID/source" --data-binary @demo.mp4 | jq .job.status
curl -s "localhost:8080/api/videos/$ID" | jq '{status, progress, playback_url}'
```

## Output layout

```
data/media/<video_id>/
├── master.m3u8                # EXT-X-STREAM-INF per video rendition + EXT-X-MEDIA audio group
├── thumbnail.jpg
├── v_1080p/ init.mp4  seg_00001.m4s …  index.m3u8
├── v_720p/  …
└── a_128k/  init.mp4  seg_00001.m4s …  index.m3u8
```

While transcoding, `index.m3u8` is an `EVENT` playlist carrying `#EXT-X-PART … BYTERANGE` entries for the most recent segments and `#EXT-X-PART-INF` / `#EXT-X-SERVER-CONTROL`; at completion it becomes `VOD` with `#EXT-X-ENDLIST`.

## Testing

```bash
backend/build/vsp-mp4-tests                     # ISOBMFF parser unit tests
./scripts/make-test-clip.sh .testdata 30        # synthetic 1080p30 clip with audio (no external media)
./scripts/verify-hls.sh http://127.0.0.1:8080/media/<id>/master.m3u8 data/media/<id>   # hlsdemux2 playback + decode of every rendition
```

`scripts/electron-cdp-check.mjs` / `electron-cdp-upload.mjs` drive the running Electron app over CDP (`npm run dev -- -- --remote-debugging-port=9333`) to assert real playback and the full upload → live preview → VOD flow.

## `vsp-probe` — media compatibility scanner

Scans files or whole directory trees and tells you which ones the platform can ingest, using the *same* GStreamer probe the server uses. Every file is probed in its own child process, so a crashing demuxer only fails that file instead of the scan.

```bash
backend/build/vsp-probe /path/to/archive                 # metadata probe (fast, I/O bound)
backend/build/vsp-probe -d /path/to/archive              # + decode the first 3 s of each file (catches broken essence)
backend/build/vsp-probe -p -d --json out.json --csv out.csv /path/to/archive   # only problems, with exports
backend/build/vsp-probe file1.mxf file2.mov              # individual files
```

| verdict | meaning |
|---------|---------|
| `supported` | transcodes as-is |
| `partial` | transcodes with caveats: no audio, interlaced source, >60 fps (capped), >8-bit (output is 8-bit H.264), extra audio/video tracks (first used) |
| `unsupported` | probe or decode failed — the reason is printed (missing decoder, demuxer crash, corrupt file, timeout) |
| `not-video` | audio-only essence (Avid/P2 `A01…` sidecars), raw camera frame sequences (`.ari`, `.dng`, `.dpx`…), stills |

The report ends with a per-codec and per-container tally and a ranked list of failure reasons. Exit status is 1 if anything is `unsupported`. Default parallelism is low (2–6) because probing is disk-seek bound; raise `-j` on SSDs.

### Broadcast/pro-format support

The full matrix of containers, video/audio codecs and how source properties are handled is in **[docs/SUPPORTED_FORMATS.md](docs/SUPPORTED_FORMATS.md)**.


Tested against a 129 GB broadcast test corpus (Avid, Panasonic P2, Sony XDCAM/XDCAM-EX, ARRI, Canon, GoPro, FCP exports — see [docs/editshare-compatibility-report.txt](docs/editshare-compatibility-report.txt)). **614 of 614 video files that probe also decode and transcode**: DNxHD, Apple ProRes, AVC-Intra 50/100 (Avid OP-Atom and P2), XDCAM-HD 422, IMX/D-10, DV/DVCPRO, MPEG-2, uncompressed 4:2:2, H.264 (GoPro/Canon/phones), MJPEG/animation.

Two platform fixes came out of that run and apply to the server as well as the probe:
- **`avdemux_mxf` is preferred over `mxfdemux`** (`src/media/gst_init.cpp`). GStreamer's own MXF demuxer rejects Avid OP-Atom files and aborts on Panasonic P2 index tables.
- **AVC-Intra caps fix-up** (`src/media/mxf_fixup.cpp`). libav's MXF demuxer advertises AVC-Intra as `stream-format=avc` with Annex-B SPS/PPS in `codec_data`, which `h264parse` refuses; a pad probe rewrites the caps to byte-stream and injects the SPS/PPS in-band.

Still unsupported in that corpus (71 files): Avid 2.8-era JFIF/15:1s MXF, Grass Valley `gfcam` AUDIO/ANC sidecars, a few Avid 10-bit uncompressed OP-Atom clips (`v210` essence key not recognised), Convergent Design 720p50 MOVs with an unknown FourCC, and `dvhp`/`dvhq` (DVCPRO HD in MOV) which has no GStreamer decoder.

## Project layout

```
backend/src/
  core/      config, logging, utils
  storage/   models + SQLite repository
  media/     gst_init · mxf_fixup · encoder_detect · probe (GstDiscoverer) · ladder · mp4_box (ISOBMFF) · cmaf_packager · transcoder · thumbnail
backend/tools/
  probe_main.cpp   vsp-probe compatibility scanner
  jobs/      worker pool running transcode jobs
  http/      Beast coroutine server · router · static file serving (Range) · SSE broker · API handlers
desktop/src/
  main/      Electron main (window, dialogs)
  preload/   context bridge
  renderer/  React app: Library · UploadWizard · Player · Jobs · Settings
scripts/     setup-ubuntu.sh · build-backend.sh · dev.sh · make-test-clip.sh · verify-hls.sh · electron-cdp-*.mjs
```

## Roadmap / known limitations (v1)

- No authentication or multi-tenancy; intended for a trusted network.
- LL-HLS blocking playlist reload (`CAN-BLOCK-RELOAD`, `_HLS_msn`) and `EXT-X-PRELOAD-HINT` are not implemented; players poll the playlist.
- Uploads are single-request (not resumable); disk I/O is synchronous on the HTTP threads (io_uring via `asio::stream_file` is a natural next step).
- H.264 only; HEVC/AV1 ladders, DASH output, live ingest (RTMP/SRT/WebRTC WHIP) are the next milestones.
- Interlaced sources are encoded as-is (no deinterlacer in the pipeline yet); >8-bit sources are converted to 8-bit.
