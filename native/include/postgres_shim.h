// postgres_shim.h - pointer-free API over libpq for cp3 `import native`.
//
// cp3 cannot bind raw pointers (`PGconn*`/`PGresult*`), so this shim keeps
// them on the C++ side and exposes integer handles plus `std::string` values.
//
// Async by default. The connection and query paths are non-blocking and are
// driven by cp3's `std/io` reactor (`io.pollUntilDone`): the shim exposes a
// socket plus a `checkProgress` function per operation; only the awaiting
// async function suspends. See native/src/queries.cpp and
// src/client.cp3 for the consumption pattern:
//
//   await io.pollUntilDone(() => pg.cp3pg_socket(h), () => pg.cp3pg_query_progress(h));
//   while ((res = pg.cp3pg_get_result(h)) !== 0) { ... }
//
// Result consumption rules:
//   * non-pipelined queries: one result per statement; `get_result` returns 0
//     when the command is complete (a multi-statement simple query yields
//     several results before that 0).
//   * pipelined queries: one result per queued statement, terminated by a
//     `PGRES_PIPELINE_SYNC` result (status 10), after which `get_result`
//     returns 0.
//
// Ownership:
//   * a connection handle is created by cp3pg_connect_start and released by
//     cp3pg_close;
//   * a result handle is created by cp3pg_get_result and released by
//     cp3pg_clear;
//   * a cancel handle is created by cp3pg_cancel_start and released by
//     cp3pg_cancel_finish;
//   * returned strings are owned by the caller.
//
// Handles are process-local and are never valid across processes.
//
// Thread rule (libpq requirement): one PGconn must not be used from two
// threads at once. c3's client serializes calls per connection and never
// closes a handle with an operation in flight.

#ifndef CP3_POSTGRES_SHIM_H
#define CP3_POSTGRES_SHIM_H

#include <string>

// The declarations use ordinary C++ linkage on purpose: cp3's binder skips
// every declaration inside an `extern "C"` block. cp3 compiles generated C++
// with the same Itanium ABI (Clang/GCC on Windows, Linux and macOS).

// Connection lifecycle -------------------------------------------------------

// Begin a non-blocking connection. Returns a handle, or 0 on immediate
// failure (see cp3pg_last_error()). Drive to completion with
// cp3pg_connect_progress() and cp3pg_socket().
long long cp3pg_connect_start(const std::string& conninfo);

// io.pollUntilDone checkProgress for an in-flight connection:
//   0 = POLL_DONE (connected), 1 = POLL_READING, 2 = POLL_WRITING,
//   -1 = POLL_FAILED (error is on the connection).
int cp3pg_connect_progress(long long conn);

// Socket of a connection for polling (fd on POSIX, SOCKET on Windows).
// Returns -1 for an unknown handle or before one is assigned.
int cp3pg_socket(long long conn);

// Close a connection and free its PGconn. Unknown handles are ignored.
// Do not close while an operation is in flight on the handle.
void cp3pg_close(long long conn);

// libpq ConnStatusType of a live handle: 0 = CONNECTION_OK, 1 = CONNECTION_BAD.
// Returns -1 for an unknown handle.
int cp3pg_status(long long conn);

// Last error message recorded on this connection (empty when there is none).
std::string cp3pg_error(long long conn);

// Error from the most recent failed native call on this thread.
std::string cp3pg_last_error();

// Query execution -------------------------------------------------------------

// Send a simple-protocol query (no parameters). Returns 0 on success, -1 on
// immediate failure (see cp3pg_last_error()). Not permitted in pipeline mode;
// use cp3pg_send_query_params instead.
int cp3pg_send_query(long long conn, const std::string& sql);

// Send an extended-protocol query.
// `packed` is a base64 encoding of the parameters:
//     segment   := "v:" <base64-of-value-bytes> | "n"
//     packed    := segment (" " segment)*
// "n" is a NULL parameter. The decoded bytes are sent with an explicit
// length, so values may contain spaces and zero bytes. Parameters are always
// sent in text format. Pass an empty string for zero parameters.
// `result_format` is 0 for text results, 1 for binary results.
int cp3pg_send_query_params(long long conn, const std::string& sql,
                            const std::string& packed, int result_format);

// Prepared statements. `name` may be empty (unnamed). nparams is required up
// front because libpq does not infer it for prepares.
int cp3pg_send_prepare(long long conn, const std::string& name,
                       const std::string& sql, int nparams);
// Send an already-prepared statement. `packed` uses the same encoding as
// cp3pg_send_query_params. `result_format` as above.
int cp3pg_send_query_prepared(long long conn, const std::string& name,
                              const std::string& packed, int result_format);

