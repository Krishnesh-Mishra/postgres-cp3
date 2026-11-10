# postgres

An **asynchronous** PostgreSQL client for cp3 built on libpq (tested against
libpq 17.6 headers; runtime DLLs vendored from PostgreSQL 17.x).

- All I/O is driven by cp3's async runtime — nothing blocks the event loop.
- libpq is wrapped behind integer handles so the API is usable across cp3
  module boundaries on Windows x64.
- Parameters are packed as length-prefixed wire strings; results are read
  through free functions.
- Server-side prepared statements, binary result formats, query cancellation
  (`pg_cancel_backend` / `PQcancel`), and a connection pool.

> **Status of this build:** the package builds, links and packs end to end
> (`cp3 build` → `postgres.cp3pak`). Because the authoring environment could
> not run a PostgreSQL server (see "Known limitations"), the server-dependent
> test stages are validated only up to the connection probe — the suite fails
> fast with `could not connect` when no server is listening. Claims about
> throughput or latency need a live PostgreSQL; the benchmark suite
> (`benchmark/`) is provided for that.

## Quick start

```cp3
import { env } from "std/env";
import {
  connect, queryParams, close, valueByName, clear, textParam,
} from "postgres";

const conn = await connect(env.get("CP3_PG_CONNINFO", { default: "" }));
if (conn > 0) {
  const res = await queryParams(conn, "select $1::text as greeting",
                                textParam("hello"));
  console.log("greeting:", valueByName(res, 0, "greeting"));
  clear(res);
  close(conn);
}
```

A complete example (mirrors `benchmark/cp3/bench.cp3`):

```cp3
import {
  connect, close, queryParams, clear, valueByName,
  textParam, poolCreate, poolQuery, poolEnd,
} from "postgres";

const conn = await connect("host=127.0.0.1 port=55432 dbname=cp3_test user=postgres");
if (conn === 0) {
  throw new Error("could not connect");
}

const res = await queryParams(conn, "select $1::text as greeting", textParam("hello"));
console.log("greeting:", valueByName(res, 0, "greeting"));
clear(res);
close(conn);

// Pool of up to 16 connections:
const pool = poolCreate("host=127.0.0.1 port=55432 dbname=cp3_test user=postgres", 16, 0);
const pres = await poolQuery(pool, "select 1 as one");
clear(pres);
await poolEnd(pool);
```

## Build the package

```bash
# Type-check and pack the package (writes .cp3/out/postgres.cp3pak)
cp3 check src/index.cp3
cp3 build
```

The pack depends on the native tree, which is prebuilt in `native/`. If you
edit the shim (`native/include/postgres_shim.h` / `native/src/*.cpp`),
rebuild the static library first:

```bash
pwsh -File native/build.ps1 -Regenerate
```

This also re-runs `cp3 native-install` and rewrites the generated binding
metadata to repository-relative paths. `-RebuildImportLib` rebuilds
`native/lib/libpq.a` from `native/runtime/libpq.dll` via MinGW gendef/dlltool.

## Run the test suite

The suite needs a running PostgreSQL server. The helper starts a throwaway
cluster in `%TEMP%` unless one is already listening (it tries
`initdb`/`pg_ctl` from a PostgreSQL 17 binary directory you pass with
`-PgBin` or that it finds on PATH):

```bash
pwsh -File tests/run.ps1                      # throwaway cluster, dropped at the end
pwsh -File tests/run.ps1 -NoStart -Port 5432  # reuse a running server
```

Or with Docker (this is how the current results were produced — expects no
other containers; `postgres:17` from Docker Hub):

```bash
docker run -d --name postgres-test-server \
  -p 55432:5432 \
  -e POSTGRES_HOST_AUTH_METHOD=trust \
  -e POSTGRES_DB=cp3_test \
  postgres:17

pwsh -File tests/run.ps1 -NoStart -Port 55432 -User postgres
# or directly: pwsh -File benchmark/run.py  (uses 127.0.0.1:55432 cp3_test)
docker stop postgres-test-server && docker rm postgres-test-server
```

