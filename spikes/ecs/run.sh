#!/bin/sh
# Interleaves runs of both benchmarks and keeps each one's fastest mean, which is the least
# disturbed by whatever else the machine is doing.
dir=${1:-build/release/spikes/ecs}
runs=${2:-5}
out=$(mktemp -d)
for run in $(seq "$runs"); do
    for bench in entt flecs; do
        taskset -c 2-9 "$dir/ecs_${bench}_bench" --benchmark-samples 100 --reporter xml \
            > "$out/$bench-$run.xml" 2>/dev/null
    done
done
python3 - "$out" <<'PY'
import glob, sys, xml.etree.ElementTree as ET
best = {}
for path in sorted(glob.glob(sys.argv[1] + "/*.xml")):
    library = path.split("/")[-1].split("-")[0]
    for result in ET.parse(path).iter("BenchmarkResults"):
        mean = float(result.find("mean").get("value"))
        key = (library, result.get("name"))
        best[key] = min(best.get(key, mean), mean)
for (library, name), ns in sorted(best.items(), key=lambda kv: (kv[0][1], kv[0][0])):
    print(f"{name:40} {library:6} {ns / 1000:10.1f} us")
PY
rm -r "$out"