// Flush pending output; returns 1 when the fd needs to become writable
// before everything is sent, 0 when the send buffer is empty, -1 on error.
int cp3pg_flush(long long conn);

// Reactor entry point for in-flight queries. Calls PQflush / PQconsumeInput /
// PQisBusy in order and reports the io.pollUntilDone codes:
//   0 = POLL_DONE (results are ready to drain with cp3pg_get_result),
//   1 = POLL_READING,
//   2 = POLL_WRITING,
//   -1 = POLL_FAILED.
int cp3pg_query_progress(long long conn);

// Pipeline mode ----------------------------------------------------------------

int cp3pg_enter_pipeline(long long conn);  // 0 ok / -1
int cp3pg_exit_pipeline(long long conn);   // 1 ok / 0
// Send a Sync and flush: terminates the batch. Returns 1 on success, 0 otherwise.
int cp3pg_pipeline_sync(long long conn);
// 0 = OFF, 1 = ON, 2 = ABORTED, -1 for an unknown handle.
int cp3pg_pipeline_status(long long conn);

// Consume and return the next result handle, or 0 when no more results are
// ready. Call after cp3pg_query_progress reports POLL_DONE, then keep calling
// until it returns 0. PGRES_PIPELINE_SYNC results arrive here too.
long long cp3pg_get_result(long long conn);

// Result diagnostic fields (PQresultErrorField), as raw single chars:
// 'S' severity, 'V' severity (non-localized), 'C' sqlstate code,
// 'M' message, 'D' detail, 'H' hint, 's' schema, 't' table, 'c' column,
// 'n' constraint, 'P' position, 'F' file, 'L' line, 'R' routine.
// Returns "" when the result handle is unknown or the field is absent.
std::string cp3pg_result_diag(long long res, int field);
int cp3pg_result_status(long long res);
std::string cp3pg_result_error(long long res);
std::string cp3pg_cmd_status(long long res);
// Rows changed by INSERT/UPDATE/DELETE; rows in the result for other commands;
// -1 for an unknown handle.
int cp3pg_affected(long long res);
int cp3pg_ntuples(long long res);
int cp3pg_nfields(long long res);
std::string cp3pg_fname(long long res, int col);
int cp3pg_fnumber(long long res, const std::string& name);
// Column OID of a result column (PQftype), -1 for an unknown result handle.
int cp3pg_ftype(long long res, int col);
// Column value with text semantics: booleans as "true"/"false", everything
// else as the server sent it. SQL NULL and out-of-range indices yield "" —
// use cp3pg_getisnull to distinguish.
std::string cp3pg_getvalue(long long res, int row, int col);
// Column value decoded per the column's OID, rendered as UTF-8 text:
//   int2/int4/int8: decimal; float4/float8: shortest round trip;
//   bool: "true"/"false"; text/varchar/json(b): raw bytes;
//   bytea: base64. Unknown OIDs are returned as raw bytes.
// Use for binary-format results.
std::string cp3pg_getvalue_decoded(long long res, int row, int col);
int cp3pg_getisnull(long long res, int row, int col);
void cp3pg_clear(long long res);

// Escaping -------------------------------------------------------------------

// Quote a string as a SQL literal (including the surrounding quotes).
std::string cp3pg_escape_literal(long long conn, const std::string& value);
// Quote an identifier (table, column, ...), including the surrounding quotes.
std::string cp3pg_escape_identifier(long long conn, const std::string& value);

// Cancellation ----------------------------------------------------------------
//
// Creates a cancel handle tied to the given connection; the cancel is
// initiated immediately with cp3pg_cancel_start. Poll the cancel connection
// with the same io.pollUntilDone reactor (cp3pg_cancel_socket /
// cp3pg_cancel_progress), then release with cp3pg_cancel_finish.
// Returns a cancel handle, or 0 on failure. `conn` must be a live
// connection handle; the cancel connection is independent of it.
long long cp3pg_cancel_start(long long conn);
// io.pollUntilDone checkProgress for an in-flight cancel:
//   0 = POLL_DONE, 1 = POLL_READING, 2 = POLL_WRITING, -1 = POLL_FAILED.
int cp3pg_cancel_progress(long long handle);
int cp3pg_cancel_socket(long long handle);
int cp3pg_cancel_status(long long handle);  // ConnStatusType or -1
std::string cp3pg_cancel_error(long long handle);
void cp3pg_cancel_finish(long long handle);
void cp3pg_cancel_reset(long long handle);

// Build information ----------------------------------------------------------

// libpq's compiled-in version, e.g. 170006 for 17.6.
int cp3pg_lib_version();
// Server version of a live connection (e.g. 170006), -1 for an unknown handle.
int cp3pg_server_version(long long conn);

#endif  // CP3_POSTGRES_SHIM_H