Configuration: `CP3_PG_HOST`, `CP3_PG_PORT`, `CP3_PG_DB`, `CP3_PG_USER`,
`CP3_PG_PASSWORD`, or a full libpq conninfo in `CP3_PG_CONNINFO` (wins).

The suite is a pipeline of stages (`tests/src/stages/*.cp3`) behind
`tests/src/main.cp3`. Stages that need a live server open their own
connections; a failing stage logs `not ok - ...` and the pipeline continues.
Without a server the suite fails fast with `could not connect` instead of
hanging (the default `connect_timeout` is 2 s in the test config).

## Public API

All calls use **integer handles and primitive types only** (cp3 v0.2.0 cannot
codegen classes or arrays across module boundaries). Everything that touches
the server is async; pure inspection of a result handle is synchronous.

### Connections

| Function | Description |
|---|---|
| `connect(conninfo: string): Promise<number>` | Open a connection; handle > 0, or 0 on failure |
| `connectTo(host, port, database, user, password): Promise<number>` | Same, from fields |
| `conninfo(host, port, database, user, password): string` | Quoted libpq conninfo |
| `conninfoString(host, port, database, user, password): string` | Alias for `conninfo` |
| `quoteConninfoField(value: string): string` | libpq-quoted conninfo field |
| `close(conn: number): undefined` | Close a connection (0 / unknown is a no-op) |
| `status(conn: number): number` | `ConnStatusType`; -1 for unknown handle |
| `isOpen(conn: number): boolean` | Live and connected |
| `connectionError(conn: number): string` | Last libpq error for the connection |
| `lastError(): string` | Last `connect()` failure on this thread |
| `serverVersion(conn: number): number` | Version like 170006; -1 for unknown |
| `escapeLiteral(conn, value): string` | `PQescapeLiteral` |
| `escapeIdentifier(conn, value): string` | `PQescapeIdentifier` |

### Queries

| Function | Description |
|---|---|
| `query(conn, sql): Promise<number>` | Simple protocol; result handle or 0 |
| `queryTimeout(conn, sql, ms): Promise<number>` | Same, with a client-side `statement_timeout` (PQcancel watchdog) |
| `queryParams(conn, sql, packed): Promise<number>` | Extended protocol; `packed` from param factories |
| `queryParamsBinary(conn, sql, packed): Promise<number>` | Extended protocol, binary result format |
| `queryParamsTimeout(conn, sql, packed, ms): Promise<number>` | queryParams with a client-side `statement_timeout` |
| `prepare(conn, name, sql, nparams): Promise<boolean>` | Create a named server-side prepared statement |
| `executePrepared(conn, name, packed): Promise<number>` | Execute it |
| `executePreparedBinary(conn, name, packed): Promise<number>` | Execute with binary format |
| `execute(conn, sql): Promise<number>` | DDL/DML; affected rows or -1 on failure |
| `executeParams(conn, sql, packed): Promise<number>` | Same with parameters |
| `transaction(conn, fn): Promise<undefined>` | `BEGIN` / `COMMIT` (or `ROLLBACK` on throw) around `fn(conn)` |

Parameters are `string`s produced by the factories in `params.cp3` and joined
with single spaces:

```cp3
import { textParam, intParam, boolParam, nilParam, pack2, pack3 } from "postgres";

textParam("a string");           // any value, UTF-8, may contain spaces/NULs
intParam(42);                     // String(number); > 2^53 loses precision
boolParam(true);                  // "true" / "false"
nilParam();                       // SQL NULL
pack2(textParam("a"), intParam(7)); // "$1 $2" wire segments
```

### Result inspection (synchronous)

