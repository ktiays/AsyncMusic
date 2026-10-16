# Apple Lossless (ALAC)

Apple's open-source reference codec, from https://github.com/macosforge/alac
(commit `c38887c5c5e64a4b31108733bd79ca9b2496d987`), Apache License 2.0 (see `LICENSE`).
Only the `codec` directory is included. Use it through `asyncmusic/alac.hpp`.

Local changes:

- `ALACDecoder.cpp`: partial frames longer than the cookie's frame length are rejected instead
  of overflowing the decoder's buffers (packets arrive from the network).
- `EndianPortable.c`: little-endian targets are detected from compiler macros; the original
  list missed MSVC and ARM outside Apple's headers, which made cookies and PCM byte-swapped.
