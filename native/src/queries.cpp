// queries.cpp - non-blocking query dispatch, pipeline mode and the result
// drain boundary.

#include "shim_internal.hpp"

using namespace cp3pg;

int cp3pg_flush(long long handle) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) return -1;
    return PQflush(conn);
}

int cp3pg_query_progress(long long handle) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) return -1;
    const int flush = PQflush(conn);
    if (flush == 1) return 2;   // POLL_WRITING
    if (flush == -1) {
        rememberError(PQerrorMessage(conn));
        return -1;              // POLL_FAILED
    }
    if (PQconsumeInput(conn) == 0) {
        rememberError(PQerrorMessage(conn));
        return -1;              // POLL_FAILED
    }
    if (PQisBusy(conn) == 1) return 1;  // POLL_READING
    return 0;                            // POLL_DONE
}

int cp3pg_send_query(long long handle, const std::string& sql) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) {
        rememberError("unknown connection handle");
        return -1;
    }
    const int rc = PQsendQuery(conn, sql.c_str());
    if (rc == 0) {
        rememberError(PQerrorMessage(conn));
        return -1;
    }
    return 0;
}

int cp3pg_send_query_params(long long handle, const std::string& sql,
                            const std::string& packed, int result_format) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) {
        rememberError("unknown connection handle");
        return -1;
    }
    DecodedParams decoded;
    std::string error;
    if (!decodePacked(packed, decoded, error)) {
        rememberError(error);
        return -1;
    }
    const int nparams = static_cast<int>(decoded.pointers.size());
    const int rc = PQsendQueryParams(
        conn, sql.c_str(), nparams, nullptr,
        nparams == 0 ? nullptr : decoded.pointers.data(),
        nparams == 0 ? nullptr : decoded.lengths.data(), nullptr,
        result_format);
    if (rc == 0) {
        rememberError(PQerrorMessage(conn));
        return -1;
    }
    return 0;
}

int cp3pg_send_prepare(long long handle, const std::string& name,
                       const std::string& sql, int nparams) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) {
        rememberError("unknown connection handle");
        return -1;
    }
    const int rc = PQsendPrepare(conn, name.c_str(), sql.c_str(), nparams,
                                 nullptr);
    if (rc == 0) {
        rememberError(PQerrorMessage(conn));
        return -1;
    }
    return 0;
}

int cp3pg_send_query_prepared(long long handle, const std::string& name,
                              const std::string& packed, int result_format) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) {
        rememberError("unknown connection handle");
        return -1;
    }
    DecodedParams decoded;
    std::string error;
    if (!decodePacked(packed, decoded, error)) {
        rememberError(error);
        return -1;
    }
    const int nparams = static_cast<int>(decoded.pointers.size());
    const int rc = PQsendQueryPrepared(
        conn, name.c_str(), nparams,
        nparams == 0 ? nullptr : decoded.pointers.data(),
        nparams == 0 ? nullptr : decoded.lengths.data(), nullptr,
        result_format);
    if (rc == 0) {
        rememberError(PQerrorMessage(conn));
        return -1;
    }
    return 0;
}

int cp3pg_enter_pipeline(long long handle) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) return -1;
    return PQenterPipelineMode(conn) == 1 ? 0 : -1;
}

int cp3pg_exit_pipeline(long long handle) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) return 0;
    return PQexitPipelineMode(conn);
}

int cp3pg_pipeline_sync(long long handle) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) return 0;
    return PQpipelineSync(conn);
}

int cp3pg_pipeline_status(long long handle) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) return -1;
    return static_cast<int>(PQpipelineStatus(conn));
}

long long cp3pg_get_result(long long handle) {
    PGconn* conn = findConnection(handle);
    if (conn == nullptr) return 0;
    PGresult* result = PQgetResult(conn);
    if (result == nullptr) return 0;
    return storeResult(result);
}
