# Decoder benchmark results

Dataset: 500 synthetic images (125 each JPEG/PNG/WEBP/BMP, 500x500 .. 3840x2160, 912 MB),
`scripts/generate_dataset.py`. Harness: `scripts/benchmark_decoders.py` (Linux, 4 cores, warm page cache).

**Caveat:** the AntiDupl core is Windows-only (GDI+/MSVC) and could not be built here. The harness calls the
same libjpeg-turbo / libwebp entry points as `adTurboJpeg.cpp` / `adWebp.cpp`; PNG (libpng) and BMP (own reader)
are stand-ins for GDI+, and gray conversion is a scalar stand-in for Simd. The C# side was not profiled
(no .NET SDK here). Numbers show relative cost of decoders, not absolute Windows timings.

## Baseline (1 thread, 500 images: 11.2 s)
| Decoder | ms/img | Mpix/s |
|---|---|---|
| JPEG (turbo) | 5.5 | 265 |
| PNG (libpng) | 55.0 | 26 |
| WEBP | 17.5 | 83 |
| BMP | 1.4 | 1047 |

PNG is ~61% of total time, WebP ~19%, JPEG ~6%. Gray conversion costs ~2.2 ms/img for every format.

## Thread scaling (baseline)
1 thread 1.00x, 2 threads 1.96x (98%), 4 threads 3.80x (95%): the existing pool scales well.

## Variants @ 4 threads (baseline 2948 ms, 338 MB peak RSS)
| Variant | Speedup | Peak RSS | Output identical |
|---|---|---|---|
| per-thread buffer reuse | 1.04x | 317 MB | yes |
| early free of 32-bit view | 1.04x | 293 MB (-13%) | yes |
| sequential read hint | 1.00x | 311 MB | yes |
| JPEG FASTUPSAMPLE+FASTDCT | 1.05x | 308 MB | **no** (gray mean diff 0.92, max 26) |
| reuse+earlyfree+seqread | 1.06x | 322 MB | yes |

## Applied
Only the early free of the 32-bit view (`TImage::ReleaseView`, called in `TDataCollector::FillPixelData`),
because it is output-neutral and measurably lowers peak memory. Fast JPEG flags were rejected (changes pixels,
JPEG is a small share of time). Not compiled/tested on Windows.
