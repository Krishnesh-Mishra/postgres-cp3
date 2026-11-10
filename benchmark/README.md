# Benchmarks

The same five workloads measured against three PostgreSQL clients on one
machine, one server, one configuration:

| workload | description |
| --- | --- |
| `select1` | 1000 x `select 1` |
| `series` | 100 x `select * from generate_series(1, 1000)`, drained row by row |
| `insert500` | 10 x (`BEGIN`; one multi-VALUES `INSERT` of 500 rows; `COMMIT`) = 5000 rows |
| `prepared` | 1000 x execution of a server-side prepared `select $1::int` |
| `pool16` | 16 concurrent `select pg_sleep(0.1)` through a 16-way concurrency limit — should take ≈ 0.1 s |

Clients:

* `cpp/` — raw libpq (the in-process C API). The floor for any client.
* `cp3/` — this package, via the `src/` tree in the repository.
* `node/` — node-postgres (`pg`), the reference implementation node-pg-style
  APIs aim at.

## Requirements

* PostgreSQL 17 server, reachable on `127.0.0.1:55432`, database `cp3_test`,
  user `postgres` (override with `CP3_PG_*` / `CP3_PG_CONNINFO`).
* `g++` (or any C++17 compiler) for the `cpp` benchmark — links the bundled
  `native/lib/libpq.a`.
* `cp3` on PATH for the `cp3` benchmark.
* `node` + `npm` for the `node` benchmark (`pg` is installed into
  `benchmark/node/node_modules` on first run; use `--skip-npm` to opt out).

## Running

Against an already-running server (default `127.0.0.1:55432`):

```powershell
python benchmark/run.py
```

With a throwaway cluster created just for the run (uses a PostgreSQL 17
binary distribution; `--pg-bin` points at its `bin` directory):

```powershell
python benchmark/run.py --start-pg --pg-bin "C:\path\to\pgsql\bin"
```

Environment for the benchmarks themselves:

```
CP3_PG_HOST  CP3_PG_PORT  CP3_PG_DB  CP3_PG_USER  CP3_PG_PASSWORD  CP3_PG_CONNINFO
```

The throwaway cluster is initialized with `--locale=C`, trust authentication,
TCP on 127.0.0.1:55432, and is dropped when the run finishes.

## Output

A terminal table plus the raw `<client> <workload> <ops> <ms> <us/op>` lines.

## Notes

* The connection pool is single-pool/one-pool-per-process in this build of
  cp3 (see `src/pool.cp3`); `pool16` uses `max = 16`.
* `pool16` measures wall time for the whole 16-way burst. A value close to
  `select pg_sleep(0.1)`'s runtime means the pool actually ran 16 clients at
  once; a value near 16x that means serialization somewhere.
* Row access in `cp3/` benchmarks reads every row through the accessor API,
  same as a real application would.
