# Buck2 build with NativeLink remote cache

Yetty's Buck2 build routes actions through a shared remote-cache server
(NativeLink) so that cmake/autotools third-party builds and genrules get
served from cache on every machine after the first run.

## TL;DR

```sh
# One-time on the cache-server host:
./buck-build-tools/setup-server.sh

# On every dev machine, from the repo root:
nix develop '.#buck2' --command buck2 build //:yetty
```

First build on a fresh cache: ~2 min. Subsequent `buck2 clean && buck2 build`: ~2 s.

## Components

```
    dev machine                              cache-server host
    -----------                              -----------------
  buck2 (inside nix develop)   ─ gRPC ─▶   CAS        :50051
                               ─ gRPC ─▶   scheduler  :50052
                                       ▲
                                       │ worker API :50061
                                       │
                                    nativelink worker
                                    (runs inside nix develop,
                                     executes actions on host)
```

- **CAS + AC** (content-addressable store + action cache): stores action
  inputs, outputs, and `action_digest → result` mappings. NativeLink binary
  serving `grpc://HOST:50051`.
- **Scheduler**: matches actions to workers. `grpc://HOST:50052`, worker API
  on `:50061`.
- **Worker**: executes actions. Must run inside `nix develop '.#buck2'` so it
  has the nix-provided toolchain (clang, cmake, pkg-config, …) on `PATH`.

## Target list

All targets are defined in the root `BUCK`. Useful ones:

| Target | What |
|---|---|
| `//:yetty` | Main binary (Linux desktop) |
| `//:ycore` | Core library (smallest unit for testing) |
| `//...` | Everything |
| `//buck-build-tools/third_party:...` | Third-party deps (freetype, glfw, libuv, cmake genrules) |

Commands:

```sh
nix develop '.#buck2' --command buck2 build //:yetty                  # build
nix develop '.#buck2' --command buck2 build //:yetty --show-output    # print path
nix develop '.#buck2' --command buck2 run //:yetty                    # build + run
nix develop '.#buck2' --command buck2 targets //...                   # list all
nix develop '.#buck2' --command buck2 clean                           # wipe buck-out
nix develop '.#buck2' --command buck2 kill                            # stop daemon
```

The built executable lands at:

```
buck-out/v2/gen/root/<hash>/__yetty__/yetty
```

Use `--show-output` to get the exact path.

## One-time server setup

On the machine that will host the cache for the team (can be the same dev
machine or a dedicated one), run:

```sh
./buck-build-tools/setup-server.sh
```

This:

1. Pulls the NativeLink docker image.
2. Writes `tmp/nativelink/{docker-compose.yml,local-storage-cas.json5,scheduler.json5,worker.json5}`.
3. Starts CAS + scheduler via docker-compose.
4. Extracts the nativelink binary from the image to `tmp/nativelink/nativelink`.
5. Captures the nix-develop environment into `tmp/nativelink/action-env.sh`
   and writes the entrypoint wrapper `tmp/nativelink/action-entrypoint.sh`.
6. Starts the host-side worker inside `nix develop '.#buck2'`.

After the script completes, verify:

```sh
docker-compose -f tmp/nativelink/docker-compose.yml ps    # 2 containers up
pgrep -af 'nativelink worker.json5'                        # worker process
```

## Dev machine setup

If `HOST` is the cache-server IP (loopback if you run both on the same box),
point `.buckconfig` at it:

```ini
[buck2_re_client]
engine_address = grpc://HOST:50052
action_cache_address = grpc://HOST:50051
cas_address = grpc://HOST:50051
instance_name =
tls = false
```

The current in-repo `.buckconfig` already points at `127.0.0.1` — edit the
three addresses above to use the shared host's IP for multi-machine setups.

## Config tuning

Execution platform: `buck-build-tools/platforms/defs.bzl`. Key flags:

| Flag | Value | Effect |
|---|---|---|
| `use_limited_hybrid` | `True` | Check cache, run remote, fall back to local |
| `allow_limited_hybrid_fallbacks` | `True` | Allow the fallback path |
| `allow_hybrid_fallbacks_on_failure` | `True` | Local retry when remote errors |
| `allow_cache_uploads` | `True` | Upload local action results to AC |
| `max_cache_upload_mebibytes` | `1024` | Per-output upload cap |

Worker-side env propagation: `tmp/nativelink/action-env.sh` is regenerated
by `setup-server.sh` from the current `nix develop '.#buck2'` env. Re-run
the script after any `flake.nix` change that affects toolchain versions.

**Everything under `tmp/nativelink/` is generated** by `setup-server.sh`
from the upstream NativeLink config plus documented patches in the script.
Don't hand-edit those files — edit `setup-server.sh` and re-run. That's
what keeps this reproducible.

## Troubleshooting

- **`buck2 build` times out / queues forever** — worker not registered.
  Check `pgrep -af 'nativelink worker'` and the tail of
  `tmp/nativelink-worker.log` for `Worker registered`.
- **Action fails with `cc: cannot execute 'cc1'`** — worker started outside
  `nix develop`. Kill it and re-run with `nix develop '.#buck2' --command ./nativelink worker.json5`.
- **`CMake Error: Could not find CMAKE_ROOT`** — same as above; PATH not
  propagated. Regenerate `action-env.sh` via `setup-server.sh`.
- **Cache never grows** — check AC: `sudo find /var/lib/docker/volumes/nativelink_nativelink_cache/_data/content_path-ac -type f | wc -l`. If zero, buck2
  isn't reaching the CAS — verify `.buckconfig` addresses and `docker-compose ps`.
- **Cached failed results stick** — NativeLink caches non-zero-exit results.
  Wipe to force re-execution:
  ```sh
  cd tmp/nativelink && docker-compose stop
  sudo rm -rf /var/lib/docker/volumes/nativelink_nativelink_cache/_data/{content_path-cas,content_path-ac,tmp_path-cas,tmp_path-ac}
  docker-compose up -d
  ```

## Known limitations

- Worker must run inside `nix develop` — no systemd unit yet.
- ~20 buck2-internal actions (symlinked_dir, write) always run local; they
  don't cache. Not worth pursuing: they take milliseconds each.
- c_compile actions are cached via AC but for pure compile fan-out, plain
  ccache/distcc may still be preferable on laptops.

## References

- [NativeLink](https://docs.nativelink.com/)
- [Buck2 CommandExecutorConfig](https://buck2.build/docs/api/build/CommandExecutorConfig/)
- [Buck2 Remote Execution](https://buck2.build/docs/users/remote_execution/)
- REv2 spec: [build.bazel.remote.execution.v2](https://github.com/bazelbuild/remote-apis/blob/main/build/bazel/remote/execution/v2/remote_execution.proto)
