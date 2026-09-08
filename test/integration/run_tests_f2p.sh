#!/usr/bin/env bash
set -u -o pipefail

ROOT="${REPO_ROOT:-/app}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${TMPDIR:-/tmp}/viabtc_golden_integration"


# Extract the registered test names directly from integration_tests.c so the
# shell-side progress/NOT RUN reporting cannot drift from the C suite. Every
# behavioral test is implemented as its own function and registered below.
load_test_names() {
  local suite="$1"
  local start end

  if [[ "$suite" == "f2p" ]]; then
    start='static const struct f2p_case F2P_CASES\[\]'
    end='^};'
  else
    start='static const struct p2p_case P2P_CASES\[\]'
    end='^};'
  fi

  mapfile -t TEST_NAMES < <(
    sed -n "/$start/,/$end/p" "$HERE/integration_tests.c" \
      | sed -n 's/^[[:space:]]*{[[:space:]]*"\([^"]*\)".*/\1/p'
  )
}

# Show the complete test plan before build/startup work begins. This makes the
# verifier log useful even when an infrastructure prerequisite prevents tests
# from reaching the integration client.
print_test_plan() {
  local suite="$1"
  printf '\n============================================\n'
  printf '  %s integration test plan (%d cases)\n' "${suite^^}" "${#TEST_NAMES[@]}"
  printf '============================================\n'
  for name in "${TEST_NAMES[@]}"; do
    printf 'TEST QUEUED: %s\n' "$name"
  done
  printf '\n'
}

# If a prerequisite fails before the client can execute, explicitly mark every
# affected test as NOT RUN rather than leaving the verifier with no test output.
not_run_all() {
  local reason="$1"
  local name

  printf '\nTEST EXECUTION BLOCKED: %s\n' "$reason" >&2
  for name in "${TEST_NAMES[@]}"; do
    printf 'TEST: %-72s NOT RUN\n' "$name"
  done
  printf '\nRESULT: 0 passed, 0 failed, %d not run, %d total\n' \
    "${#TEST_NAMES[@]}" "${#TEST_NAMES[@]}"
}

# Run the C suite while teeing its live output. If the client exits prematurely,
# mark only the test cases that never produced a PASS/FAIL result as NOT RUN.
run_integration_suite() {
  local suite="$1"
  local log="${TMPDIR:-/tmp}/golden_${suite}_results.log"
  local rc missing=0 interrupted=0 name

  : >"$log"
  REPO_ROOT="$ROOT" GOLDEN_ME_CONFIG="${GOLDEN_ME_CONFIG:-}" GOLDEN_AH_CONFIG="${GOLDEN_AH_CONFIG:-}" GOLDEN_MP_CONFIG="${GOLDEN_MP_CONFIG:-}" "$BIN" "$suite" 2>&1 | tee "$log"
  rc=${PIPESTATUS[0]}

  for name in "${TEST_NAMES[@]}"; do
    if grep -Fq "TEST: $name" "$log"; then
      continue
    fi

    if grep -Fq "RUN : $name" "$log"; then
      printf 'TEST: %-72s FAIL\n' "$name"
      printf '    integration client stopped before this running test produced a result\n' >&2
      interrupted=$((interrupted + 1))
    else
      printf 'TEST: %-72s NOT RUN\n' "$name"
      missing=$((missing + 1))
    fi
  done

  if (( interrupted > 0 || missing > 0 )); then
    printf '\nINCOMPLETE SUMMARY: %d interrupted test(s) failed; %d later test(s) were NOT RUN.\n' \
      "$interrupted" "$missing"
    return 1
  fi

  return "$rc"
}

