# Run the postgres cp3 test suite against a local PostgreSQL server.
#
# By default the script starts a throwaway cluster in the temp directory, runs
# the suite, and stops the cluster again. If a server is already listening on
# the chosen port, it is reused and left running.
#
# Examples:
#   pwsh -File tests/run.ps1 -PgBin "C:\Program Files\PostgreSQL\17\bin"
#   pwsh -File tests/run.ps1 -NoStart -Port 5432 -Database postgres
#   pwsh -File tests/run.ps1 -KeepServer
#
# The test program itself only needs CP3_PG_* variables or CP3_PG_CONNINFO:
#   CP3_PG_HOST, CP3_PG_PORT, CP3_PG_DB, CP3_PG_USER, CP3_PG_PASSWORD

param(
    [string]$PgBin = "",
    [string]$DataDir = "",
    [int]$Port = 55432,
    [string]$Database = "cp3_test",
    [string]$User = "postgres",
    [string]$Password = "",
    [string]$Cp3 = "",
    [switch]$NoStart,
    [switch]$KeepServer
)

$ErrorActionPreference = "Stop"

$tests = $PSScriptRoot
$root = Split-Path -Parent $tests
$serverHost = "127.0.0.1"

if (-not $DataDir) {
    $DataDir = Join-Path $env:TEMP "cp3-postgres-tests\data"
}
$Log = Join-Path $env:TEMP "cp3-postgres-tests\server.log"

function Resolve-Cp3([string]$explicit) {
    if ($explicit) {
        if (Test-Path $explicit -PathType Leaf) {
            return (Resolve-Path $explicit).Path
        }
        $candidate = Join-Path $explicit "cp3.cmd"
        if (Test-Path $candidate) { return $candidate }
        throw "cp3 executable not found: $explicit"
    }

    $cmd = Get-Command cp3 -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $known = "C:\Users\Krishnesh\AppData\Local\Programs\cp3\bin\cp3.cmd"
    if (Test-Path $known) { return $known }

    throw "Could not find cp3. Pass -Cp3 <path-to-cp3.cmd> or add it to PATH."
}

function Resolve-PgBin([string]$explicit) {
    if ($explicit) {
        if (Test-Path $explicit -PathType Leaf) {
            return (Split-Path -Parent (Resolve-Path $explicit).Path)
        }
        if (Test-Path $explicit -PathType Container) {
            return (Resolve-Path $explicit).Path
        }
        throw "PostgreSQL bin path not found: $explicit"
    }

    if ($env:CP3_PG_BIN -and (Test-Path (Join-Path $env:CP3_PG_BIN "pg_ctl.exe"))) {
        return $env:CP3_PG_BIN
    }

    $pgCtl = Get-Command pg_ctl -ErrorAction SilentlyContinue
    if ($pgCtl) {
        return (Split-Path -Parent $pgCtl.Source)
    }

    $programFiles = Join-Path $env:ProgramFiles "PostgreSQL"
    if (Test-Path $programFiles) {
        $candidate = Get-ChildItem $programFiles -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName "bin" } |
            Where-Object { Test-Path (Join-Path $_ "pg_ctl.exe") } |
            Select-Object -First 1
        if ($candidate) { return $candidate }
    }

    throw "Could not find PostgreSQL. Pass -PgBin <path-to-postgres-bin> or set CP3_PG_BIN."
}

$cp3Exe = Resolve-Cp3 $Cp3
$pgBinPath = $null
$pgCtl = $null
$pgIsReady = $null
$psql = $null
$createdb = $null
$started = $false

if (-not $NoStart) {
    $pgBinPath = Resolve-PgBin $PgBin
    $pgCtl = Join-Path $pgBinPath "pg_ctl.exe"
    $pgIsReady = Join-Path $pgBinPath "pg_isready.exe"
    $psql = Join-Path $pgBinPath "psql.exe"
    $createdb = Join-Path $pgBinPath "createdb.exe"
}

function Test-ServerReady() {
    if (-not $pgIsReady) { return $false }
    & $pgIsReady -h $serverHost -p $Port 2>$null | Out-Null
    return ($LASTEXITCODE -eq 0)
}

if (-not $NoStart) {
    if (Test-ServerReady) {
        Write-Host "using PostgreSQL already listening on ${serverHost}:${Port}"
    } else {
        if (-not (Test-Path $DataDir)) {
            Write-Host "initializing temporary PostgreSQL cluster in $DataDir"
            New-Item -ItemType Directory -Force (Split-Path -Parent $DataDir) | Out-Null
            & (Join-Path $pgBinPath "initdb.exe") -D $DataDir -U $User -A trust -E UTF8 --locale=C
            if ($LASTEXITCODE -ne 0) {
                throw "initdb failed with exit code $LASTEXITCODE"
            }
        }

        New-Item -ItemType Directory -Force (Split-Path -Parent $Log) | Out-Null
        Write-Host "starting PostgreSQL on ${serverHost}:${Port}"
        & $pgCtl -D $DataDir -l $Log -o "-p $Port -c listen_addresses=$serverHost" start
        if ($LASTEXITCODE -ne 0) {
            throw "pg_ctl failed with exit code $LASTEXITCODE; see $Log"
        }
        $started = $true

        $ready = $false
        for ($i = 0; $i -lt 60; $i = $i + 1) {
            Start-Sleep -Milliseconds 500
            if (Test-ServerReady) {
                $ready = $true
                break
            }
        }
        if (-not $ready) {
            throw "PostgreSQL did not become ready; see $Log"
        }

        $dbLiteral = $Database.Replace("'", "''")
        $exists = & $psql -h $serverHost -p $Port -U $User -d postgres -tAc "select 1 from pg_database where datname = '$dbLiteral'"
        if (($exists | Out-String).Trim() -ne "1") {
            Write-Host "creating database $Database"
            & $createdb -h $serverHost -p $Port -U $User $Database
            if ($LASTEXITCODE -ne 0) {
                throw "createdb failed with exit code $LASTEXITCODE"
            }
        }
    }
}

$env:CP3_PG_HOST = $serverHost
$env:CP3_PG_PORT = "$Port"
$env:CP3_PG_DB = $Database
$env:CP3_PG_USER = $User
$env:CP3_PG_PASSWORD = $Password
Remove-Item Env:CP3_PG_CONNINFO -ErrorAction SilentlyContinue

$exitCode = 1
try {
    Push-Location $tests
    & $cp3Exe run src\main.cp3
    $exitCode = $LASTEXITCODE
} finally {
    Pop-Location
    if ($started -and -not $KeepServer) {
        Write-Host "stopping PostgreSQL"
        & $pgCtl -D $DataDir stop -m fast
    }
}

exit $exitCode
