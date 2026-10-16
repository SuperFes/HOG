#!/bin/sh
# Runs each transport's server and clients in a network namespace of their own, whose loopback
# is impaired like a home connection: 75 ms each way, ±15 ms jitter and 2% loss. Nothing outside
# the namespace is slowed.
#   spikes/net/run.sh [build dir] [clients] [seconds] [transport...]
dir=$(realpath "${1:-build/release/spikes/net}")
clients=${2:-4}
seconds=${3:-30}
shift 3 2>/dev/null
transports=${*:-baseline gns msquic yojimbo}
port=27015

for transport in $transports; do
    [ -x "$dir/net_$transport" ] || { echo "$transport: not built"; continue; }
    unshare -rn sh -s "$dir" "$transport" "$clients" "$seconds" "$port" <<'INNER'
dir=$1 transport=$2 clients=$3 seconds=$4 port=$5
ip link set lo up
tc qdisc add dev lo root netem limit 100000 delay 75ms 15ms loss 2%
out=$(mktemp -d)
cd "$out" || exit 1
# For QUIC's TLS; the clients here don't validate it.
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -keyout key.pem -out cert.pem \
    -days 1 -nodes -subj /CN=localhost 2>/dev/null
"$dir/net_$transport" server "$port" $((seconds + 5)) > server.json 2> server.log &
server=$!
sleep 1
"$dir/net_$transport" clients "$clients" 127.0.0.1 "$port" "$seconds" > clients.json 2> clients.log
wait $server
printf '%-9s %s\n' "$transport" "$(cat clients.json)"
printf '%-9s %s\n' "" "$(cat server.json)"
[ -s server.log ] && sed 's/^/  server: /' server.log | tail -3
[ -s clients.log ] && sed 's/^/  clients: /' clients.log | tail -3
rm -r "$out"
INNER
done
