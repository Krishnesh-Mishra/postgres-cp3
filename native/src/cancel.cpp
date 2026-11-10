// cancel.cpp - query cancellation on a separate connection.
//
// The cancel connection is independent of the query connection; drive it with
// the same io.pollUntilDone reactor ("cancel" waits), then release with
// cp3pg_cancel_finish. Cancelling an idle connection is harmless: the
// backend receives a CancelRequest and finds nothing to cancel.

#include "shim_internal.hpp"

using namespace cp3pg;

long long cp3pg_cancel_start(long long conn_handle) {
    PGconn* conn = findConnection(conn_handle);
    if (conn == nullptr) {
        rememberError("unknown connection handle");
        return 0;
    }
    PGcancelConn* cancel = PQcancelCreate(conn);
    if (cancel == nullptr) {
        rememberError("PQcancelCreate failed");
        return 0;
    }
    const long long handle = nextHandle();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cancels.emplace(handle, cancel);
    }
    if (PQcancelStart(cancel) == 0) {
        rememberError(PQcancelErrorMessage(cancel));
        cp3pg_cancel_finish(handle);
        return 0;
    }
    return handle;
}

int cp3pg_cancel_progress(long long handle) {
    PGcancelConn* cancel = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_cancels.find(handle);
        if (it == g_cancels.end()) return -1;
        cancel = it->second;
    }
    const PostgresPollingStatusType status = PQcancelPoll(cancel);
    switch (status) {
        case PGRES_POLLING_FAILED:
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

int cp3pg_cancel_socket(long long handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_cancels.find(handle);
    if (it == g_cancels.end()) return -1;
    return PQcancelSocket(it->second);
}

int cp3pg_cancel_status(long long handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_cancels.find(handle);
    if (it == g_cancels.end()) return -1;
    return static_cast<int>(PQcancelStatus(it->second));
}

std::string cp3pg_cancel_error(long long handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_cancels.find(handle);
    if (it == g_cancels.end()) return std::string();
    return textOrEmpty(PQcancelErrorMessage(it->second));
}

void cp3pg_cancel_finish(long long handle) {
    PGcancelConn* cancel = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_cancels.find(handle);
        if (it == g_cancels.end()) return;
        cancel = it->second;
        g_cancels.erase(it);
    }
    PQcancelFinish(cancel);
}

void cp3pg_cancel_reset(long long handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_cancels.find(handle);
    if (it != g_cancels.end()) PQcancelReset(it->second);
}
