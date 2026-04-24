#!/usr/bin/env bash
# Set up the NativeLink remote-cache server for yetty Buck2 builds.
#
# Reproducible — regenerates everything in ./tmp/nativelink/ from this
# script. No hand-editing of generated files. Edit the script instead.
#
# Layout:
#   CAS + scheduler run in docker (image pinned below).
#   Worker runs on the host inside `nix develop '.#buck2'` so actions see
#   the nix toolchain (clang, cmake, pkg-config, ...).
#
# Idempotent. Re-run after any flake.nix change that alters the toolchain.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

NL_IMAGE="ghcr.io/tracemachina/nativelink:v1.0.0"
NL_DIR="$REPO_ROOT/tmp/nativelink"
WORKER_CACHE="$HOME/.cache/nativelink-worker"

log() { printf '\n==> %s\n' "$*"; }

# ---- prerequisites ----------------------------------------------------------
command -v docker         >/dev/null || { echo "docker not found"; exit 1; }
command -v docker-compose >/dev/null || { echo "docker-compose not found"; exit 1; }
command -v nix            >/dev/null || { echo "nix not found"; exit 1; }
command -v python3        >/dev/null || { echo "python3 not found"; exit 1; }

[ -f "$REPO_ROOT/flake.nix" ] || { echo "flake.nix missing — run from yetty repo root"; exit 1; }

mkdir -p "$NL_DIR" \
         "$WORKER_CACHE"/work \
         "$WORKER_CACHE"/data/content_path-cas \
         "$WORKER_CACHE"/data/tmp_path-cas

# ---- image + binary ---------------------------------------------------------
log "Pulling $NL_IMAGE"
docker pull -q "$NL_IMAGE"

log "Extracting nativelink binary from image (for host worker)"
cid=$(docker create "$NL_IMAGE")
nl_path=$(docker inspect -f '{{index .Config.Entrypoint 0}}' "$cid")
docker cp "$cid:$nl_path" "$NL_DIR/nativelink"
docker rm "$cid" >/dev/null
chmod +x "$NL_DIR/nativelink"

# ---- CAS config (docker) ---------------------------------------------------
log "Writing local-storage-cas.json5"
cat > "$NL_DIR/local-storage-cas.json5" <<'JSON'
{
  stores: [
    {
      name: "CAS_MAIN_STORE",
      compression: {
        compression_algorithm: { lz4: {} },
        backend: {
          filesystem: {
            content_path: "/root/.cache/nativelink/content_path-cas",
            temp_path: "/root/.cache/nativelink/tmp_path-cas",
            eviction_policy: { max_bytes: 10000000000 },
          },
        },
      },
    },
    {
      name: "AC_MAIN_STORE",
      filesystem: {
        content_path: "/root/.cache/nativelink/content_path-ac",
        temp_path: "/root/.cache/nativelink/tmp_path-ac",
        eviction_policy: { max_bytes: 500000000 },
      },
    },
  ],
  servers: [
    {
      listener: { http: { socket_address: "0.0.0.0:50051" } },
      services: {
        cas: { "": { cas_store: "CAS_MAIN_STORE" } },
        ac:  { "": { ac_store:  "AC_MAIN_STORE"  } },
        capabilities: {},
        bytestream: { cas_stores: { "": "CAS_MAIN_STORE" } },
      },
    },
  ],
}
JSON

# ---- scheduler config (docker) ---------------------------------------------
log "Writing scheduler.json5"
cat > "$NL_DIR/scheduler.json5" <<'JSON'
{
  stores: [
    {
      name: "GRPC_LOCAL_STORE",
      grpc: {
        instance_name: "",
        endpoints: [{ address: "grpc://${CAS_ENDPOINT:-127.0.0.1}:50051" }],
        store_type: "cas",
      },
    },
    {
      name: "GRPC_LOCAL_AC_STORE",
      grpc: {
        instance_name: "",
        endpoints: [{ address: "grpc://${CAS_ENDPOINT:-127.0.0.1}:50051" }],
        store_type: "ac",
      },
    },
  ],
  schedulers: [
    {
      name: "MAIN_SCHEDULER",
      simple: {
        supported_platform_properties: {
          cpu_count: "minimum",
          OSFamily: "priority",
          "container-image": "priority",
          "lre-rs": "priority",
          ISA: "exact",
        },
      },
    },
  ],
  servers: [
    {
      listener: { http: { socket_address: "0.0.0.0:50052" } },
      services: {
        ac: [{ ac_store: "GRPC_LOCAL_AC_STORE" }],
        execution: [{
          cas_store: "GRPC_LOCAL_STORE",
          scheduler: "MAIN_SCHEDULER",
        }],
        capabilities: [{ remote_execution: { scheduler: "MAIN_SCHEDULER" } }],
      },
    },
    {
      listener: { http: { socket_address: "0.0.0.0:50061" } },
      services: {
        worker_api: { scheduler: "MAIN_SCHEDULER" },
        health: {},
      },
    },
  ],
}
JSON

