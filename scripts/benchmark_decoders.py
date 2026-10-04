#!/usr/bin/env python3
"""Build and run the decoder benchmark harness over the generated dataset.

    python3 scripts/generate_dataset.py
    python3 scripts/benchmark_decoders.py [--dataset DIR] [--out report.json]

Needs g++ and the dev packages libturbojpeg, libwebp, libpng
(Debian/Ubuntu: apt install libturbojpeg0-dev libwebp-dev libpng-dev).

Scenarios: thread scaling (1/2/4/N) for the baseline behaviour, plus the
optimisation variants from scripts/bench/decode_bench.cpp at the highest thread
count. Reports per-decoder time, throughput, peak RSS, thread efficiency and
whether decoded gray output is bit-identical to the baseline.
"""
import argparse, glob, json, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def build(exe):
    src = os.path.join(HERE, "bench", "decode_bench.cpp")
    cmd = ["g++", "-O2", "-std=c++17", "-pthread", src, "-o", exe, "-lturbojpeg", "-lwebp", "-lpng"]
    subprocess.check_call(cmd)


def run(exe, listfile, threads, opt, repeat):
    out = subprocess.check_output([exe, "--list", listfile, "--threads", str(threads),
                                   "--repeat", str(repeat), "--opt", opt])
    return json.loads(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", default=os.path.join(HERE, "..", "testdata", "dataset"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "testdata", "benchmark_report.json"))
    ap.add_argument("--max-threads", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    files = sorted(f for f in glob.glob(os.path.join(os.path.abspath(args.dataset), "*"))
                   if not f.endswith(".json"))
    if not files:
        sys.exit("no images found; run scripts/generate_dataset.py first")
    tmp = tempfile.mkdtemp()
    exe = os.path.join(tmp, "decode_bench")
    build(exe)
    lst = os.path.join(tmp, "files.txt")
    open(lst, "w").write("\n".join(files) + "\n")
    for f in files:                       # warm the page cache: measure decode, not disk
        open(f, "rb").read()

    tmax = args.max_threads
    scen = [("baseline", "none", t) for t in sorted({1, 2, 4, tmax} - {0})]
    for opt in ("reuse", "earlyfree", "seqread", "fastjpeg", "reuse,earlyfree,seqread"):
        scen.append((opt, opt, tmax))
    results = []
    for name, opt, t in scen:
        r = run(exe, lst, t, opt, 1)
        r["name"] = name
        results.append(r)
        print(f"{name:26s} threads={t}  wall={r['wall_ms']:8.0f} ms  {r['images_per_s']:6.1f} img/s  rss={r['peak_rss_mb']:6.0f} MB", flush=True)

    base = {r["threads"]: r for r in results if r["name"] == "baseline"}
    t1 = base[1]
    print("\nThread scaling (baseline):")
    for t, r in sorted(base.items()):
        sp = t1["wall_ms"] / r["wall_ms"]
        print(f"  {t} threads: speedup {sp:.2f}x  efficiency {sp / t * 100:.0f}%")
    print("\nPer-decoder (baseline, 1 thread):")
    for k, v in t1["formats"].items():
        print(f"  {k:5s} decode {v['decode_ms_per_img']:7.2f} ms/img  {v['decode_mpix_per_s']:7.1f} Mpix/s  gray {v['gray_ms'] / v['count']:.2f} ms/img")
    ref = base[tmax]
    print(f"\nVariants vs baseline @ {tmax} threads (wall / RSS / output identical):")
    for r in results:
        if r["name"] == "baseline":
            continue
        same = all(r["formats"][k]["gray_hash"] == ref["formats"][k]["gray_hash"] for k in ref["formats"])
        print(f"  {r['name']:26s} {ref['wall_ms'] / r['wall_ms']:5.2f}x  rss {r['peak_rss_mb']:.0f} vs {ref['peak_rss_mb']:.0f} MB  identical={same}")
    json.dump(results, open(args.out, "w"), indent=1)
    print("\nreport:", os.path.abspath(args.out))


if __name__ == "__main__":
    main()
