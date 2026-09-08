# ViaBTC golden integration tests

These golden tests exercise the deployed exchange components through their public/runtime interfaces. They do not inspect source files for implementation strings.

## Test entrypoints

- `run_tests_f2p.sh` builds the server components, starts the local infrastructure/services, and runs the post-change feature suite.
- `run_tests_p2p.sh` performs the same build/runtime preparation and runs compatibility/regression checks.
- `integration_tests.c` contains one C function per named test case. Shared F2P/P2P context objects carry state only when a later lifecycle test needs an ID or timestamp created by an earlier test.

## Result and assertion output

Every test is queued before runtime startup and then prints a live `RUN` line plus a final `PASS` or `FAIL` line. If a prerequisite blocks execution, affected tests print `NOT RUN`.

Failed assertions identify the exact expectation that did not hold. Depending on the test, diagnostics include:

- expected versus actual HTTP status;
- transport errors and raw HTTP response bodies;
- JSON-RPC error messages and complete JSON responses;
- expected versus actual array counts;
- missing or unexpected named assets/markets;
- expected versus actual `suspended` values;
- missing order IDs or pending-order IDs;
- expected versus actual balance fields;
- slice-history/table counts and matchengine logs;
- operlog row counts and matching details;
- CLI output when a CLI operation is rejected.

## Important fixture details

Test assets use `prec_save=12`. Test markets use `fee_prec=4`, `stock_prec=4`, and `money_prec=4`; these values satisfy the exchange's precision validation rules. Earlier versions of the suite used 8/8 stock/money precision, which made the supposedly valid market fixture invalid and caused many cascading failures.

`makeslice` is asynchronous because matchengine forks the snapshot work. The tests therefore poll for a new `slice_history` row instead of assuming a fixed sleep is sufficient.

Persistence tests restart the dependent service stack in dependency order (`matchengine -> accesshttp -> marketprice`) so post-restart HTTP checks do not reuse an accesshttp RPC connection that was established before matchengine restarted.

The runners generate temporary runtime configs under `/tmp` that only cap process resource limits to values permitted by the verifier container. Repository `config.json` files remain unchanged.

### Golden-alignment notes

The F2P launcher waits for matchengine's asynchronous outbound refresh RPC client
to connect to marketprice before feature mutations begin. This avoids a startup
race where the first successful admin operation can occur before the refresh
channel exists.

The delist-refund assertion compares the asset's frozen balance with the baseline
immediately before the delist-specific order. Other intentionally open orders may
freeze the same asset, so a correct delist does not necessarily make the global
freeze value zero.

Operlog assertions inspect only dated runtime tables (`operlog_YYYYMMDD`); the
`operlog_example` table is a schema template and never contains runtime entries.