# The repository asks each service to raise its process limits to values that can
# exceed the verifier container's hard limits. A process cannot raise a hard
# RLIMIT without extra container privileges, so create temporary runtime configs
# capped to limits the current environment already permits. The tracked configs
# under /app are never modified.
prepare_runtime_configs() {
  local requested_nofile=1000000
  local requested_core_bytes=1000000000
  local hard_nofile hard_core_blocks runtime_nofile runtime_core_bytes
  local config_dir="${TMPDIR:-/tmp}/golden_runtime_configs"
  local component src dst

  printf 'LIMIT RUN : derive verifier-safe runtime config limits\n'
  printf 'LIMIT INFO: environment nofile soft=%s hard=%s; core soft=%s hard=%s\n' \
    "$(ulimit -Sn)" "$(ulimit -Hn)" "$(ulimit -Sc)" "$(ulimit -Hc)"

  hard_nofile="$(ulimit -Hn)"
  if [[ "$hard_nofile" == "unlimited" ]]; then
    runtime_nofile="$requested_nofile"
  elif [[ "$hard_nofile" =~ ^[0-9]+$ ]] && (( hard_nofile > 0 )); then
    if (( hard_nofile < requested_nofile )); then
      runtime_nofile="$hard_nofile"
    else
      runtime_nofile="$requested_nofile"
    fi
  else
    printf 'LIMIT FAIL: could not determine a usable hard nofile limit: %s\n' "$hard_nofile" >&2
    return 1
  fi

  # Bash reports RLIMIT_CORE in 1024-byte blocks. Convert a finite hard limit to
  # bytes before writing core_limit into the JSON config. If it is unlimited,
  # retain the repository's requested 1,000,000,000-byte value.
  hard_core_blocks="$(ulimit -Hc)"
  if [[ "$hard_core_blocks" == "unlimited" ]]; then
    runtime_core_bytes="$requested_core_bytes"
  elif [[ "$hard_core_blocks" =~ ^[0-9]+$ ]]; then
    runtime_core_bytes=$((hard_core_blocks * 1024))
    if (( runtime_core_bytes > requested_core_bytes )); then
      runtime_core_bytes="$requested_core_bytes"
    fi
  else
    printf 'LIMIT FAIL: could not determine a usable hard core limit: %s\n' "$hard_core_blocks" >&2
    return 1
  fi

  mkdir -p "$config_dir"

  # Copy each runtime service config to /tmp and change only the two process
  # limit values. sed is sufficient here because both fields are scalar numeric
  # values in every component config.
  for component in matchengine accesshttp marketprice accessws readhistory alertcenter; do
    src="$ROOT/$component/config.json"
    dst="$config_dir/${component}.json"

    if [[ ! -f "$src" ]]; then
      continue
    fi

    sed -E \
      -e "s/(\"file_limit\"[[:space:]]*:[[:space:]]*)[0-9]+/\\1${runtime_nofile}/" \
      -e "s/(\"core_limit\"[[:space:]]*:[[:space:]]*)[0-9]+/\\1${runtime_core_bytes}/" \
      "$src" >"$dst" || return 1
  done

  GOLDEN_CONFIG_DIR="$config_dir"
  GOLDEN_ME_CONFIG="$config_dir/matchengine.json"
  GOLDEN_AH_CONFIG="$config_dir/accesshttp.json"
  GOLDEN_MP_CONFIG="$config_dir/marketprice.json"
  export GOLDEN_CONFIG_DIR GOLDEN_ME_CONFIG GOLDEN_AH_CONFIG GOLDEN_MP_CONFIG

  printf 'LIMIT INFO: repository requested file_limit=%d core_limit=%d bytes\n' \
    "$requested_nofile" "$requested_core_bytes"
  printf 'LIMIT INFO: runtime config uses file_limit=%s core_limit=%s bytes\n' \
    "$runtime_nofile" "$runtime_core_bytes"
  printf 'LIMIT INFO: temporary configs: %s\n' "$config_dir"
  printf 'LIMIT PASS: verifier-safe runtime configs prepared; repository configs unchanged\n'
}

load_test_names f2p
print_test_plan f2p

# Build every server component before starting any runtime tests. These link
# overrides mirror the supplied Dockerfile so the build runs against the same
# bookworm-era toolchain and transitive library requirements.
build_component() {
  local name="$1"
  shift

  printf 'BUILD RUN : %s\n' "$name"
  if "$@"; then
    printf 'BUILD PASS: %s\n' "$name"
    return 0
  fi

  printf 'BUILD FAIL: %s\n' "$name" >&2
  return 1
}

