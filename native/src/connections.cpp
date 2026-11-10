// connections.cpp - connection lifecycle, reactor sockets and build info.

#include "shim_internal.hpp"

using namespace cp3pg;

namespace cp3pg {

std::string& lastErrorRef() {
    static thread_local std::string s;
    return s;
}

}  // namespace cp3pg

long long cp3pg_connect_start(const std::string& conninfo) {
    PGconn* conn = PQconnectStart(conninfo.c_str());
    if (conn == nullptr) {
        rememberError("PQconnectStart returned no connection");
        return 0;
    }
    return storeConnection(conn);
}

int cp3pg_connect_progress(long long handle) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) return -1;
    const PostgresPollingStatusType status = PQconnectPoll(conn);
    switch (status) {
        case PGRES_POLLING_FAILED:
            rememberError(PQerrorMessage(conn));
            return -1;  // POLL_FAILED
        case PGRES_POLLING_READING:
            return 1;   // POLL_READING
        case PGRES_POLLING_WRITING:
            return 2;   // POLL_WRITING
        case PGRES_POLLING_OK:
            return 0;   // POLL_DONE
        default:
            return -1;
    }
}

int cp3pg_socket(long long handle) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) return -1;
    return PQsocket(conn);
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

std::string cp3pg_last_error() { return lastErrorRef(); }

int cp3pg_lib_version() { return PQlibVersion(); }

int cp3pg_server_version(long long handle) {
    PGconn* conn = findConnection(handle);
    return conn == nullptr ? -1 : PQserverVersion(conn);
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
