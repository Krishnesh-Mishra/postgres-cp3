// postgres_shim.cpp - implementation of the pointer-free libpq shim.
//
// The handle maps are guarded by a mutex so that looking a handle up is safe
// from any thread. libpq itself still requires that one PGconn is not used by
// two threads at the same time; that rule is documented, not enforced here.

#include "postgres_shim.h"

#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

#include <libpq-fe.h>

namespace {

std::mutex g_mutex;
std::unordered_map<long long, PGconn*> g_connections;
std::unordered_map<long long, PGresult*> g_results;
long long g_next_handle = 1;

thread_local std::string g_last_error;

long long nextHandle() { return g_next_handle++; }

PGconn* findConnection(long long handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_connections.find(handle);
    return it == g_connections.end() ? nullptr : it->second;
}

PGresult* findResult(long long handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_results.find(handle);
    return it == g_results.end() ? nullptr : it->second;
}

void rememberError(const std::string& message) { g_last_error = message; }

std::string textOrEmpty(const char* text) {
    return text != nullptr ? std::string(text) : std::string();
}

long long storeConnection(PGconn* conn) {
    const long long handle = nextHandle();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_connections.emplace(handle, conn);
    return handle;
}

long long storeResult(PGresult* result) {
    const long long handle = nextHandle();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_results.emplace(handle, result);
    return handle;
}

long long runQuery(long long conn_handle, const std::string& sql, int nparams,
                   const char* const* values) {
    PGconn* conn = findConnection(conn_handle);
    if (conn == nullptr) {
        rememberError("unknown connection handle");
        return 0;
    }
    PGresult* result = PQexecParams(conn, sql.c_str(), nparams, nullptr, values,
                                    nullptr, nullptr, 0);
    if (result == nullptr) {
        rememberError(PQerrorMessage(conn));
        return 0;
    }
    return storeResult(result);
}

}  // namespace

long long cp3pg_connect(const std::string& conninfo) {
    PGconn* conn = PQconnectdb(conninfo.c_str());
    if (conn == nullptr) {
        rememberError("PQconnectdb returned no connection");
        return 0;
    }
    if (PQstatus(conn) != CONNECTION_OK) {
        rememberError(PQerrorMessage(conn));
        PQfinish(conn);
        return 0;
    }
    return storeConnection(conn);
}

void cp3pg_close(long long handle) {
    PGconn* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_connections.find(handle);
        if (it == g_connections.end()) return;
        conn = it->second;
        g_connections.erase(it);
    }
    PQfinish(conn);
}

int cp3pg_status(long long handle) {
    PGconn* conn = findConnection(handle);
    return conn == nullptr ? -1 : static_cast<int>(PQstatus(conn));
}

std::string cp3pg_error(long long handle) {
    PGconn* conn = findConnection(handle);
    return conn == nullptr ? std::string() : textOrEmpty(PQerrorMessage(conn));
}

std::string cp3pg_last_error() { return g_last_error; }

long long cp3pg_exec(long long conn, const std::string& sql) {
    return runQuery(conn, sql, 0, nullptr);
}

long long cp3pg_exec1(long long conn, const std::string& sql,
                      const std::string& p1) {
    const char* values[1] = {p1.c_str()};
    return runQuery(conn, sql, 1, values);
}

long long cp3pg_exec2(long long conn, const std::string& sql,
                      const std::string& p1, const std::string& p2) {
    const char* values[2] = {p1.c_str(), p2.c_str()};
    return runQuery(conn, sql, 2, values);
}

long long cp3pg_exec3(long long conn, const std::string& sql,
                      const std::string& p1, const std::string& p2,
                      const std::string& p3) {
    const char* values[3] = {p1.c_str(), p2.c_str(), p3.c_str()};
    return runQuery(conn, sql, 3, values);
}

int cp3pg_result_status(long long handle) {
    PGresult* result = findResult(handle);
    return result == nullptr ? -1 : static_cast<int>(PQresultStatus(result));
}

std::string cp3pg_result_error(long long handle) {
    PGresult* result = findResult(handle);
    return result == nullptr ? std::string()
                             : textOrEmpty(PQresultErrorMessage(result));
}

std::string cp3pg_cmd_status(long long handle) {
    PGresult* result = findResult(handle);
    return result == nullptr ? std::string() : textOrEmpty(PQcmdStatus(result));
}

std::string cp3pg_cmd_tuples(long long handle) {
    PGresult* result = findResult(handle);
    return result == nullptr ? std::string() : textOrEmpty(PQcmdTuples(result));
}

int cp3pg_affected(long long handle) {
    PGresult* result = findResult(handle);
    if (result == nullptr) return -1;
    // PQcmdTuples also parses the row count out of a SELECT command tag, but
    // this API documents "rows changed by INSERT/UPDATE/DELETE". Keep SELECT
    // and error results at 0.
    if (PQresultStatus(result) != PGRES_COMMAND_OK) return 0;
    return std::atoi(PQcmdTuples(result));
}

int cp3pg_ntuples(long long handle) {
    PGresult* result = findResult(handle);
    return result == nullptr ? -1 : PQntuples(result);
}

int cp3pg_nfields(long long handle) {
    PGresult* result = findResult(handle);
    return result == nullptr ? -1 : PQnfields(result);
}

std::string cp3pg_fname(long long handle, int col) {
    PGresult* result = findResult(handle);
    if (result == nullptr || col < 0 || col >= PQnfields(result)) {
        return std::string();
    }
    return textOrEmpty(PQfname(result, col));
}

int cp3pg_fnumber(long long handle, const std::string& name) {
    PGresult* result = findResult(handle);
    if (result == nullptr) return -1;
    return PQfnumber(result, name.c_str());
}

std::string cp3pg_getvalue(long long handle, int row, int col) {
    PGresult* result = findResult(handle);
    if (result == nullptr || row < 0 || row >= PQntuples(result) || col < 0 ||
        col >= PQnfields(result)) {
        return std::string();
    }
    return textOrEmpty(PQgetvalue(result, row, col));
}

int cp3pg_getisnull(long long handle, int row, int col) {
    PGresult* result = findResult(handle);
    if (result == nullptr || row < 0 || row >= PQntuples(result) || col < 0 ||
        col >= PQnfields(result)) {
        return 1;
    }
    return PQgetisnull(result, row, col) ? 1 : 0;
}

void cp3pg_clear(long long handle) {
    PGresult* result = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_results.find(handle);
        if (it == g_results.end()) return;
        result = it->second;
        g_results.erase(it);
    }
    PQclear(result);
}

std::string cp3pg_escape_literal(long long handle, const std::string& value) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) return std::string();
    char* escaped = PQescapeLiteral(conn, value.c_str(), value.size());
    if (escaped == nullptr) return std::string();
    std::string out(escaped);
    PQfreemem(escaped);
    return out;
}

std::string cp3pg_escape_identifier(long long handle,
                                    const std::string& value) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) return std::string();
    char* escaped = PQescapeIdentifier(conn, value.c_str(), value.size());
    if (escaped == nullptr) return std::string();
    std::string out(escaped);
    PQfreemem(escaped);
    return out;
}

int cp3pg_lib_version() { return PQlibVersion(); }

int cp3pg_server_version(long long handle) {
    PGconn* conn = findConnection(handle);
    return conn == nullptr ? -1 : PQserverVersion(conn);
}