build_all_components() {
  local jobs
  jobs="$(nproc)"

  build_component network \
    make -C "$ROOT/network" -j"$jobs" || return 1

  build_component utils \
    make -C "$ROOT/utils" -j"$jobs" || return 1

  build_component matchengine \
    make -C "$ROOT/matchengine" -j"$jobs" \
      LIBS='-L ../utils -lutils -L ../network -lnetwork -Wl,-Bstatic -lev -ljansson -lmpdec -lmysqlclient -lz -lrdkafka -llz4 -lssl -lcrypto -lhiredis -Wl,-Bdynamic -lm -lpthread -ldl' || return 1

  build_component readhistory \
    make -C "$ROOT/readhistory" -j"$jobs" \
      LIBS='-L ../utils -lutils -L ../network -lnetwork -Wl,-Bstatic -lev -ljansson -lmpdec -lmysqlclient -lz -lssl -lcrypto -lhiredis -Wl,-Bdynamic -lm -lpthread -ldl' || return 1

  build_component accesshttp \
    make -C "$ROOT/accesshttp" -j"$jobs" || return 1

  build_component accessws \
    make -C "$ROOT/accessws" -j"$jobs" \
      LIBS='-L ../utils -lutils -L ../network -lnetwork -Wl,-Bstatic -lev -ljansson -lmpdec -lrdkafka -llz4 -lz -lssl -lcrypto -lhiredis -Wl,-Bdynamic -lcurl -lm -lpthread -ldl' || return 1

  build_component alertcenter \
    make -C "$ROOT/alertcenter" -j"$jobs" || return 1
}

# Start and verify the infrastructure daemons baked into the Docker image.
# The verifier launches a fresh container with CMD=/bin/bash, so MariaDB, Redis,
# ZooKeeper, and Kafka are installed and initialized but are not running yet.
# Start each dependency only when needed so rerunning P2P after F2P is harmless.
wait_tcp() {
  local port="$1"
  local attempts="${2:-80}"
  local i

  for ((i = 0; i < attempts; i++)); do
    if nc -z 127.0.0.1 "$port" >/dev/null 2>&1; then
      return 0
    fi
    sleep .25
  done
  return 1
}

infra_step() {
  local name="$1"
  shift

  printf 'INFRA RUN : %s\n' "$name"
  if "$@"; then
    printf 'INFRA PASS: %s\n' "$name"
    return 0
  fi

  printf 'INFRA FAIL: %s\n' "$name" >&2
  return 1
}

ensure_mariadb() {
  # Step 1: Start MariaDB if the fresh verifier container has not started it.
  if ! mysqladmin ping --silent >/dev/null 2>&1; then
    service mariadb start >/tmp/golden_mariadb_start.log 2>&1 || {
      cat /tmp/golden_mariadb_start.log >&2 || true
      return 1
    }
  fi

  # Step 2: Wait until the server accepts the Dockerfile-provisioned test user.
  local i
  for i in $(seq 1 40); do
    if mysql -N -B -uuser -ppass trade_log -e 'SELECT 1' >/dev/null 2>&1; then
      return 0
    fi
    sleep .25
  done
  return 1
}

ensure_redis() {
  # Step 1: Start the Redis master used by the exchange when it is not running.
  if ! redis-cli -h 127.0.0.1 -p 6379 ping 2>/dev/null | grep -qx PONG; then
    redis-server --daemonize yes >/tmp/golden_redis_start.log 2>&1 || {
      cat /tmp/golden_redis_start.log >&2 || true
      return 1
    }
  fi

  # Step 2: Start the three sentinel processes configured by the Dockerfile.
  local port
  for port in 26381 26382 26383; do
    if ! wait_tcp "$port" 1; then
      redis-sentinel "/etc/redis/sentinel-$port.conf" --daemonize yes \
        >>/tmp/golden_redis_start.log 2>&1 || return 1
    fi
  done

  redis-cli -h 127.0.0.1 -p 6379 ping 2>/dev/null | grep -qx PONG
}

ensure_zookeeper() {
  # Step 1: Reuse an existing ZooKeeper when F2P already started it.
  if wait_tcp 2181 1; then
    return 0
  fi

  # Step 2: Start the Dockerfile-pinned Kafka/ZooKeeper stack with Java 11.
  export JAVA_HOME=/opt/java11
  export PATH="$JAVA_HOME/bin:$PATH"
  /opt/kafka/bin/zookeeper-server-start.sh -daemon \
    /opt/kafka/config/zookeeper.properties >/tmp/golden_zookeeper_start.log 2>&1 || return 1

  # Step 3: Wait for ZooKeeper's client port before starting Kafka.
  wait_tcp 2181 80
}

