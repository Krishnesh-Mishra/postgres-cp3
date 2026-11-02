# postgres

A synchronous PostgreSQL client for cp3 built on libpq (tested against libpq 17.6).
It wraps `PGconn*`/`PGresult*` behind integer handles so the API is usable across
cp3 module boundaries on Windows x64.

## Status

This package works end-to-end, but it is built against **cp3 beta one**, which has
several open limitations documented below. It is a working prototype, not a
production client.

- 177 tests pass against PostgreSQL 17.6.
- API is stable for now; the beta compiler may change before a final 1.0.

## Quick start

### Build the package

```bash
# Type-check and pack the package (writes .cp3/out/postgres.cp3pak)
cp3 check src/index.cp3
cp3 build
```

The pack depends on the native tree, which is prebuilt in `native/`. If you edit
the shim (`native/include/postgres_shim.h` / `native/src/postgres_shim.cpp`),
rebuild the static library first:

```bash
pwsh -File native/build.ps1
```

`native/build.ps1` also supports:

- `-Regenerate` — re-run `cp3 native-install` and rewrite the generated binding
  metadata (`index.cp3`/`native.json`) to repository-relative paths;
- `-RebuildImportLib` — rebuild `native/lib/libpq.a`/`libpq.dll.a` from
  `native/runtime/libpq.dll` using MinGW `gendef`/`dlltool`.

### Run the test suite

The suite needs a running PostgreSQL server. The helper starts a throwaway
cluster in `%TEMP%` unless one is already listening:

```bash
# Starts a temp PostgreSQL 17.6 cluster, runs 177 assertions, shuts it down
pwsh -File tests/run.ps1

# Reuse an existing server (no start/stop)
pwsh -File tests/run.ps1 -NoStart -Port 5432 -Database mydb

# Point at your own PostgreSQL bin / port / database
pwsh -File tests/run.ps1 -PgBin "C:\Program Files\PostgreSQL\17\bin" -Port 5433
```

Configuration for the helper: `CP3_PG_HOST`, `CP3_PG_PORT`, `CP3_PG_DB`,
`CP3_PG_USER`, `CP3_PG_PASSWORD`. The test program also accepts a full libpq
conninfo in `CP3_PG_CONNINFO`.

## Public API

All calls use **integer handles and primitive types only** (cp3 beta-one cannot
codegen classes, objects or arrays across module boundaries):

| Function | Description |
|---|---|
| `connect(conninfo: string): number` | Open a connection; handle > 0, or 0 on failure |
| `connectTo(host, port, database, user, password): number` | Same, from fields |
| `conninfo(host, port, database, user, password): string` | Quoted libpq conninfo |
| `close(conn: number): void` | Close a connection (0 / unknown handle is a no-op) |
| `status(conn: number): number` | `ConnStatusType`; -1 for unknown handle |
| `isOpen(conn: number): boolean` | Live and connected |
| `connectionError(conn: number): string` | Last libpq error for the connection |
| `lastError(): string` | Last `connect()` failure on this thread |
| `serverVersion(conn: number): number` | Version like 170006; -1 for unknown |
| `query(conn, sql: string): number` | Run a command; result handle or 0 |
| `query1/2/3(conn, sql, p1, [p2], [p3]): number` | 1–3 text parameters as `$1..$3` |
| `execute(conn, sql: string): number` | DDL/DML; affected rows, or -1 on error/none |
| `resultStatus(res: number): number` | `ExecStatusType`; -1 for unknown handle |
| `resultError(res: number): string` | Error text for the result |
| `resultOk(res: number): boolean` | Handle is live and error-free |
| `command(res: number): string` | Command tag, e.g. `"INSERT 0 1"` |
| `affected(res: number): number` | Rows changed by INSERT/UPDATE/DELETE; -1 for unknown |
| `rowCount(res: number): number` | Rows in the result; -1 for unknown |
| `fieldCount(res: number): number` | Columns; -1 for unknown |
| `columnName(res, col: number): string` | Column name by zero-based index |
| `columnIndex(res, name: string): number` | Column position; -1 if none |
| `value(res, row, col: number): string` | Cell value (empty string for NULL) |
| `valueByName(res, row, name: string): string` | Cell by column name |
| `isNull(res, row, col: number): boolean` | Whether the cell is SQL NULL |
| `isNullByName(res, row, name: string): boolean` | Same by name |
| `clear(res: number): void` | Release a result handle |
| `escapeLiteral(conn, value: string): string` | `PQescapeLiteral` |
| `escapeIdentifier(conn, value: string): string` | `PQescapeIdentifier` |
| `libVersion(): number` | Compiled-in libpq version |

