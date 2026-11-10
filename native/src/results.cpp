// results.cpp - result inspection, value decoding and cleanup.

#include "shim_internal.hpp"

using namespace cp3pg;

std::string cp3pg_result_diag(long long handle, int field) {
    PGresult* result = findResult(handle);
    if (result == nullptr) return std::string();
    const char* value = PQresultErrorField(result, field);
    return value != nullptr ? std::string(value) : std::string();
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

int cp3pg_affected(long long handle) {
    PGresult* result = findResult(handle);
    if (result == nullptr) return -1;
    // Rows changed by INSERT/UPDATE/DELETE; other commands report 0.
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

int cp3pg_ftype(long long handle, int col) {
    PGresult* result = findResult(handle);
    if (result == nullptr || col < 0 || col >= PQnfields(result)) return -1;
    return static_cast<int>(PQftype(result, col));
}

std::string cp3pg_getvalue(long long handle, int row, int col) {
    PGresult* result = findResult(handle);
    if (result == nullptr || row < 0 || row >= PQntuples(result) || col < 0 ||
        col >= PQnfields(result)) {
        return std::string();
    }
    const char* value = PQgetvalue(result, row, col);
    if (value == nullptr) return std::string();
    if (PQftype(result, col) == 16) {
        // Normalize the boolean text representation.
        if (value[0] == 't' || value[0] == 'T' || value[0] == '1') {
            return "true";
        }
        return "false";
    }
    return std::string(value);
}

std::string cp3pg_getvalue_decoded(long long handle, int row, int col) {
    PGresult* result = findResult(handle);
    if (result == nullptr || row < 0 || row >= PQntuples(result) || col < 0 ||
        col >= PQnfields(result)) {
        return std::string();
    }
    const unsigned char* value =
        reinterpret_cast<const unsigned char*>(PQgetvalue(result, row, col));
    const int len = PQgetlength(result, row, col);
    switch (PQftype(result, col)) {
        case 16:  // bool
            return len > 0 && value[0] != 0 ? "true" : "false";
        case 21:  // int2
            return len >= 2 ? std::to_string(readBe16(value)) : std::string();
        case 23:  // int4
            return len >= 4 ? std::to_string(readBe32(value)) : std::string();
        case 20:  // int8
            return len >= 8
                       ? std::to_string(static_cast<int64_t>(readBeU64(value)))
                       : std::string();
        case 700:  // float4
            return len >= 4 ? formatFloat(static_cast<double>(readBeF32(value)))
                            : std::string();
        case 701:  // float8
            return len >= 8 ? formatFloat(readBeF64(value)) : std::string();
        case 17:  // bytea
            return base64Encode(value, static_cast<size_t>(len));
        case 25:   // text
        case 1043:  // varchar
        case 114:  // json
        case 3802:  // jsonb
        default:
            return std::string(reinterpret_cast<const char*>(value),
                               static_cast<size_t>(len));
    }
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
