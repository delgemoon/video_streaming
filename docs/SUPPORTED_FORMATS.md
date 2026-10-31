# Supported formats

What VSP can ingest (upload → transcode → LL-HLS) and what it produces. Ingest support is whatever
GStreamer on the server host can demux and decode; the lists below reflect the reference Ubuntu
setup from [`scripts/setup-ubuntu.sh`](../scripts/setup-ubuntu.sh) (GStreamer 1.28, plugins
base/good/bad/ugly/libav, NVIDIA driver present).

Run [`vsp-probe`](../README.md#vsp-probe--media-compatibility-scanner) on your own media to get a
definitive answer for a given host: `backend/build/vsp-probe -d /path/to/media`.

Legend
- ✅ **Verified** — real files transcoded end-to-end and the HLS output decoded back (counts are from the
  129 GB broadcast test corpus in [editshare-compatibility-report.txt](editshare-compatibility-report.txt)).
- 🟡 **Available** — a decoder/demuxer is installed and should work, but has not been exercised with real content.
- ❌ **Not supported** — no decoder/demuxer, or known to fail.

---

## Input containers

| Container | Extensions | Demuxer | Status |
|---|---|---|---|
| QuickTime / MOV | `.mov` `.qt` | `qtdemux` | ✅ 307/307 |
| MP4 / M4V / 3GP | `.mp4` `.m4v` `.3gp` `.3g2` | `qtdemux` | ✅ (GoPro, phone, FCP exports) |
| MXF — Sony XDCAM / XDCAM-EX / generic OP1a | `.mxf` | `avdemux_mxf` (preferred), `mxfdemux` | ✅ |
| MXF — Avid OP-Atom (DNxHD, AVC-Intra, XDCAM-HD, IMX, DV) | `.mxf` | `avdemux_mxf` | ✅ 297/297 MXF total |
| MXF — Panasonic P2 (AVC-Intra, DVCPRO) | `.mxf` | `avdemux_mxf` + AVC-Intra caps fix-up | ✅ |
| MPEG Program Stream | `.mpg` `.mpeg` `.ps` `.vob` `.mp2ps` | `mpegpsdemux` | ✅ 5/5 |
| MPEG Transport Stream | `.ts` `.m2ts` `.mts` `.mp2ts` | `tsdemux` | ✅ 2/2 |
| AVI | `.avi` `.divx` | `avidemux` | ✅ 2/2 |
| Raw DV | `.dv` | `dvdemux` | ✅ 1/1 |
| Matroska / WebM | `.mkv` `.webm` | `matroskademux` | 🟡 |
| FLV | `.flv` `.f4v` | `flvdemux` | 🟡 |
| ASF / WMV | `.asf` `.wmv` | `asfdemux` | 🟡 |
| Ogg | `.ogv` `.ogm` | `oggdemux` | 🟡 |
| RealMedia | `.rm` `.rmvb` | `rmdemux` | 🟡 |
| GXF (Grass Valley) | `.gxf` | `avdemux_gxf` | 🟡 |
| NUT, IVF, Y4M | `.nut` `.ivf` `.y4m` | `avdemux_nut`, `avdemux_ivf`, `y4mdec` | 🟡 |
| Elementary streams | `.h264` `.264` `.hevc` `.265` `.m2v` | parsers via `decodebin3` | 🟡 |
| Camera raw frame sequences | `.ari` `.dng` `.dpx` `.exr` `.cin` | — | ❌ reported as *not-video* |
| RED / Blackmagic raw | `.r3d` `.braw` | — | ❌ |

---

## Input video codecs

| Codec | Decoder | Status | Notes |
|---|---|---|---|
| H.264 / AVC (Baseline, Main, High, High 10/4:2:2 Intra) | `avdec_h264`, `nvh264dec` | ✅ 122/122 | incl. GoPro, Canon DSLR, phone, FCP |
| AVC-Intra 50 / 100 (Avid OP-Atom and P2) | `avdec_h264` | ✅ | needs `mxf_fixup` (SPS/PPS injected in-band) |
| MPEG-2 Video (XDCAM-HD 422, HDV, IMX/D-10, DVD) | `avdec_mpeg2video`, `nvmpeg2videodec` | ✅ 272/272 | |
| MPEG-1 Video | `avdec_mpegvideo` | ✅ 2/2 | |
| Avid DNxHD / DNxHR | `avdec_dnxhd` | ✅ 70/70 | 8- and 10-bit |
| Apple ProRes (422 Proxy/LT/422/HQ, 4444) | `avdec_prores` | ✅ 45/45 | 10-bit → 8-bit output |
| DV / DVCPRO 25 / DVCPRO 50 | `avdec_dvvideo` | ✅ 58/58 | |
| Uncompressed 4:2:2 (`2vuy`, `yuv2`, `v210` in MOV) | raw | ✅ 30/30 | |
| Uncompressed RGB/RGBA | raw | ✅ 2/2 | |
| QuickTime Animation (RLE) | `avdec_qtrle` | ✅ 11/11 | |
| MPEG-4 Part 2 / DivX / Xvid | `avdec_mpeg4` | ✅ 2/2 | |
| HEVC / H.265 (8/10-bit) | `avdec_h265`, `nvh265dec` | 🟡 | |
| VP8 / VP9 | `vp8dec`, `vp9dec`, `nvvp9dec` | 🟡 | |
| AV1 | `av1dec` (aom), `nvav1dec` | 🟡 | |
| VC-1 / WMV 7-9 | `avdec_vc1`, `avdec_wmv*` | 🟡 | |
| Theora | `theoradec` | 🟡 | |
| Motion JPEG / MJPEG-B | `avdec_mjpeg`, `jpegdec` | 🟡 | |
| JPEG 2000 | `avdec_jpeg2000`, `openjpegdec` | 🟡 | |
| CineForm (CFHD) | `avdec_cfhd` | 🟡 | |
| FFV1, HuffYUV, UT Video | `avdec_ffv1`, `avdec_huffyuv` | 🟡 | |
| H.263, Sorenson, Flash Video, Cinepak, Indeo, RealVideo, MS Video 1 | libav | 🟡 | legacy |
| DVCPRO HD in MOV (`dvhp`, `dvhq`, `dvh5`, `dvh6`) | — | ❌ | no GStreamer decoder; works via MXF |
| Avid 10-bit uncompressed OP-Atom, Avid JFIF 15:1s / 2:1 (Meridien) | — | ❌ | pre-2010 Avid essence keys |
| ARRIRAW, REDCODE, Blackmagic RAW | — | ❌ | camera raw, out of scope |

---

## Input audio codecs

| Codec | Decoder | Status |
|---|---|---|
| PCM 8 / 16 / 24 / 32-bit int, 32-bit float | raw | ✅ 286/286 |
| AAC-LC / HE-AAC | `avdec_aac` | ✅ 46/46 |
| MPEG-1 Layer II (MP2) | `avdec_mp2float` | ✅ 6/6 |
| MP3 | `avdec_mp3` | 🟡 |
| AC-3 / E-AC-3 | `avdec_ac3`, `avdec_eac3` | 🟡 |
| Opus, Vorbis, FLAC, ALAC, Speex | libav / `speexdec` | 🟡 |
| WMA v1/v2/Pro/Lossless | libav | 🟡 |
| AMR-NB / AMR-WB | libav | 🟡 |
| Dolby TrueHD / MLP | libav | 🟡 |
| ADPCM (IMA, MS, QT, SWF …) | libav | 🟡 |
| DTS | — | ❌ not installed (`libdca`) |

Multi-track audio: the first audio stream is used. Avid/P2 split-essence audio files (`A01…A08.mxf`,
`CONTENTS/AUDIO/*.MXF`) are audio-only and are reported as *not-video*; audio is **not** currently merged
from sidecar files when the video essence has no embedded audio.

---

## Source characteristics and how they are handled

| Property | Handling |
|---|---|
| Resolution | Any; ladder is capped at source height (no upscaling). Sub-240p sources get one source-size rendition. |
| Frame rate | Preserved up to 60 fps; higher rates are capped to 60 (`videorate`). Variable frame rate is normalised to constant. |
| Interlaced | Encoded as-is (no deinterlacer yet) — flagged *partial* by `vsp-probe`. |
| Bit depth | 10/12-bit sources are converted to 8-bit (H.264 High profile 4:2:0 output). |
| Chroma | 4:2:2 / 4:4:4 converted to 4:2:0. |
| Aspect ratio | Non-square PAR (DV, IMX anamorphic) is squared on output. |
| Multiple video tracks | First one used. |
| Still-image sources | Rejected (*not-video*). |

---

## Output

| | |
|---|---|
| Container | CMAF fragmented MP4 (`init.mp4` + `.m4s`), LL-HLS playlists (`EXT-X-PART`, byte-range parts) |
| Video | H.264 High profile, 8-bit 4:2:0, CBR with GOP = part duration; ABR ladder 2160p → 240p |
| Encoders | NVENC (`nvh264enc`), VA-API (`vah264enc`/`vaapih264enc`), Intel QSV, x264, OpenH264 — auto-detected |
| Audio | AAC-LC 48 kHz stereo, 96–256 kbps, separate rendition (`EXT-X-MEDIA` audio group) |
| Player compatibility | hls.js (desktop app), Safari/iOS native HLS, Android ExoPlayer |

Not yet produced: HEVC/AV1 ladders, DASH manifests, subtitles/WebVTT, multi-audio.

---

## Live ingest

Not yet implemented. Elements are present for RTMP (`rtmp2src`, `flvdemux`), SRT (`srtsrc`, listener
mode) and RTSP (`rtspsrc`); WebRTC/WHIP elements are not installed (`gstreamer1.0-plugins-rs`).
See the roadmap in the [README](../README.md#roadmap--known-limitations-v1).

---

## Extending support

- **AV1 / dav1d, WHIP/WHEP, HLS/DASH sinks:** `sudo apt install gstreamer1.0-plugins-rs`
- **Intel/AMD hardware decode/encode:** `gstreamer1.0-vaapi` or the `va` plugin plus `intel-media-va-driver-non-free` / `mesa-va-drivers`
- **DTS audio:** `gstreamer1.0-plugins-bad` with `libdca` (patent-encumbered; not in default Ubuntu)
- Anything libav can decode but that is marked 🟡 above only needs a real sample to be promoted to ✅ — run
  `vsp-probe -d` over it and open an issue with the report if it fails.
