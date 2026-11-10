// bench.js - node-postgres (pg) baseline for the benchmark suite.
//
// Requires the `pg` package:  cd benchmark/node && npm install pg
// Run:  node bench.js
// Environment: CP3_PG_CONNINFO or CP3_PG_HOST/PORT/DB/USER/PASSWORD
//
// Workloads mirror benchmark/cpp and benchmark/cp3:
//   select1     1000 x simple `select 1`
//   series      100  x select * from generate_series(1,1000), draining all rows
//   insert500   10   x (BEGIN; INSERT 500-row multi-VALUES; COMMIT)
//   prepared    name-based prepared statement, 1000 x named query
//   pool16      16 concurrent clients from a pool of 16 (pg_sleep(0.1) x 16)

const { Client, Pool } = require('pg');

function config() {
  const override = process.env.CP3_PG_CONNINFO;
  if (override) {
    return { connectionString: override, connectionTimeoutMillis: 2000 };
  }
  return {
    host: process.env.CP3_PG_HOST || '127.0.0.1',
    port: Number(process.env.CP3_PG_PORT || 55432),
    database: process.env.CP3_PG_DB || 'cp3_test',
    user: process.env.CP3_PG_USER || 'postgres',
    password: process.env.CP3_PG_PASSWORD || undefined,
    connectionTimeoutMillis: 2000,
  };
}

const CI = config();

function us(ms, ops) {
  return ops > 0 ? ((ms * 1000) / ops).toFixed(3) : '0';
}

async function benchSelect1(client, iterations) {
  const t0 = process.hrtime.bigint();
  for (let i = 0; i < iterations; i++) {
    await client.query('select 1');
  }
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  console.log(`node select1 ${iterations} ${Math.round(ms)} ${us(ms, iterations)}`);
}

async function benchSeries(client, iterations) {
  const t0 = process.hrtime.bigint();
  for (let i = 0; i < iterations; i++) {
    const res = await client.query('select * from generate_series(1, 1000)');
    if (res.rows.length !== 1000) {
      console.error(`series: got ${res.rows.length} rows, want 1000`);
    }
  }
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  console.log(`node series ${iterations} ${Math.round(ms)} ${us(ms, iterations)}`);
}

async function benchInsert500(client, batches) {
  let values = '';
  for (let i = 0; i < 500; i++) {
    values += (i > 0 ? ',' : '') + `(${i},'row-${i}')`;
  }
  const sql = `insert into cp3_bench_items (id, note) values ${values}`;

  await client.query('drop table if exists cp3_bench_items; ' +
                     'create table cp3_bench_items (id int, note text)');

  const t0 = process.hrtime.bigint();
  for (let b = 0; b < batches; b++) {
    await client.query('begin');
    await client.query(sql);
    await client.query('commit');
  }
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  const ops = batches * 500;
  console.log(`node insert500 ${ops} ${Math.round(ms)} ${us(ms, ops)}`);
}

async function benchPrepared(client, iterations) {
  // node's named prepared statements: first call prepares server-side.
  const t0 = process.hrtime.bigint();
  for (let i = 0; i < iterations; i++) {
    await client.query({
      name: 'bench_prep',
      text: 'select $1::int as n',
      values: [i],
    });
  }
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  console.log(`node prepared ${iterations} ${Math.round(ms)} ${us(ms, iterations)}`);
}

async function benchPool16() {
  const pool = new Pool({ ...CI, max: 16 });
  const t0 = process.hrtime.bigint();
  const jobs = [];
  for (let i = 0; i < 16; i++) {
    jobs.push(pool.query('select pg_sleep(0.1)'));
  }
  await Promise.all(jobs);
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  console.log(`node pool16 16 ${Math.round(ms)} ${us(ms, 16)}`);
  await pool.end();
}

async function main() {
  const client = new Client(CI);
  try {
    await client.connect();
  } catch (err) {
    console.error(`could not connect to the test server; set CP3_PG_* or CP3_PG_CONNINFO\n${err.message}`);
    process.exit(2);
  }

  await benchSelect1(client, 1000);
  await benchSeries(client, 100);
  await benchInsert500(client, 10);
  await benchPrepared(client, 1000);
  await client.end();

  console.error('note: pool16 warms up 16 connections');
  try {
    await benchPool16();
  } catch (err) {
    console.error(`pool16 failed: ${err.message || err}`);
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