ensure_kafka() {
  # Step 1: Reuse the broker when an earlier suite already started it, or start
  # it now. A listening TCP socket is only the first stage of Kafka readiness.
  export JAVA_HOME=/opt/java11
  export PATH="$JAVA_HOME/bin:$PATH"

  if ! wait_tcp 9092 1; then
    /opt/kafka/bin/kafka-server-start.sh -daemon \
      /opt/kafka/config/server.properties >/tmp/golden_kafka_start.log 2>&1 || return 1
    wait_tcp 9092 120 || return 1
  fi

  # Step 2: Require a real Kafka protocol handshake. This is stronger than the
  # TCP check above and prevents librdkafka clients from racing broker startup.
  local i
  for i in $(seq 1 60); do
    if /opt/kafka/bin/kafka-broker-api-versions.sh \
         --bootstrap-server 127.0.0.1:9092 >/tmp/golden_kafka_api.log 2>&1; then
      break
    fi
    sleep .5
  done
  if ! /opt/kafka/bin/kafka-broker-api-versions.sh \
       --bootstrap-server 127.0.0.1:9092 >/tmp/golden_kafka_api.log 2>&1; then
    cat /tmp/golden_kafka_api.log >&2 || true
    return 1
  fi

  # Step 3: Ensure every exchange topic expected by the deployed configuration
  # exists before matchengine or marketprice attempts to use it.
  local topic
  for topic in deals orders balances; do
    /opt/kafka/bin/kafka-topics.sh --create --if-not-exists \
      --topic "$topic" --zookeeper 127.0.0.1:2181 \
      --partitions 1 --replication-factor 1 >/dev/null 2>&1 || return 1
  done

  # Step 4: Wait until each topic has a real leader. Creating a topic through
  # ZooKeeper can succeed before the broker has assigned its partition leader.
  for topic in deals orders balances; do
    for i in $(seq 1 40); do
      if /opt/kafka/bin/kafka-topics.sh --describe \
           --topic "$topic" --zookeeper 127.0.0.1:2181 2>/dev/null \
           | grep -Eq 'Leader:[[:space:]]*[0-9]+'; then
        break
      fi
      sleep .25
    done
    if ! /opt/kafka/bin/kafka-topics.sh --describe \
         --topic "$topic" --zookeeper 127.0.0.1:2181 2>/dev/null \
         | grep -Eq 'Leader:[[:space:]]*[0-9]+'; then
      printf 'Kafka topic %s never received a partition leader\n' "$topic" >&2
      return 1
    fi
  done

  printf 'INFRA INFO: Kafka broker handshake succeeded and all exchange topics have leaders\n'
}

ensure_runtime_infrastructure() {
  infra_step MariaDB ensure_mariadb || return 1
  infra_step Redis ensure_redis || return 1
  infra_step ZooKeeper ensure_zookeeper || return 1
  infra_step Kafka ensure_kafka || return 1
}

# Stop the components exercised by this suite before rebuilding. Restarting them
# afterward guarantees that every request reaches the newly compiled binaries.
pkill -QUIT -x marketprice.exe >/dev/null 2>&1 || true
pkill -QUIT -x accesshttp.exe  >/dev/null 2>&1 || true
pkill -QUIT -x matchengine.exe >/dev/null 2>&1 || true
sleep 1

# F2P validates the expected post-change implementation, where every component
# must build successfully. Unlike the base-commit Dockerfile, marketprice is
# therefore a required build here rather than a best-effort build.
build_all_components || { not_run_all "required server component build failed"; exit 1; }
build_component marketprice \
  make -C "$ROOT/marketprice" -j"$(nproc)" || { not_run_all "required marketprice build failed"; exit 1; }

# Build the black-box integration test client only after the server build passes.
printf 'BUILD RUN : golden integration client\n'
if gcc -O2 -Wall -Wextra "$HERE/integration_tests.c" -o "$BIN" -ljansson -lcurl -lm; then
  printf 'BUILD PASS: golden integration client\n'