# ---- worker config (host) --------------------------------------------------
log "Writing worker.json5"
NPROC=$(nproc)
cat > "$NL_DIR/worker.json5" <<JSON
{
  stores: [
    {
      name: "GRPC_LOCAL_STORE",
      grpc: {
        instance_name: "",
        endpoints: [{ address: "grpc://127.0.0.1:50051" }],
        store_type: "cas",
      },
    },
    {
      name: "GRPC_LOCAL_AC_STORE",
      grpc: {
        instance_name: "",
        endpoints: [{ address: "grpc://127.0.0.1:50051" }],
        store_type: "ac",
      },
    },
    {
      name: "WORKER_FAST_SLOW_STORE",
      fast_slow: {
        fast: {
          filesystem: {
            content_path: "$WORKER_CACHE/data/content_path-cas",
            temp_path: "$WORKER_CACHE/data/tmp_path-cas",
            eviction_policy: { max_bytes: 10000000000 },
          },
        },
        fast_direction: "get",
        slow: { ref_store: { name: "GRPC_LOCAL_STORE" } },
      },
    },
  ],
  workers: [
    {
      local: {
        worker_api_endpoint: { uri: "grpc://127.0.0.1:50061" },
        cas_fast_slow_store: "WORKER_FAST_SLOW_STORE",
        upload_action_result: { ac_store: "GRPC_LOCAL_AC_STORE" },
        work_directory: "$WORKER_CACHE/work",
        entrypoint: "$NL_DIR/action-entrypoint.sh",
        platform_properties: {
          cpu_count: { values: ["$NPROC"] },
          OSFamily: { values: [""] },
          "container-image": { values: [""] },
          "lre-rs": { values: [""] },
          ISA: { values: ["x86-64"] },
        },
      },
    },
  ],
  servers: [],
}
JSON

# ---- docker-compose --------------------------------------------------------
log "Writing docker-compose.yml"
cat > "$NL_DIR/docker-compose.yml" <<YML
services:
  nativelink_cas:
    image: $NL_IMAGE
    volumes:
      - nativelink_cache:/root/.cache/nativelink
      - ./local-storage-cas.json5:/config.json5:ro
    environment:
      RUST_LOG: \${RUST_LOG:-warn}
    ports:
      - "50051:50051/tcp"
    command: ["/config.json5"]

  nativelink_scheduler:
    image: $NL_IMAGE
    volumes:
      - ./scheduler.json5:/config.json5:ro
    environment:
      RUST_LOG: \${RUST_LOG:-warn}
      CAS_ENDPOINT: nativelink_cas
    ports:
      - "50052:50052/tcp"
      - "50061:50061/tcp"
    command: ["/config.json5"]
    depends_on:
      - nativelink_cas

volumes:
  nativelink_cache:
YML

# ---- capture nix-develop env into action-env.sh ----------------------------
log "Capturing nix-develop env into action-env.sh"
nix develop '.#buck2' --command env 2>/dev/null > "$NL_DIR/_raw-env"
python3 - "$NL_DIR/_raw-env" "$NL_DIR/action-env.sh" <<'PY'
import re, shlex, sys, pathlib
src, dst = sys.argv[1], sys.argv[2]
# Skip host-specific / session vars that would leak into the action env.
skip = re.compile(
    r'^(_|PWD|OLDPWD|SHLVL|SHELL|USER|LOGNAME|DBUS_|DESKTOP|WAYLAND_DISPLAY|'
    r'DISPLAY|XDG_(SESSION|RUNTIME|GREETER|CURRENT|SEAT|VTNR)|SSH_|TERM$|'
    r'TMUX|LS_COLORS|LESS$|PAGER$|EDITOR$|VISUAL$|XAUTHORITY|GDK_|QT_|GTK_|'
    r'HOSTNAME|MOTD|HISTFILE|HISTSIZE|LANGUAGE|LC_|__NIXOS|GOPATH$|COLORTERM|'
    r'COLUMNS|LINES|npm_|BASH_|INVOCATION_ID|JOURNAL|SYSTEMD_|ALACRITTY)=')
out = []
for ln in pathlib.Path(src).read_text().splitlines():
    m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)=(.*)$', ln)
    if not m:
        continue
    k, v = m.group(1), m.group(2)
    if skip.match(k + '='):
        continue
    out.append(f'export {k}={shlex.quote(v)}')
pathlib.Path(dst).write_text('\n'.join(out) + '\n')
print(f'{len(out)} env vars captured')
PY
rm -f "$NL_DIR/_raw-env"

# ---- action-entrypoint.sh (sourced before every action) --------------------
log "Writing action-entrypoint.sh"
cat > "$NL_DIR/action-entrypoint.sh" <<EOF
#!/bin/sh
. "$NL_DIR/action-env.sh"
exec "\$@"
EOF
chmod +x "$NL_DIR/action-entrypoint.sh"

# ---- start stack + host worker ---------------------------------------------
log "Starting CAS + scheduler via docker-compose"
(cd "$NL_DIR" && docker-compose down >/dev/null 2>&1 || true)
(cd "$NL_DIR" && docker-compose up -d)

for _ in $(seq 1 30); do
    if docker-compose -f "$NL_DIR/docker-compose.yml" ps | grep -q 'Up'; then
        break
    fi
    sleep 1
done

log "Starting host worker (inside nix develop '.#buck2')"
pkill -f 'nativelink worker.json5' 2>/dev/null || true
sleep 2
: > "$REPO_ROOT/tmp/nativelink-worker.log"
nohup nix develop '.#buck2' --command bash -c \
    "cd '$NL_DIR' && exec ./nativelink worker.json5" \
    > "$REPO_ROOT/tmp/nativelink-worker.log" 2>&1 &
disown

for _ in $(seq 1 30); do
    if grep -q 'Worker registered' "$REPO_ROOT/tmp/nativelink-worker.log" 2>/dev/null; then
        break
    fi
    sleep 1
done

if grep -q 'Worker registered' "$REPO_ROOT/tmp/nativelink-worker.log" 2>/dev/null; then
    log "Worker registered. Setup complete."
    cat <<EOF

Verify:
  docker-compose -f $NL_DIR/docker-compose.yml ps
  pgrep -af 'nativelink worker.json5'

Build:
  nix develop '.#buck2' --command buck2 build //:yetty
EOF
else
    log "Worker did NOT register within 30s. See $REPO_ROOT/tmp/nativelink-worker.log"
    exit 1
fi