| Function | Description |
|---|---|
| `resultStatus(res): number` | `ExecStatusType`; -1 for unknown |
| `resultError(res): string` | Error text (`""` on success) |
| `resultOk(res): boolean` | Live and error-free |
| `resultDiag(res, field: number): string` | `PQresultErrorField` by single-char code (67=`'C'` sqlstate, 83=`'S'` severity, 77=`'M'` message, 80=`'P'` position) |
| `resultSqlstate(res): string` | SQLSTATE or `""` |
| `resultSeverity(res): string` | Severity or `""` |
| `command(res): string` | Command tag, e.g. `"INSERT 0 1"` |
| `affected(res): number` | Rows changed by INSERT/UPDATE/DELETE; -1 unknown |
| `rowCount(res): number` | Rows returned; -1 unknown |
| `fieldCount(res): number` | Columns; -1 unknown |
| `columnName(res, col): string` / `columnIndex(res, name): number` / `fieldOid(res, col): number` | Column metadata |
| `value(res, row, col): string` / `valueByName(res, row, name): string` | Cell text (`""` for NULL) |
| `valueBinary(res, row, col): string` / `valueBinaryByName(res, row, name): string` | OID-decoded binary cell |
| `isNull(res, row, col): boolean` / `isNullByName(res, row, name): boolean` | NULL test |
| `clear(res: number): undefined` | Release a result handle |

Constants: `CONNECTION_OK`, `CONNECTION_BAD`, `RESULT_EMPTY_QUERY`,
`RESULT_COMMAND_OK`, `RESULT_TUPLES_OK`, `RESULT_FATAL_ERROR`,
`RESULT_PIPELINE_SYNC`, `RESULT_PIPELINE_ABORTED`.

### Connection pool

A single pool per process (compiler limitation, see below). `poolCreate`
always returns `1`.

| Function | Description |
|---|---|
| `poolCreate(conninfo, max, connectionTimeoutMillis): number` | `max` live connections (0 → 10); `connectionTimeoutMillis` for waiting acquisitions (0 → wait forever) |
| `poolSize(poolId)` / `poolPending(poolId)` | Live / checked-out connection counts |
| `poolQuery(poolId, sql): Promise<number>` | Simple query on the next available client |
| `poolQueryParams(poolId, sql, packed): Promise<number>` | Extended protocol |
| `poolQueryParamsBinary(poolId, sql, packed): Promise<number>` | Binary format |
| `poolExecute(poolId, sql)` / `poolExecuteParams(poolId, sql, packed)` | DDL/DML; affected rows |
| `poolAcquire(poolId)` / `poolRelease(poolId, handle)` | Explicit checkout/checkin for custom use |
| `poolEnd(poolId): Promise<undefined>` | Close idle connections, wait for in-flight work |

All `poolQuery*` calls serialize per-connection internally; waiting
acquisitions are parked FIFO and hand out connections in order. Cancellation
of a running statement uses `PQcancel` on a dedicated connection — either
out-of-band (another connection calls `pg_cancel_backend`, see the
cancellation stage of the test suite) or client-side via
`queryTimeout`/`queryParamsTimeout`.

## Benchmarks

`benchmark/` has the same five workloads against libpq (C), this package
(cp3) and node-postgres (`pg`). See [benchmark/README.md](benchmark/README.md).

```powershell
python benchmark/run.py                          # against a running server
python benchmark/run.py --start-pg --pg-bin "<pg 17 bin>"   # throwaway cluster
```

## Design notes

- **Pointer-free boundary.** libpq's `PGconn*`/`PGresult*` live on the C++
  side of the shim as `long long` handles; cp3 never sees them.
- **Ownership.** `connect()`/`query()` return handles; `close()`/`clear()`
  release them. Unknown handles return documented sentinels (`-1`, `""`,
  `false`) instead of crashing. Results and connections are independent.