Constants: `CONNECTION_OK` (0), `CONNECTION_BAD` (1), `RESULT_EMPTY_QUERY` (0),
`RESULT_COMMAND_OK` (1), `RESULT_TUPLES_OK` (2), `RESULT_FATAL_ERROR` (7).

## Using the package in an app

Because of cp3 beta-one limitations, the package cannot yet be imported from a
bundled `.cp3pak`. The working path today is to vendor the package source and its
native tree into your app (copy `src/index.cp3`, the `native/` tree, and
`.cp3/native/postgres` into your project), then import the source relatively:

```cp3
import { connect, query, close, rowCount, value, clear } from "../vendor/postgres/src/index.cp3";

const info = env.get("CP3_PG_CONNINFO", { default: "" });
const conn = connect(info);
if (conn > 0) {
  const res = query(conn, "select 42 as answer");
  if (resultOk(res)) {
    console.log("answer:", value(res, 0, 0));
    clear(res);
  }
  close(conn);
}
```

The app must carry its own `tests/.cp3/native/postgres` binding (copied from the
package) with the `native.json` paths pointing at the vendored `native/` tree.
See the "Known beta limitations" section below.

## Design notes

- **Pointer-free boundary.** libpq is full of `PGconn*`/`PGresult*`, which beta
  one rejects from `import native`. The shim (`native/include/postgres_shim.h`)
  owns the pointers and exposes handles plus `std::string` values. cp3
  codegen consumes these directly, so no manual `.c_str()` conversion is needed.
- **Ownership.** `connect()` and `query()` return a handle; `close()` and
  `clear()` release it. An unknown or cleared handle returns the documented
  sentinel values (`-1`, `""`, etc.) rather than crashing. `clear()` is only
  about the result handle — results and connections live independently.
- **Synchronous.** All calls block until the server answers. Run long queries off
  the main event loop.
- **Thread safety.** Handle lookup is mutex-protected, but libpq still requires
  one `PGconn` per thread at a time; that rule is documented, not enforced.
- **Encoding.** libpq sends/receives text in the connection's encoding (usually
  UTF-8), and strings are UTF-8 in the shim, so Unicode round-trips.

## Known beta limitations

1. **No `extern "C"`.** cp3's binder silently skips declarations inside
   `extern "C"`; the shim uses ordinary C++ linkage (Itanium ABI on Windows).
2. **No raw pointers in bindings.** Every libpq pointer lives on the C++ side of
   the shim as a `long long` handle.
3. **Package imports of functions are broken in this compiler build.** An
   installed consumer cannot call a package's exported functions, even after
   `cp3 install`. The workaround today is to vendor the package source and
   import it locally (see "Using the package in an app"), which is why the test
   suite imports `../../src/index.cp3` directly. This also means
   `native.json` must be installed alongside each consuming app — the
   `.cp3pak` itself does not carry native bindings.
4. **Cross-module classes/objects/arrays don't codegen.** Exported function
   return types for classes, object literals and arrays resolve to `unknown`, so
   the public API is strictly primitive handles/strings.
5. **Synchronous blocking** by design, no async.

PostgreSQL's license applies to the vendored runtime DLLs:

```
LICENSE: PostgreSQL License
The PostgreSQL global development group. All rights reserved.
```

## Project layout

```
postgres/
  src/
    index.cp3            # public package API
  native/
    include/
      postgres_shim.h    # shim header (passed to cp3 native-install)
      postgres/
        libpq-fe.h       # vendored for the shim build
        postgres_ext.h
        pg_config_ext.h
    src/
      postgres_shim.cpp  # libpq handle implementation
    lib/
      libpostgres_shim.a
      libpq.a            # MinGW import library for -lpq
      libpq.dll.a
      libpq.def
    runtime/
      libpq.dll, libssl-3-x64.dll, libcrypto-3-x64.dll,
      libintl-9.dll, libiconv-2.dll, libwinpthread-1.dll
    build.ps1            # rebuild shim (+binding regeneration)
    native.json          # generated binding metadata (project-relative paths)
  tests/
    src/
      main.cp3           # 177-assertion test suite
    run.ps1              # starts a temp PostgreSQL cluster and runs tests
    package.json
    .cp3/native/postgres/  # copy of the generated binding
  package.json           # "type": "package", main: "src/index.cp3"
  README.md
```

## License

The package source and shim are MIT licensed; see [LICENSE](LICENSE).

The binaries in `native/runtime/` are vendored from the PostgreSQL 17.6
Windows distribution and retain their upstream licenses. libpq is distributed
under the PostgreSQL License; OpenSSL, libintl, libiconv and the MinGW runtime
DLLs retain their respective upstream licenses.
