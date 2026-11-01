// postgres_shim.h - pointer-free API over libpq for cp3 `import native`.
//
// cp3 beta-one rejects every declaration that takes or returns a raw pointer,
// and libpq's public API is built almost entirely from `PGconn*` and
// `PGresult*`. This shim keeps those pointers on the C++ side and exposes
// integer handles plus `std::string` values, which `cp3 native-install` can
// bind and `import native "postgres"` can call.
//
// Ownership:
//   * a connection handle is created by cp3pg_connect and released by
//     cp3pg_close;
//   * a result handle is created by cp3pg_exec* and released by cp3pg_clear;
//   * returned strings are owned by the caller (they are copied into a cp3
//     string by the generated code).
//
// Handles are process-local and are never valid across processes.

#ifndef CP3_POSTGRES_SHIM_H
#define CP3_POSTGRES_SHIM_H

#include <string>

// The declarations use ordinary C++ linkage on purpose: cp3's beta-one
// binder skips every declaration inside an `extern "C"` block. cp3's
// generated C++ is compiled with the same Itanium ABI (Clang/GCC on
// Windows, Linux and macOS), so the mangled names match this shim.

// Connection lifecycle -------------------------------------------------------

// Connect with a libpq conninfo string ("host=... dbname=... user=...").
// Returns a positive handle on success, or 0 on failure. On failure the
// reason is available from cp3pg_last_error().
long long cp3pg_connect(const std::string& conninfo);

// Close a connection and free its PGconn. Unknown handles are ignored.
void cp3pg_close(long long conn);

// libpq ConnStatusType of a live handle: 0 = CONNECTION_OK, 1 = CONNECTION_BAD.
// Returns -1 for an unknown handle.
int cp3pg_status(long long conn);

// Last error message recorded on this connection (empty when there is none).
std::string cp3pg_error(long long conn);

// Error from the most recent failed cp3pg_connect on this thread.
std::string cp3pg_last_error();

// Query execution ------------------------------------------------------------

// Run a SQL command. Returns a result handle, or 0 when libpq could not
// produce a result (the reason is then in cp3pg_last_error()).
// The result handle must be released with cp3pg_clear.
long long cp3pg_exec(long long conn, const std::string& sql);

// Same as cp3pg_exec with one to three text parameters ($1..$3). Parameters
// are sent in text format; use cp3pg_exec for statements without parameters.
long long cp3pg_exec1(long long conn, const std::string& sql,
                      const std::string& p1);
long long cp3pg_exec2(long long conn, const std::string& sql,
                      const std::string& p1, const std::string& p2);
long long cp3pg_exec3(long long conn, const std::string& sql,
                      const std::string& p1, const std::string& p2,
                      const std::string& p3);

// Result inspection ----------------------------------------------------------

// libpq ExecStatusType: 1 = COMMAND_OK, 2 = TUPLES_OK, 7 = FATAL_ERROR, ...
// Returns -1 for an unknown handle.
int cp3pg_result_status(long long res);
std::string cp3pg_result_error(long long res);
std::string cp3pg_cmd_status(long long res);
std::string cp3pg_cmd_tuples(long long res);
int cp3pg_affected(long long res);
int cp3pg_ntuples(long long res);
int cp3pg_nfields(long long res);
std::string cp3pg_fname(long long res, int col);
int cp3pg_fnumber(long long res, const std::string& name);
std::string cp3pg_getvalue(long long res, int row, int col);
int cp3pg_getisnull(long long res, int row, int col);
void cp3pg_clear(long long res);

// Escaping -------------------------------------------------------------------

// Quote a string as a SQL literal (including the surrounding quotes).
std::string cp3pg_escape_literal(long long conn, const std::string& value);
std::string cp3pg_escape_identifier(long long conn, const std::string& value);

// Build information ----------------------------------------------------------

// libpq's compiled-in version, e.g. 170006 for 17.6.
int cp3pg_lib_version();
int cp3pg_server_version(long long conn);

#endif  // CP3_POSTGRES_SHIM_H