else
  printf 'BUILD FAIL: golden integration client\n' >&2
  not_run_all "integration client build failed"
  exit 1
fi

# Bring up the Dockerfile-provisioned infrastructure before touching runtime state.
# A fresh verifier container does not automatically run /usr/local/bin/start-services.
if ! ensure_runtime_infrastructure; then
  not_run_all "runtime infrastructure startup failed"
  exit 1
fi

# Prepare temporary configs whose process limits fit the verifier container.
if ! prepare_runtime_configs; then
  not_run_all "could not prepare verifier-safe runtime configs"
  exit 1
fi

printf 'Preparing isolated runtime state for F2P integration tests...\n'

# Verify the deployed trade_log database is reachable before destructive setup.
# If it is unavailable, none of the runtime feature cases can execute reliably.
if ! mysql -N -B -uuser -ppass trade_log -e 'SELECT 1' >/dev/null 2>&1; then
  not_run_all "trade_log database is unavailable"
  exit 1
fi

# Re-deploy the trade-log schema from the patched submission itself. The verifier
# image initializes MariaDB when the base image is built, before the task patch is
# applied. Without this step, newly added template tables such as
# slice_asset_example and slice_market_example would not exist even though the
# submitted sql/create_trade_log.sql correctly defines them.
#
# This models the task's stated assumption that the system *and database* have
# been successfully deployed from the resulting codebase.
printf 'DB DEPLOY RUN : trade_log schema from patched sql/create_trade_log.sql\n'
mapfile -t tables < <(mysql -N -B -uuser -ppass trade_log -e 'SHOW TABLES' 2>/dev/null || true)
for t in "${tables[@]}"; do
  [[ -n "$t" ]] || continue
  mysql -uuser -ppass trade_log -e "DROP TABLE IF EXISTS \`$t\`" >/dev/null 2>&1 || {
    printf 'DB DEPLOY FAIL: could not drop existing table %s\n' "$t" >&2
    not_run_all "trade_log schema deployment failed"
    exit 1
  }
done

if mysql -uuser -ppass trade_log < "$ROOT/sql/create_trade_log.sql" \
     >/tmp/golden_trade_log_schema.out 2>/tmp/golden_trade_log_schema.err; then
  printf 'DB DEPLOY PASS: trade_log schema loaded from current repository\n'
else
  printf 'DB DEPLOY FAIL: current sql/create_trade_log.sql could not be applied\n' >&2
  cat /tmp/golden_trade_log_schema.err >&2 || true
  not_run_all "trade_log schema deployment failed"
  exit 1
fi

# Start a component from the freshly built executable and capture its logs.
start_component() {
  local dir="$1"
  local exe="$2"
  local log="$3"

  # Run against the temporary config for this component. The repository's
  # tracked config.json remains untouched.
  local config="$GOLDEN_CONFIG_DIR/$dir.json"
  (cd "$ROOT/$dir" && nohup "./$exe" "$config" >"$log" 2>&1 &)
}

# Wait until the component is listening before issuing any test request.
wait_port() {
  local port="$1"
  for _ in $(seq 1 50); do
    nc -z 127.0.0.1 "$port" >/dev/null 2>&1 && return 0
    sleep .2
  done
  return 1
}

# Wait for the HTTP gateway to complete its worker/upstream initialization. A
# listening port alone is insufficient because the listener can accept a socket
# before a worker is ready to return a JSON response.
wait_http_ready() {
  local attempts="${1:-60}"
  local i status body=/tmp/golden_http_probe.json

  for i in $(seq 1 "$attempts"); do
    status="$(curl -sS --max-time 2 -o "$body" -w '%{http_code}' \
      -H 'Content-Type: application/json' \
      -d '{"id":1,"method":"asset.list","params":[]}' \
      http://127.0.0.1:8080/ 2>/dev/null || true)"
    if [[ "$status" == "200" ]] && grep -Eq '"(result|error)"[[:space:]]*:' "$body" 2>/dev/null; then
      return 0
    fi
    sleep .25
  done
  return 1
}

