#!/usr/bin/env python3
"""
Benchmarks the cp3 postgres package against node-postgres (pg) and libpq.

    python benchmark/run.py

Workloads (same for all three):
    select1     1000 x `select 1`
    series      100  x generate_series(1, 1000) drained row by row
    insert500   10   x (BEGIN; INSERT ... 500-row multi-VALUES; COMMIT)
    prepared    1000 x named/prepared statement execution
    pool16      16 concurrent pg_sleep(0.1) through a concurrency limit of 16

Server configuration: same variables as the test suite -
    CP3_PG_HOST/PORT/DB/USER/PASSWORD (defaults 127.0.0.1:55432 cp3_test)
    CP3_PG_CONNINFO wins if set.

Start a throwaway server for the run (initdb + pg_ctl from a PostgreSQL 17
binary distribution):
    python benchmark/run.py --start-pg [--pg-bin PATH/TO/bin]

--pg-bin / CP3_PG_BIN selects the PostgreSQL bin directory. Without it the
script searches PATH and common install roots.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BENCH = REPO / "benchmark"
NATIVE_LIB = REPO / "native" / "lib" / "libpq.a"
NATIVE_INCLUDE = REPO / "native" / "include" / "postgres"

ENV_CONNINFO = os.environ.get("CP3_PG_CONNINFO", "")
ENV_HOST = os.environ.get("CP3_PG_HOST", "127.0.0.1")
ENV_PORT = int(os.environ.get("CP3_PG_PORT", "55432"))
ENV_DB = os.environ.get("CP3_PG_DB", "cp3_test")
ENV_USER = os.environ.get("CP3_PG_USER", "postgres")
ENV_PASSWORD = os.environ.get("CP3_PG_PASSWORD", "")


def log(msg: str) -> None:
    print(f"[run.py] {msg}")


def which(name: str) -> str | None:
    if sys.platform == "win32" and not name.endswith((".exe", ".bat")):
        name += ".exe"
    path = shutil.which(name)
    return path


def find_pg_bin(start: str | None) -> Path | None:
    if start:
        p = Path(start)
        if p.is_dir() and (p / "initdb.exe").exists():
            return p
        log(f"warning: {start} does not look like a PostgreSQL bin directory")
        return None

    for name in ("initdb", "pg_ctl"):
        path = which(name)
        if path:
            return Path(path).parent

    candidates = []
    for root in (Path("C:/Program Files/PostgreSQL"), Path("C:/Program Files (x86)/PostgreSQL"),
                 Path(os.environ.get("LOCALAPPDATA", "")) / "Programs"):
        if root.is_dir():
            candidates.extend(root.glob("*/bin"))
            candidates.extend(root.glob("*/*/bin"))
    for c in candidates:
        if (c / "initdb.exe").exists():
            return c
    return None


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    log("$ " + " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, **kwargs)


class Throwaway:
    """A throwaway PostgreSQL cluster for the benchmark run."""

    def __init__(self, pg_bin: Path, tmp: Path) -> None:
        self.pg_bin = pg_bin
        self.tmp = tmp
        self.data = tmp / "data"
        self.port = ENV_PORT

    def start(self) -> bool:
        if self.data.exists():
            shutil.rmtree(self.data)
        tmp_log = self.tmp / "server.log"
        initdb = self.pg_bin / "initdb.exe"
        pg_ctl = self.pg_bin / "pg_ctl.exe"
        if not (initdb.exists() and pg_ctl.exists()):
            initdb, pg_ctl = self.pg_bin / "initdb", self.pg_bin / "pg_ctl"
        self.tmp.mkdir(parents=True, exist_ok=True)
        r = run([str(initdb), "-D", str(self.data), "-U", ENV_USER, "-A", "trust",
                 "-E", "UTF8", "--locale=C"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode != 0:
            log(f"initdb failed:\n{r.stdout}")
            return False
        r = run([str(pg_ctl), "-D", str(self.data), "-l", str(tmp_log),
                 "-o", f"-p {self.port} -c listen_addresses=127.0.0.1", "start"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                timeout=60)
        if r.returncode != 0:
            log(f"pg_ctl start failed:\n{r.stdout}\n--- server.log ---\n")
            log(tmp_log.read_text(errors="replace") if tmp_log.exists() else "<no log>")
            return False
        time.sleep(0.5)
        return True

    def create_db(self) -> bool:
        pg_isready = self.pg_bin / "pg_isready.exe"
        if not pg_isready.exists():
            pg_isready = self.pg_bin / "pg_isready"
        psql = self.pg_bin / "psql.exe"
        if not psql.exists():
            psql = self.pg_bin / "psql"
        for _ in range(30):
            if run([str(pg_isready), "-h", "127.0.0.1", "-p", str(self.port)],
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT).returncode == 0:
                break
            time.sleep(0.5)
        else:
            log("server did not become ready in time")
            return False
        # Create the test database if missing.
        r = run([str(psql), "-h", "127.0.0.1", "-p", str(self.port), "-U", ENV_USER,
                 "-d", "postgres", "-tAc",
                 f"select 1 from pg_database where datname='{ENV_DB}'"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.stdout.strip() != "1":
            r = run([str(psql), "-h", "127.0.0.1", "-p", str(self.port), "-U", ENV_USER,
                     "-d", "postgres", "-c", f"create database {ENV_DB}"],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            log("created database " + ENV_DB)
        return True


def server_reachable() -> bool:
    if ENV_CONNINFO:
        # Cannot parse arbitrary conninfo cheaply; attempt a raw socket to the
        # default port only when it matches.
        return True  # let the benchmarks fail loudly instead
    import socket
    try:
        with socket.create_connection((ENV_HOST, ENV_PORT), timeout=2):
            return True
    except OSError:
        return False


def conn_env() -> dict[str, str]:
    env = dict(os.environ)
    if not ENV_CONNINFO:
        env["CP3_PG_HOST"] = ENV_HOST
        env["CP3_PG_PORT"] = str(ENV_PORT)
        env["CP3_PG_DB"] = ENV_DB
        env["CP3_PG_USER"] = ENV_USER
        if ENV_PASSWORD:
            env["CP3_PG_PASSWORD"] = ENV_PASSWORD
    return env


def bench_cpp() -> str | None:
    src = BENCH / "cpp" / "bench.cpp"
    exe = BENCH / "cpp" / ("bench.exe" if sys.platform == "win32" else "bench")
    cmd = ["g++", "-O2", "-std=c++17", "-static-libgcc", "-static-libstdc++",
           "-o", str(exe), str(src),
           "-I", str(NATIVE_INCLUDE), str(NATIVE_LIB)]
    r = run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        log("C++ benchmark failed to build:\n" + r.stdout)
        return None
    # libpq.dll and friends live next to libpostgres_shim; the exe needs
    # them at run time.
    runtime = REPO / "native" / "runtime"
    if runtime.is_dir():
        for dll in runtime.glob("*.dll"):
            shutil.copy2(dll, exe.parent / dll.name)
    r = run([str(exe)], env=conn_env(), stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=300)
    return r.stdout


def bench_cp3() -> str | None:
    cp3 = os.environ.get("CP3_CMD") or shutil.which("cp3") or shutil.which("cp3.cmd")
    if cp3 is None:
        local = Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "cp3" / "bin"
        p = local / "cp3.cmd"
        if p.exists():
            cp3 = str(p)
    if cp3 is None:
        log("cp3 not found on PATH (set CP3_CMD to the cp3 launcher)")
        return None

    bench_dir = BENCH / "cp3"
    # Remove the previous output directory: cp3 refuses to overwrite the
    # native DLLs it copies next to the executable.
    out_dir = bench_dir / ".cp3" / "out"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    # Fresh native binding with absolute paths for this project; patch in
    # the static/shared/runtime DLLs the plain native-install does not
    # know about.
    binding = bench_dir / ".cp3" / "native" / "postgres"
    header = REPO / "native" / "include" / "postgres_shim.h"
    lib_dir = REPO / "native" / "lib"
    runtime = REPO / "native" / "runtime"
    r = run([cp3, "native-install", str(header), "--name", "postgres",
             "--lib-dir", str(lib_dir), "--link", "postgres_shim", "--link", "pq",
             "--force", "--json"],
            cwd=bench_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=120)
    if r.returncode != 0:
        log("cp3 native-install failed:\n" + r.stdout)
        return None

    native_json = binding / "native.json"
    d = json.loads(native_json.read_text(encoding="utf-8"))
    d["staticLibraries"] = [str(REPO / "native" / "lib" / "libpostgres_shim.a"),
                            str(REPO / "native" / "lib" / "libpq.a")]
    d["sharedLibraries"] = [str(REPO / "native" / "lib" / "libpq.dll.a")]
    d["runtimeDlls"] = [str(runtime / "libpq.dll"),
                        str(runtime / "libssl-3-x64.dll"),
                        str(runtime / "libcrypto-3-x64.dll"),
                        str(runtime / "libintl-9.dll"),
                        str(runtime / "libiconv-2.dll"),
                        str(runtime / "libwinpthread-1.dll")]
    native_json.write_text(json.dumps(d, indent=2), encoding="utf-8")

    r = run([cp3, "run", "bench.cp3"], cwd=bench_dir, env=conn_env(),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            timeout=600)
    return r.stdout


def bench_node(skip_npm: bool) -> str | None:
    node = shutil.which("node")
    if node is None:
        log("node not found on PATH; skipping the node benchmark")
        return None
    bench_dir = BENCH / "node"
    if not skip_npm and not (bench_dir / "node_modules" / "pg").is_dir():
        npm = shutil.which("npm")
        if npm:
            r = run([npm, "install", "--no-audit", "--no-fund"], cwd=bench_dir,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, timeout=600)
            if r.returncode != 0:
                log("npm install pg failed:\n" + r.stdout)
                return None
    r = run([node, "bench.js"], cwd=bench_dir, env=conn_env(),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            timeout=600)
    return r.stdout


ROWS = ["select1", "series", "insert500", "prepared", "pool16"]


def parse(output: str | None) -> dict[str, str]:
    out: dict[str, str] = {}
    if not output:
        return out
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 4:
            key = parts[1]
            if key in ROWS:
                out[key] = f"{parts[2]} ops / {parts[3]} ms"
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--start-pg", action="store_true",
                    help="start a throwaway PostgreSQL cluster for the run")
    ap.add_argument("--pg-bin", default=os.environ.get("CP3_PG_BIN"),
                    help="path to the PostgreSQL bin directory")
    ap.add_argument("--skip-npm", action="store_true",
                    help="do not npm install pg before running the node benchmark")
    ap.add_argument("--skip-native", action="store_true")
    args = ap.parse_args()

    throwaway = None
    if args.start_pg:
        pg_bin = find_pg_bin(args.pg_bin)
        if pg_bin is None:
            log("could not find a PostgreSQL 17 bin directory; pass --pg-bin")
            return 2
        log(f"using PostgreSQL binaries at {pg_bin}")
        tmp = Path(os.environ.get("TEMP", "/tmp")) / "cp3-bench-pg"
        throwaway = Throwaway(pg_bin, tmp)
        if not throwaway.start() or not throwaway.create_db():
            return 2

    try:
        if not args.start_pg and not server_reachable():
            log(f"no PostgreSQL server at {ENV_HOST}:{ENV_PORT} "
                f"(set CP3_PG_* / CP3_PG_CONNINFO, or use --start-pg)")
            return 2

        log("building and running the C++ (libpq) benchmark")
        cpp_out = None if args.skip_native else bench_cpp()

        log("building and running the cp3 benchmark")
        cp3_out = bench_cp3()

        log("running the node-postgres benchmark")
        node_out = bench_node(args.skip_npm)
    finally:
        if throwaway is not None:
            pg_ctl = throwaway.pg_bin / "pg_ctl.exe"
            if not pg_ctl.exists():
                pg_ctl = throwaway.pg_bin / "pg_ctl"
            run([str(pg_ctl), "-D", str(throwaway.data), "stop", "-m", "fast"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    table = parse(cpp_out) | parse(cp3_out) | parse(node_out)
    print()
    print(f"{'workload':<12} {'cpp':<22} {'cp3':<22} {'node':<22}")
    print("-" * 80)
    for row in ROWS:
        cols = []
        for name, out in (("cpp", cpp_out), ("cp3", cp3_out), ("node", node_out)):
            entry = parse(out).get(row, "-") if out else "-"
            cols.append(entry)
        print(f"{row:<12} {cols[0]:<22} {cols[1]:<22} {cols[2]:<22}")
    print()
    if cpp_out:
        print("cpp output:\n" + cpp_out)
    if cp3_out:
        print("cp3 output:\n" + cp3_out)
    if node_out:
        print("node output:\n" + node_out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