- **Async.** Everything that waits on the server is `async`; the polling
  wait is timer-driven (5 ms) on top of libpq's non-blocking API. cp3 v0.2.0's
  libuv/IOCP reactor does not wake for TCP-connect-pending sockets on this
  build, so waiting on `PQconnectPoll` cannot rely on the reactor alone.
- **Statement timeouts.** `queryTimeout`/`queryParamsTimeout` cancel the
  statement from a dedicated connection (PQcancel) when it overruns; the
  cancelled statement reports its own error result with SQLSTATE 57014.
- **Wire-format parameters.** `v:<base64-of-bytes>` or `n` for NULL,
  space-separated, with explicit length — arbitrary bytes round-trip.
- **Binary results.** `queryParamsBinary` decodes bool/int2/int4/int8/float4/
  float8/bytea and passes text/varchar/json/jsonb through as bytes.
- **Thread safety.** Handle lookup is mutex-protected; one `PGconn` at a time
  remains libpq's rule.

## Known limitations (cp3 v0.2.0, this build)

1. **No cross-module class codegen.** A class imported from another module is
   opaque: it cannot be constructed, have methods called, or have fields read.
   The whole public API is therefore free functions over integer handles —
   the same design the original v0.1.0 used.
2. **No arrays anywhere.** `T[]`/`Array<T>` emit a `#include
   "cp3/typed_array.hpp"` that does not exist in this install, so arrays and
   `for...of` are unusable; row data is read through `value()`/`isNull()`
   accessors instead of materialized row objects.
3. **Package consumers can't import from the `.cp3pak` yet.** The working
   consumption path is to vendor the package source + native tree (like
   `tests/` and `benchmark/cp3/` do), or install the pack with `cp3 install`
   once that path is fixed upstream.
4. **Single pool per process** (`poolCreate` always returns 1).
5. **Pipeline mode** is wired in the shim (`cp3pg_enter_pipeline`,
   `cp3pg_pipeline_sync`, …) but not exposed: it cannot return an array of
   results across the module boundary, and per-statement callback delivery
   does not codegen either.
6. **Errors are not exceptions.** `instanceof Error` in `catch` emits an
   unresolved runtime symbol; the idiom is `err.message`.
7. The server DLLs are x64 Windows builds; POSIX is not supported yet.

PostgreSQL's license applies to the vendored runtime DLLs:

```
LICENSE: PostgreSQL License
The PostgreSQL global development group. All rights reserved.
```

## Project layout

```
postgres/
  src/
    index.cp3            # public API re-exports
    client.cp3           # connections + queries (async)
    pool.cp3             # the connection pool
    result.cp3           # result inspection + diag
    params.cp3           # parameter factories (textParam, intParam, ...)
    conninfo.cp3         # conninfo string construction
    types.cp3            # libpq status constants
  native/
    include/
      postgres_shim.h    # shim header (passed to cp3 native-install)
      postgres/          # vendored libpq headers
    src/
      connections.cpp    # connection lifecycle, reactor sockets
      queries.cpp        # query dispatch, pipeline mode
      results.cpp        # result inspection, value decoders
      cancel.cpp         # PQcancel
      shim_internal.hpp  # shared state helpers
    lib/                 # libpostgres_shim.a, libpq import lib
    runtime/             # vendored DLLs
    build.ps1            # rebuild shim (+binding regeneration)
  tests/
    src/                 # staged suite (main.cp3 + stages/*.cp3)
    run.ps1              # throwaway cluster + suite runner
  benchmark/
    cpp/  cp3/  node/    # libpq, this package, node-postgres
    run.py               # runs all three, prints a table
  package.json           # "type": "package", main: "src/index.cp3"
```

## License

The package source and shim are MIT licensed; see [LICENSE](LICENSE).

The binaries in `native/runtime/` are vendored from the PostgreSQL 17.x
Windows distribution and retain their upstream licenses. libpq is distributed
under the PostgreSQL License; OpenSSL, libintl, libiconv and the MinGW runtime
DLLs retain their respective upstream licenses.
