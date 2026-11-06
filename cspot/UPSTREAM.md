# Origin of this code

This directory is a copy of [cspot](https://github.com/feelfreelinux/cspot) by
feelfreelinux and alufers, with its support library
[bell](https://github.com/feelfreelinux/bell) in `cspot/bell`. It was vendored
because upstream has seen no maintenance for a long time and the firmware needs
changes inside it. Thanks to the original authors.

Licence: GNU General Public License v3 or later, see `LICENSE.md`. Bell and the
libraries under `cspot/bell/external` keep their own licences and notices.

| Component | Upstream | Commit copied |
|---|---|---|
| cspot | https://github.com/feelfreelinux/cspot | `1b07a9c00ba6d5e878e5b33dcbf89bff493cde26` (2024-07-12) |
| bell | https://github.com/feelfreelinux/bell | `e83737367a08b5a5a1f652a7ecb97a0d926929dd` |
| mdnssvc | https://github.com/philippe44/mdnssvc | `44e78c149d3e595dea8ce6bede7371744ff8b40d` |
| nlohmann/json | https://github.com/nlohmann/json | `07182ebc1d4d8204cd51635fbd62116c6b50da59` (v3.11.2-25) |

Local changes on top of those commits were first kept as patch files and were
folded into the source when it was vendored. Summary: nanopb generator support for
protobuf 7, login blob MAC check and connect error handling, civetweb thread
creation fixes on ESP32, Mercury reconnect/stack fixes, HTTP EOF and stream error
handling, 320 kbps with fallback, PSRAM stream buffers, and catalogue-aware track
restrictions. Test files, docs and benchmarks of nlohmann/json were dropped to
save space. Use `git log -- cspot` for details.
