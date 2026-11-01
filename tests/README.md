# postgres test suite

A 177-assertion test suite for the `postgres` package. The suite is a
self-contained cp3 application that imports the package source directly and
connects to a local PostgreSQL server.

## Run it

```bash
pwsh -File run.ps1
```

This starts a throwaway PostgreSQL cluster in `%TEMP%`, configures a `cp3_test`
database, runs all assertions, and then shuts the cluster down.

Reuse an existing server instead:

```bash
pwsh -File run.ps1 -NoStart -Port 5432 -Database cp3_test
```

Options:

| Flag | Default | Description |
|---|---|---|
| `-PgBin` | searched PATH / PG installs | Path to the PostgreSQL `bin` directory |
| `-DataDir` | `%TEMP%\cp3-postgres-tests\data` | Where a temporary cluster is created |
| `-Port` | `55432` | Server port |
| `-Database` | `cp3_test` | Test database (created if missing) |
| `-User` | `postgres` | Server user |
| `-Password` | (empty) | Server password |
| `-Cp3` | searched PATH | Path to `cp3.cmd` |
| `-NoStart` | `false` | Don't start/stop a server; use one already running |
| `-KeepServer` | `false` | Don't stop the server after the tests |

The test program also reads `CP3_PG_CONNINFO` (a full libpq conninfo string),
which overrides `CP3_PG_HOST`/`_PORT`/`_DB`/`_USER`/`_PASSWORD`.

## Coverage

The tests exercise the package end to end (all against PostgreSQL 17.6):

- **Build information** — `libVersion()`
- **Conninfo** — `conninfo()` quoting and escaping of spaces, quotes, backslashes
- **Connections** — `connect()`/`connectTo()`, `status()`, `isOpen()`,
  `connectionError()`, `lastError()`, `serverVersion()`, invalid connections
- **Result metadata** — `resultStatus()`, `resultError()`, `resultOk()`,
  `command()`, `affected()`, `rowCount()`, `fieldCount()`, `columnName()`,
  `columnIndex()`, `clear()`
- **Row/column access** — `value()`, `valueByName()`, `isNull()`, `isNullByName()`
- **NULL handling** — `NULL` vs empty string distinction
- **DDL / DML** — `CREATE TABLE`, `INSERT`, `UPDATE`, `DELETE`, `execute()`,
  command tags and affected-row counts
- **Parameterized queries** — `query1()`/`query2()`/`query3()` text parameters,
  SQL-injection-shaped data stays data, Unicode parameters
- **SQL errors** — failed and syntax-error results, `resultOk()`, `resultStatus()`
- **Escaping** — `escapeLiteral()` (round-trip, dangerous values),
  `escapeIdentifier()` (whitespace, embedded quotes, Unicode)
- **Transactions** — `BEGIN`/`COMMIT`/`ROLLBACK`, error-and-rollback recovery
- **Empty result sets** — zero rows with live columns
- **Unicode and large values** — multiline text, ~10 KiB strings
- **Defensive handles** — behavior of `0`/unknown connection and result handles
- **Connection lifecycle** — `close()` and idempotent double close

## Caveats for the test harness

- cp3 beta-one does not codegen strict equality (`===`/`!=`, `!==`) on values
  returned by imported functions, so the harness wraps comparisons in local
  helper functions where the parameters have concrete types. Ordering
  comparisons (`<`, `>`) work directly on imported values.
- The suite imports `../../src/index.cp3` directly. Beta-one's package import
  machinery does not expose exported function values, so consumers must vendor
  the package source today.
- `isNull()` returns `true` for out-of-range row/column indices and for cleared
  or unknown result handles; the shim treats "unknown" the same as NULL to keep
  the C API conservative.
- `execute()` returns `-1` for an empty result set **and** for a SQL error,
  since `affected()` cannot distinguish them.