# marketprice initializes by querying market.list through accesshttp and then
# creating its Kafka consumer. Retry startup so a short-lived upstream readiness
# race does not turn into a suite-level failure. accesshttp MUST already be ready
# before this function is called.
start_marketprice_with_retry() {
  local log="$1"
  local attempt

  for attempt in $(seq 1 5); do
    printf 'SERVICE RUN : marketprice startup attempt %d/5\n' "$attempt"
    pkill -QUIT -x marketprice.exe >/dev/null 2>&1 || true
    sleep .25
    : >"$log"
    start_component marketprice marketprice.exe "$log"
    if wait_port 7416; then
      printf 'SERVICE PASS: marketprice is listening on RPC port 7416\n'
      return 0
    fi
    printf 'SERVICE RETRY: marketprice did not become ready on attempt %d\n' "$attempt" >&2
    cat "$log" >&2 || true
    sleep 1
  done
  return 1
}

# Wait until matchengine's outbound refresh RPC client has connected to the
# already-running marketprice server. The client reconnects asynchronously, so
# merely observing marketprice's listening port is not sufficient: an admin
# mutation issued too early could legitimately occur before the refresh channel
# is established and create a false negative in the propagation tests.
wait_refresh_channel_ready() {
  local attempts="${1:-75}"
  local i

  printf 'SERVICE RUN : matchengine -> marketprice refresh channel readiness\n'
  for i in $(seq 1 "$attempts"); do
    if grep -RhsE 'connect marketprice.*success' \
         /var/log/trade/matchengine /tmp/golden_matchengine.log 2>/dev/null \
         | grep -q .; then
      printf 'SERVICE PASS: matchengine refresh client is connected to marketprice\n'
      return 0
    fi
    sleep .2
  done

  printf 'SERVICE FAIL: matchengine refresh client did not connect to marketprice\n' >&2
  printf 'SERVICE DIAG: recent matchengine logs:\n' >&2
  grep -RhsE 'marketprice|connect .*success|connect .*fail' \
    /var/log/trade/matchengine /tmp/golden_matchengine.log 2>/dev/null \
    | tail -40 >&2 || true
  return 1
}

# Step 1: start matchengine. accesshttp routes most public methods to this RPC
# service, and marketprice ultimately obtains its initial market registry through
# accesshttp -> matchengine.
start_component matchengine matchengine.exe /tmp/golden_matchengine.log
if ! wait_port 7316 || ! wait_port 7317; then
  echo 'FAIL: matchengine did not start; see /tmp/golden_matchengine.log' >&2
  cat /tmp/golden_matchengine.log >&2 || true
  not_run_all "matchengine startup failed"
  exit 1
fi
printf 'SERVICE PASS: matchengine RPC/CLI ports are ready\n'

# Step 2: start accesshttp BEFORE marketprice. marketprice's init_market() calls
# the configured accesshttp URL to fetch market.list; starting marketprice first
# causes init_message to fail with ECONNREFUSED even when Kafka is healthy.
start_component accesshttp accesshttp.exe /tmp/golden_accesshttp.log
if ! wait_port 8080 || ! wait_http_ready 80; then
  echo 'FAIL: accesshttp did not become HTTP-ready; see /tmp/golden_accesshttp.log' >&2
  cat /tmp/golden_accesshttp.log >&2 || true
  not_run_all "accesshttp HTTP readiness failed"
  exit 1
fi
printf 'SERVICE PASS: accesshttp returned a valid JSON response\n'

# Step 3: start marketprice only after accesshttp can successfully proxy a
# registry request to matchengine. At this point both of marketprice's startup
# dependencies (HTTP registry access and Kafka) have been readiness-checked.
if ! start_marketprice_with_retry /tmp/golden_marketprice.log; then
  echo 'FAIL: marketprice did not start after retries; see /tmp/golden_marketprice.log' >&2
  not_run_all "marketprice startup failed after accesshttp/Kafka readiness checks"
  exit 1
fi

# Step 4: wait for matchengine's asynchronous outbound RPC client to complete
# its connection to marketprice before any admin operation can trigger refresh.
if ! wait_refresh_channel_ready 75; then
  not_run_all "matchengine-to-marketprice refresh channel did not become ready"
  exit 1
fi

# Run the post-change feature integration suite against the deployed services.
run_integration_suite f2p
