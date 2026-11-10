// bench.cpp - libpq (C API) baseline for the benchmark suite.
//
// Build (see run.py): g++ -O2 -o bench.exe bench.cpp -I <libpq include> <libpq lib>
// Run:  set CP3_PG_CONNINFO=...   (or CP3_PG_HOST/PORT/DB/USER/PASSWORD)
//
// Workloads mirror benchmark/cp3 and benchmark/node:
//   select1     1000 x simple `select 1`
//   series      100  x select * from generate_series(1,1000), draining all rows
//   insert500   10   x (BEGIN; INSERT 500-row multi-VALUES; COMMIT)
//   prepared    PQprepare + 1000 x PQexecPrepared("select $1::int")
//   pool16      16 threads x one own connection x 100 queries each (libpq
//               has no pool; this is the 16-concurrent-client equivalent)

#include <libpq-fe.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static std::string conninfo() {
    if (const char* c = std::getenv("CP3_PG_CONNINFO")) {
        if (*c) return std::string(c);
    }
    std::string host = "host=127.0.0.1";
    if (const char* h = std::getenv("CP3_PG_HOST")) host = std::string("host=") + h;
    std::string port = "port=55432";
    if (const char* p = std::getenv("CP3_PG_PORT")) port = std::string("port=") + p;
    std::string db = "cp3_test";
    if (const char* d = std::getenv("CP3_PG_DB")) db = std::string(d);
    std::string user = "postgres";
    if (const char* u = std::getenv("CP3_PG_USER")) user = std::string(u);
    std::string password = "";
    if (const char* pw = std::getenv("CP3_PG_PASSWORD")) password = std::string(pw);
    std::string out = host + " " + port + " dbname=" + db + " user=" + user +
                      " connect_timeout=2";
    if (!password.empty()) out += " password=" + password;
    return out;
}

static PGconn* openConn() {
    PGconn* conn = PQconnectdb(conninfo().c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::fprintf(stderr, "connect failed: %s\n", PQerrorMessage(conn));
        std::exit(2);
    }
    return conn;
}

static void mustOk(PGconn* conn, PGresult* res, const char* what) {
    if (!res) {
        std::fprintf(stderr, "%s: null result: %s\n", what, PQerrorMessage(conn));
        std::exit(3);
    }
    const ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK && st != PGRES_PIPELINE_SYNC) {
        std::fprintf(stderr, "%s: %s\n", what, PQresultErrorMessage(res));
        std::exit(3);
    }
}

static void drain(PGconn* conn, PGresult* res) { PQclear(res); }

static void benchSelect1(PGconn* conn, int iterations) {
    const auto t0 = Clock::now();
    for (int i = 0; i < iterations; i++) {
        PGresult* res = PQexec(conn, "select 1");
        mustOk(conn, res, "select 1");
        drain(conn, res);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    std::printf("cpp select1 %d %lld %.3f\n", iterations, (long long)ms,
                iterations > 0 ? (double)ms * 1000.0 / iterations : 0.0);
}

static void benchSeries(PGconn* conn, int iterations) {
    const auto t0 = Clock::now();
    for (int i = 0; i < iterations; i++) {
        PGresult* res = PQexec(conn, "select * from generate_series(1, 1000)");
        mustOk(conn, res, "series");
        // Drain all rows.
        long rows = PQntuples(res);
        volatile long acc = 0;
        for (long r = 0; r < rows; r++) acc += std::atol(PQgetvalue(res, r, 0));
        (void)acc;
        drain(conn, res);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    std::printf("cpp series %d %lld %.3f\n", iterations, (long long)ms,
                iterations > 0 ? (double)ms * 1000.0 / iterations : 0.0);
}

static void benchInsert500(PGconn* conn, int batches) {
    std::string values;
    values.reserve(16 * 500);
    for (int i = 0; i < 500; i++) {
        if (i > 0) values += ',';
        values += "(";
        values += std::to_string(i);
        values += ",'row-";
        values += std::to_string(i);
        values += "')";
    }
    const std::string sql =
        "insert into cp3_bench_items (id, note) values " + values;

    {
        PGresult* setup = PQexec(conn,
            "drop table if exists cp3_bench_items; "
            "create table cp3_bench_items (id int, note text)");
        mustOk(conn, setup, "create bench table");
        drain(conn, setup);
    }

    const auto t0 = Clock::now();
    for (int b = 0; b < batches; b++) {
        PGresult* begin = PQexec(conn, "begin");
        mustOk(conn, begin, "begin");
        drain(conn, begin);
        PGresult* ins = PQexec(conn, sql.c_str());
        mustOk(conn, ins, "insert 500");
        drain(conn, ins);
        PGresult* commit = PQexec(conn, "commit");
        mustOk(conn, commit, "commit");
        drain(conn, commit);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    const int ops = batches * 500;
    std::printf("cpp insert500 %d %lld %.3f\n", ops, (long long)ms,
                ops > 0 ? (double)ms * 1000.0 / ops : 0.0);
}

static void benchPrepared(PGconn* conn, int iterations) {
    PGresult* p = PQprepare(conn, "bench_prep", "select $1::int", 1, nullptr);
    mustOk(conn, p, "prepare");
    drain(conn, p);

    const auto t0 = Clock::now();
    for (int i = 0; i < iterations; i++) {
        const char* params[1] = {std::to_string(i).c_str()};
        PGresult* res = PQexecPrepared(conn, "bench_prep", 1, params, nullptr, nullptr, 0);
        mustOk(conn, res, "prepared exec");
        drain(conn, res);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    std::printf("cpp prepared %d %lld %.3f\n", iterations, (long long)ms,
                iterations > 0 ? (double)ms * 1000.0 / iterations : 0.0);
}

static void benchPool16(int perThread) {
    const auto t0 = Clock::now();
    std::vector<std::thread> threads;
    threads.reserve(16);
    for (int t = 0; t < 16; t++) {
        threads.emplace_back([perThread]() {
            PGconn* conn = openConn();
            for (int i = 0; i < perThread; i++) {
                PGresult* res = PQexec(conn, "select 1");
                if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
                    std::fprintf(stderr, "pool16 query failed\n");
                    std::exit(3);
                }
                PQclear(res);
            }
            PQfinish(conn);
        });
    }
    for (auto& th : threads) th.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    const int ops = 16 * perThread;
    std::printf("cpp pool16 %d %lld %.3f\n", ops, (long long)ms,
                ops > 0 ? (double)ms * 1000.0 / ops : 0.0);
}

int main() {
    PGconn* conn = openConn();
    benchSelect1(conn, 1000);
    benchSeries(conn, 100);
    benchInsert500(conn, 10);
    benchPrepared(conn, 1000);
    PQfinish(conn);
    benchPool16(100);  // 16 threads x 100 queries
    return 0;
}
