# Third-party notices

OmaBroadcast's own code is under the [MIT licence](LICENSE). The package links
FFmpeg built under GPL v3, so the package as a whole is distributed under GPL v3,
while this source stays MIT. x264 and x265, licensed "version 2 or any later
version", come in under that later version.

## FFmpeg and the libraries beside it

The package carries its own FFmpeg 9.1 for OmaBroadcast alone, in
`/usr/lib/omabroadcast/ffmpeg`, built by `bin/build-ffmpeg` under GPL v3 from a
pinned commit. Everything else on your computer keeps the system's FFmpeg.

This FFmpeg is modified by two small patches, kept in `pkgbuild/ffmpeg`: one from
the Chromium project (2021) that Arch Linux also applies, and OmaBroadcast's own
(2026-10-25).

These travel beside it, as Arch Linux packaged them:

| Library | Licence |
| --- | --- |
| x264 | GPL-2.0-or-later |
| x265 | GPL-2.0-or-later |
| SVT-AV1 | BSD-3-Clause and AOM patent licence |
| aom | BSD-3-Clause |
| dav1d | BSD-2-Clause |
| libvpx | BSD |
| SRT | MPL-2.0 |

Their licences are installed in `/usr/share/licenses/omabroadcast/ffmpeg/`, in a
folder for each library. The file `SOURCE` there names FFmpeg's repository, exact
commit, patches and configuration, and each library's exact version with the
address of Arch Linux's recipe for building it.

## Complete source

Every package on the [Releases](https://github.com/28allday/omabroadcast/releases)
page has its complete source beside it, at no charge, as
`omabroadcast-<version>-<release>-source.tar.xz`: OmaBroadcast, FFmpeg, each library
above with Arch Linux's recipe and patches, and the scripts that build them.

## Other components

- The Wayland protocol definitions in `src/protocols` are public specifications under
  BSD-3-Clause and MIT-style licences, with their notices in
  `/usr/share/licenses/omabroadcast/protocols/`.
- The launch window shows short quotations, each credited with its source
  (`src/launch/quotes.json`).
- **Remove background (AI)** uses the weights of MODNet (Zhanghan Ke and others,
  Apache-2.0), converted for OmaBroadcast's own engine:
  `/usr/share/licenses/omabroadcast/LICENSE.MODNet`.
- The icons are Lucide's (ISC):
  `/usr/share/licenses/omabroadcast/LICENSE.lucide`.
- The Spectrum animation on the launch window is original artwork by Gavin Nugent,
  copyright 2026.
