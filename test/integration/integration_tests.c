#define _GNU_SOURCE
#include <arpa/inet.h>
#include <curl/curl.h>
#include <endian.h>
#include <errno.h>
#include <glob.h>
#include <math.h>
#include <jansson.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define RPC_MAGIC 0x70656562u
#define RPC_REQUEST 0
#define CMD_BALANCE_QUERY 101
#define CMD_BALANCE_UPDATE 102
#define CMD_ASSET_LIST 104
#define CMD_ORDER_PUT_LIMIT 201
#define CMD_ORDER_QUERY 203
#define CMD_ORDER_CANCEL 204
#define CMD_MARKET_LIST 307

static int passed;
static int failed;
static const char *HTTP_URL;
static const char *ME_HOST;
static int ME_PORT;
static int MP_PORT;
static int CLI_PORT;
static long req_id = 700000;

static long last_http_status;
static CURLcode last_http_rc;
static char last_http_body[8192];

struct buf {
    char *p;
    size_t n;
};

#pragma pack(push, 1)
struct rpc_head {
    uint32_t magic;
    uint32_t command;
    uint16_t pkg_type;
    uint32_t result;
    uint32_t crc32;
    uint32_t sequence;
    uint64_t req_id;
    uint32_t body_size;
    uint16_t ext_size;
};
#pragma pack(pop)

typedef struct {
    const char *name;
    bool ok;
} test_state;

typedef struct {
    const char *asset_a;
    const char *asset_b;
    const char *asset_c;
    const char *market_main;
    const char *market_cli;
    const char *market_post;
    uint32_t user_id;
    uint64_t business_id;
    uint64_t active_order_id;
    uint64_t slice_order_id;
    uint64_t post_order_id;
    uint64_t delist_order_id;
    long slice_before_id;
    long slice_id;
    char slice_ts[64];
    long marketprice_refresh_before_suspend;
    double delist_freeze_before;
    double delist_freeze_with_order;
} f2p_ctx;

typedef struct {
    char asset[64];
    char stock[64];
    char money[64];
    char market[64];
    uint32_t user_id;
    uint64_t order_id;
    json_t *http_assets;
    json_t *http_markets;
} p2p_ctx;

static void diag_json(const char *label, json_t *j)
{
    char *s = j ? json_dumps(j, JSON_COMPACT | JSON_SORT_KEYS) : NULL;
    fprintf(stderr, "    %s: %s\n", label, s ? s : "<null>");
    free(s);
}

static void begin_test(test_state *t, const char *name)
{
    t->name = name;
    t->ok = true;
    printf("RUN : %s\n", name);
    fflush(stdout);
}

static void assertion(test_state *t, bool condition, const char *fmt, ...)
{
    if (condition)
        return;

    t->ok = false;
    fprintf(stderr, "    ASSERT FAIL: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static void end_test(test_state *t)
{
    printf("TEST: %-72s %s\n", t->name, t->ok ? "PASS" : "FAIL");
    if (t->ok)
        passed++;
    else
        failed++;
    fflush(stdout);
}

static bool is_ok(json_t *r)
{
    json_t *e = r ? json_object_get(r, "error") : NULL;
    return r && e && json_is_null(e);
}

static json_t *result(json_t *r)
{
    return r ? json_object_get(r, "result") : NULL;
}

static const char *rpc_error_message(json_t *r)
{
    json_t *e = r ? json_object_get(r, "error") : NULL;
    json_t *m = json_is_object(e) ? json_object_get(e, "message") : NULL;
    return json_is_string(m) ? json_string_value(m) : NULL;
}

static void assert_rpc_success(test_state *t, json_t *r, const char *operation)
{
    if (!r) {
        assertion(t, false, "%s returned no JSON-RPC response", operation);
        return;
    }
    if (!is_ok(r)) {
        const char *msg = rpc_error_message(r);
        assertion(t, false, "%s returned JSON-RPC error%s%s", operation,
                  msg ? ": " : "", msg ? msg : "");
        diag_json("actual response", r);
    }
}

static void assert_http_success(test_state *t, json_t *r, const char *operation)
{
    assertion(t, last_http_rc == CURLE_OK,
              "%s transport failed: %s", operation, curl_easy_strerror(last_http_rc));
    assertion(t, last_http_status == 200,
              "%s expected HTTP status 200, got %ld; body=%s", operation,
              last_http_status, last_http_body[0] ? last_http_body : "<empty>");
    assert_rpc_success(t, r, operation);
}

static void assert_http_rpc_error(test_state *t, json_t *r, const char *operation)
{
    assertion(t, last_http_rc == CURLE_OK,
              "%s transport failed: %s", operation, curl_easy_strerror(last_http_rc));
    assertion(t, last_http_status == 200,
              "%s expected HTTP status 200 with a JSON-RPC error, got %ld; body=%s",
              operation, last_http_status, last_http_body[0] ? last_http_body : "<empty>");
    assertion(t, r != NULL, "%s returned no parseable JSON body", operation);
    if (r)
        assertion(t, !is_ok(r), "%s was expected to fail but returned success", operation);
}

static void assert_result_array(test_state *t, json_t *r, const char *operation)
{
    json_t *v = result(r);
    assertion(t, json_is_array(v), "%s expected result to be an array", operation);
    if (!json_is_array(v) && r)
        diag_json("actual response", r);
}

static void assert_array_size(test_state *t, json_t *a, size_t expected, const char *what)
{
    if (!json_is_array(a)) {
        assertion(t, false, "%s expected an array before checking count", what);
        return;
    }
    assertion(t, json_array_size(a) == expected,
              "%s expected %zu item(s), got %zu", what, expected, json_array_size(a));
}

static size_t wr_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    struct buf *b = ud;
    size_t m = size * nmemb;
    char *q = realloc(b->p, b->n + m + 1);
    if (!q)
        return 0;
    b->p = q;
    memcpy(b->p + b->n, ptr, m);
    b->n += m;
    b->p[b->n] = 0;
    return m;
}

static json_t *http_call(const char *method, json_t *params)
{
    last_http_status = 0;
    last_http_rc = CURLE_FAILED_INIT;
    last_http_body[0] = '\0';

    CURL *c = curl_easy_init();
    if (!c)
        return NULL;

    struct buf b = {0};
    json_t *q = json_object();
    json_object_set_new(q, "id", json_integer(++req_id));
    json_object_set_new(q, "method", json_string(method));
    json_object_set(q, "params", params);
    char *body = json_dumps(q, JSON_COMPACT);
    json_decref(q);

    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, HTTP_URL);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    last_http_rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &last_http_status);

    if (b.p)
        snprintf(last_http_body, sizeof(last_http_body), "%s", b.p);

    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    free(body);

    if (last_http_rc != CURLE_OK || !b.p) {
        free(b.p);
        return NULL;
    }

    json_error_t er;
    json_t *r = json_loads(b.p, 0, &er);
    if (!r)
        fprintf(stderr, "    HTTP %s returned invalid JSON: %s\n", method, b.p);
    free(b.p);
    return r;
}

static uint32_t crc32_repo(const unsigned char *p, size_t n)
{
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
    }
    return ~c;
}

static int connect_tcp(const char *host, int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    struct timeval tv = {3, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1 || connect(s, (void *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static int send_all(int s, const void *p, size_t n)
{
    const char *b = p;
    while (n) {
        ssize_t k = send(s, b, n, 0);
        if (k <= 0)
            return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static int recv_all(int s, void *p, size_t n)
{
    char *b = p;
    while (n) {
        ssize_t k = recv(s, b, n, 0);
        if (k <= 0)
            return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static json_t *rpc_call_port(int port, uint32_t cmd, json_t *params)
{
    char *body = json_dumps(params, JSON_COMPACT);
    size_t bn = strlen(body);
    size_t total = sizeof(struct rpc_head) + bn;
    unsigned char *pkt = calloc(1, total);
    struct rpc_head *h = (void *)pkt;

    h->magic = htole32(RPC_MAGIC);
    h->command = htole32(cmd);
    h->pkg_type = htole16(RPC_REQUEST);
    h->sequence = htole32((uint32_t)++req_id);
    h->req_id = htole64((uint64_t)req_id);
    h->body_size = htole32((uint32_t)bn);
    memcpy(pkt + sizeof(*h), body, bn);
    h->crc32 = 0;
    h->crc32 = htole32(crc32_repo(pkt, total));
    free(body);

    int s = connect_tcp(ME_HOST, port);
    if (s < 0) {
        free(pkt);
        return NULL;
    }
    if (send_all(s, pkt, total) < 0) {
        close(s);
        free(pkt);
        return NULL;
    }
    free(pkt);

    struct rpc_head rh;
    if (recv_all(s, &rh, sizeof(rh)) < 0) {
        close(s);
        return NULL;
    }

    uint32_t sz = le32toh(rh.body_size);
    uint16_t ex = le16toh(rh.ext_size);
    if (ex) {
        char *tmp = malloc(ex);
        if (recv_all(s, tmp, ex) < 0) {
            free(tmp);
            close(s);
            return NULL;
        }
        free(tmp);
    }

    char *rb = calloc(1, sz + 1);
    if (recv_all(s, rb, sz) < 0) {
        free(rb);
        close(s);
        return NULL;
    }
    close(s);

    json_error_t er;
    json_t *r = json_loadb(rb, sz, 0, &er);
    if (!r)
        fprintf(stderr, "    RPC cmd %u returned invalid JSON: %s\n", cmd, rb);
    free(rb);
    return r;
}

static json_t *rpc_call(uint32_t cmd, json_t *params)
{
    return rpc_call_port(ME_PORT, cmd, params);
}

static char *cli_call(const char *cmd)
{
    int s = connect_tcp(ME_HOST, CLI_PORT);
    if (s < 0)
        return NULL;

    char line[512];
    snprintf(line, sizeof(line), "%s\n", cmd);
    if (send_all(s, line, strlen(line)) < 0) {
        close(s);
        return NULL;
    }

    char *out = calloc(1, 16384);
    ssize_t n = recv(s, out, 16383, 0);
    close(s);
    if (n <= 0) {
        free(out);
        return NULL;
    }
    out[n] = 0;
    return out;
}

static bool cli_output_success(const char *out)
{
    if (!out || !*out)
        return false;
    const char *bad[] = {"usage", "command not found", "not exist", "invalid", "fail", "error"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (strcasestr(out, bad[i]))
            return false;
    }
    return true;
}

static void assert_cli_success(test_state *t, const char *command, const char *out)
{
    assertion(t, out != NULL, "CLI command '%s' returned no output", command);
    if (out)
        assertion(t, cli_output_success(out), "CLI command '%s' returned an error-like response: %s", command, out);
}

static json_t *A0(void)
{
    return json_array();
}

static json_t *asset_params(const char *name, int save, int show)
{
    json_t *a = json_array();
    json_array_append_new(a, json_string(name));
    json_array_append_new(a, json_integer(save));
    json_array_append_new(a, json_integer(show));
    return a;
}

/*
 * The exchange validates market precisions against the asset save precision.
 * With test assets at prec_save=12, using stock_prec=8 and money_prec=8 is
 * invalid because stock_prec + money_prec exceeds 12. Use 4/4/4 so the fixture
 * is valid while still exercising nontrivial precision handling.
 */
static json_t *market_params(const char *name, const char *stock, const char *money)
{
    json_t *a = json_array();
    json_array_append_new(a, json_string(name));
    json_array_append_new(a, json_integer(4));
    json_array_append_new(a, json_string("0.001"));
    json_array_append_new(a, json_string(stock));
    json_array_append_new(a, json_integer(4));
    json_array_append_new(a, json_string(money));
    json_array_append_new(a, json_integer(4));
    return a;
}

static json_t *find_named(json_t *a, const char *name)
{
    if (!json_is_array(a))
        return NULL;
    size_t i;
    json_t *x;
    json_array_foreach(a, i, x) {
        json_t *v = json_object_get(x, "name");
        if (json_is_string(v) && strcmp(json_string_value(v), name) == 0)
            return x;
    }
    return NULL;
}

static bool array_has_named(json_t *a, const char *name)
{
    return find_named(a, name) != NULL;
}

static void assert_named_present(test_state *t, json_t *a, const char *name, const char *what)
{
    assertion(t, json_is_array(a), "%s expected an array while looking for '%s'", what, name);
    if (json_is_array(a)) {
        assertion(t, array_has_named(a, name), "%s did not contain expected item '%s'", what, name);
        if (!array_has_named(a, name))
            diag_json("actual result", a);
    }
}

static void assert_named_absent(test_state *t, json_t *a, const char *name, const char *what)
{
    assertion(t, json_is_array(a), "%s expected an array while checking absence of '%s'", what, name);
    if (json_is_array(a)) {
        assertion(t, !array_has_named(a, name), "%s unexpectedly still contained '%s'", what, name);
        if (array_has_named(a, name))
            diag_json("actual result", a);
    }
}

static bool names_sorted(json_t *a)
{
    if (!json_is_array(a))
        return false;
    const char *prev = NULL;
    size_t i;
    json_t *x;
    json_array_foreach(a, i, x) {
        json_t *n = json_object_get(x, "name");
        if (!json_is_string(n))
            return false;
        const char *s = json_string_value(n);
        if (prev && strcmp(prev, s) > 0)
            return false;
        prev = s;
    }
    return true;
}

static bool json_bool_value(json_t *v, bool *out)
{
    if (json_is_true(v)) {
        *out = true;
        return true;
    }
    if (json_is_false(v)) {
        *out = false;
        return true;
    }
    if (json_is_integer(v) && (json_integer_value(v) == 0 || json_integer_value(v) == 1)) {
        *out = json_integer_value(v) != 0;
        return true;
    }
    return false;
}

static void assert_suspended(test_state *t, json_t *market_obj, bool expected, const char *market_name)
{
    assertion(t, json_is_object(market_obj), "market '%s' was not present as an object", market_name);
    if (!json_is_object(market_obj))
        return;

    json_t *s = json_object_get(market_obj, "suspended");
    bool actual = false;
    bool valid = json_bool_value(s, &actual);
    assertion(t, valid, "market '%s' missing boolean-like 'suspended' field", market_name);
    if (valid)
        assertion(t, actual == expected, "market '%s' expected suspended=%s, got %s",
                  market_name, expected ? "true" : "false", actual ? "true" : "false");
}

static uint64_t order_id(json_t *r)
{
    json_t *x = result(r);
    if (!json_is_object(x))
        return 0;
    json_t *id = json_object_get(x, "id");
    return json_is_integer(id) ? (uint64_t)json_integer_value(id) : 0;
}

static json_t *balance_update(uint32_t user_id, const char *asset, const char *change, uint64_t business_id)
{
    json_t *a = json_array();
    json_array_append_new(a, json_integer(user_id));
    json_array_append_new(a, json_string(asset));
    json_array_append_new(a, json_string("golden"));
    json_array_append_new(a, json_integer(business_id));
    json_array_append_new(a, json_string(change));
    json_array_append_new(a, json_object());
    json_t *r = rpc_call(CMD_BALANCE_UPDATE, a);
    json_decref(a);
    return r;
}

static json_t *balance_query(uint32_t user_id, const char *asset)
{
    json_t *a = json_array();
    json_array_append_new(a, json_integer(user_id));
    json_array_append_new(a, json_string(asset));
    json_t *r = rpc_call(CMD_BALANCE_QUERY, a);
    json_decref(a);
    return r;
}

static json_t *put_limit(bool http, uint32_t user_id, const char *market, int side,
                         const char *amount, const char *price)
{
    json_t *a = json_array();
    json_array_append_new(a, json_integer(user_id));
    json_array_append_new(a, json_string(market));
    json_array_append_new(a, json_integer(side));
    json_array_append_new(a, json_string(amount));
    json_array_append_new(a, json_string(price));
    json_array_append_new(a, json_string("0.001"));
    json_array_append_new(a, json_string("0.001"));
    json_array_append_new(a, json_string("golden"));
    json_t *r = http ? http_call("order.put_limit", a) : rpc_call(CMD_ORDER_PUT_LIMIT, a);
    json_decref(a);
    return r;
}

static json_t *pending(uint32_t user_id, const char *market)
{
    json_t *a = json_array();
    json_array_append_new(a, json_integer(user_id));
    json_array_append_new(a, json_string(market));
    json_array_append_new(a, json_integer(0));
    json_array_append_new(a, json_integer(100));
    json_t *r = rpc_call(CMD_ORDER_QUERY, a);
    json_decref(a);
    return r;
}

static json_t *cancel_order(uint32_t user_id, const char *market, uint64_t id)
{
    json_t *a = json_array();
    json_array_append_new(a, json_integer(user_id));
    json_array_append_new(a, json_string(market));
    json_array_append_new(a, json_integer(id));
    json_t *r = rpc_call(CMD_ORDER_CANCEL, a);
    json_decref(a);
    return r;
}

static bool pending_has(json_t *r, uint64_t id)
{
    json_t *x = result(r);
    json_t *records = x ? json_object_get(x, "records") : NULL;
    if (!json_is_array(records))
        return false;
    size_t i;
    json_t *o;
    json_array_foreach(records, i, o) {
        json_t *j = json_object_get(o, "id");
        if (json_is_integer(j) && (uint64_t)json_integer_value(j) == id)
            return true;
    }
    return false;
}

static const char *bal_field(json_t *r, const char *asset, const char *field)
{
    json_t *x = result(r);
    json_t *a = x ? json_object_get(x, asset) : NULL;
    json_t *v = a ? json_object_get(a, field) : NULL;
    return json_is_string(v) ? json_string_value(v) : NULL;
}

static char *sql1(const char *query)
{
    /*
     * Feed SQL to mysql through a temporary file instead of embedding the query
     * in a shell command. Queries in these tests legitimately contain MySQL
     * backtick identifiers; putting them inside `sh -c "...` causes command
     * substitution (for example, `operlog_20260904` becomes a shell command).
     */
    char path[] = "/tmp/golden_sql_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "    SQL DIAG: mkstemp failed: %s\n", strerror(errno));
        return NULL;
    }

    size_t len = strlen(query);
    ssize_t written = write(fd, query, len);
    if (written != (ssize_t)len || write(fd, "\n", 1) != 1) {
        fprintf(stderr, "    SQL DIAG: failed to write query file: %s\n", strerror(errno));
        close(fd);
        unlink(path);
        return NULL;
    }
    close(fd);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "mysql -N -B -uuser -ppass trade_log < '%s' 2>/tmp/golden_mysql_stderr.log",
             path);

    FILE *f = popen(cmd, "r");
    if (!f) {
        unlink(path);
        return NULL;
    }

    char *out = calloc(1, 16384);
    size_t n = fread(out, 1, 16383, f);
    out[n] = 0;
    int rc = pclose(f);
    unlink(path);

    if (rc != 0) {
        FILE *ef = fopen("/tmp/golden_mysql_stderr.log", "r");
        if (ef) {
            char errbuf[2048] = {0};
            size_t en = fread(errbuf, 1, sizeof(errbuf) - 1, ef);
            errbuf[en] = 0;
            fclose(ef);
            fprintf(stderr, "    SQL DIAG: query failed: %s\n", query);
            if (errbuf[0])
                fprintf(stderr, "    SQL DIAG: mysql: %s", errbuf);
        }
        free(out);
        return NULL;
    }

    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = 0;
    return out;
}

static long sql_long(const char *query, long fallback)
{
    char *s = sql1(query);
    if (!s || !*s) {
        free(s);
        return fallback;
    }
    long v = strtol(s, NULL, 10);
    free(s);
    return v;
}

static bool wait_for_new_slice(long previous_id, long *id_out, char *ts_out, size_t ts_len, int seconds)
{
    for (int i = 0; i < seconds * 4; i++) {
        char q[256];
        snprintf(q, sizeof(q), "SELECT CONCAT(id,' ',time) FROM slice_history WHERE id > %ld ORDER BY id DESC LIMIT 1", previous_id);
        char *s = sql1(q);
        if (s && *s) {
            long id = 0;
            char ts[64] = {0};
            if (sscanf(s, "%ld %63s", &id, ts) == 2) {
                if (id_out)
                    *id_out = id;
                if (ts_out && ts_len)
                    snprintf(ts_out, ts_len, "%s", ts);
                free(s);
                return true;
            }
        }
        free(s);
        usleep(250000);
    }
    return false;
}

static bool wait_http_ready(int seconds)
{
    for (int i = 0; i < seconds * 5; i++) {
        json_t *a = A0();
        json_t *r = http_call("asset.list", a);
        json_decref(a);
        bool ok = last_http_status == 200 && is_ok(r);
        if (r)
            json_decref(r);
        if (ok)
            return true;
        usleep(200000);
    }
    return false;
}

static bool wait_port_c(int port, int seconds)
{
    for (int i = 0; i < seconds * 5; i++) {
        int s = connect_tcp(ME_HOST, port);
        if (s >= 0) {
            close(s);
            return true;
        }
        usleep(200000);
    }
    return false;
}

/*
 * Restart the whole dependent stack, not only matchengine. accesshttp maintains
 * RPC clients to matchengine; keeping it alive across a forced engine restart
 * can leave the gateway serving HTTP 500 responses from a stale connection.
 */
static void diag_file(const char *label, const char *path)
{
    FILE *f = fopen(path, "r");
    fprintf(stderr, "    DIAG: %s (%s):\n", label, path);
    if (!f) {
        fprintf(stderr, "<unavailable>\n");
        return;
    }

    char line[4096];
    int lines = 0;
    while (fgets(line, sizeof(line), f)) {
        fputs(line, stderr);
        if (++lines >= 80) {
            fprintf(stderr, "    ... log truncated after 80 lines ...\n");
            break;
        }
    }
    fclose(f);
}

static void stop_exchange_stack(void)
{
    /*
     * Request an orderly stop first, then forcefully clear any process that is
     * still alive. A fixed one-second sleep was racy and could leave the old
     * listener bound while the replacement process started.
     */
    system("pkill -TERM -x marketprice.exe >/dev/null 2>&1 || true; "
           "pkill -TERM -x accesshttp.exe >/dev/null 2>&1 || true; "
           "pkill -TERM -x matchengine.exe >/dev/null 2>&1 || true");

    for (int i = 0; i < 40; i++) {
        int rc = system("pgrep -x marketprice.exe >/dev/null 2>&1 || "
                        "pgrep -x accesshttp.exe >/dev/null 2>&1 || "
                        "pgrep -x matchengine.exe >/dev/null 2>&1");
        if (rc != 0)
            return;
        usleep(100000);
    }

    system("pkill -KILL -x marketprice.exe >/dev/null 2>&1 || true; "
           "pkill -KILL -x accesshttp.exe >/dev/null 2>&1 || true; "
           "pkill -KILL -x matchengine.exe >/dev/null 2>&1 || true");
    usleep(250000);
}

/*
 * Restart the whole dependent stack, not only matchengine. accesshttp maintains
 * RPC clients to matchengine; keeping it alive across a forced engine restart
 * can leave the gateway serving HTTP 500 responses from a stale connection.
 */
static bool restart_service_stack(void)
{
    const char *root = getenv("REPO_ROOT");
    const char *me_cfg = getenv("GOLDEN_ME_CONFIG");
    const char *ah_cfg = getenv("GOLDEN_AH_CONFIG");
    const char *mp_cfg = getenv("GOLDEN_MP_CONFIG");
    if (!root)
        root = "/app";
    if (!me_cfg || !*me_cfg)
        me_cfg = "config.json";
    if (!ah_cfg || !*ah_cfg)
        ah_cfg = "config.json";
    if (!mp_cfg || !*mp_cfg)
        mp_cfg = "config.json";

    stop_exchange_stack();

    char cmd[8192];

    /* Step 1: Start matchengine and require its RPC listener. */
    snprintf(cmd, sizeof(cmd),
             "cd '%s/matchengine' && nohup ./matchengine.exe '%s' >/tmp/golden_matchengine_restart.log 2>&1 &",
             root, me_cfg);
    system(cmd);
    if (!wait_port_c(ME_PORT, 15)) {
        diag_file("matchengine restart log", "/tmp/golden_matchengine_restart.log");
        system("tail -80 /var/log/trade/matchengine* 2>/dev/null >&2 || true");
        return false;
    }

    /* Step 2: Start accesshttp and require a successful live JSON request. */
    snprintf(cmd, sizeof(cmd),
             "cd '%s/accesshttp' && nohup ./accesshttp.exe '%s' >/tmp/golden_accesshttp_restart.log 2>&1 &",
             root, ah_cfg);
    system(cmd);
    if (!wait_http_ready(15)) {
        diag_file("accesshttp restart log", "/tmp/golden_accesshttp_restart.log");
        diag_file("matchengine restart log", "/tmp/golden_matchengine_restart.log");
        system("tail -80 /var/log/trade/accesshttp* /var/log/trade/matchengine* 2>/dev/null >&2 || true");
        return false;
    }

    /*
     * Step 3: Start marketprice only after accesshttp is healthy because
     * marketprice fetches market.list through the gateway during initialization.
     */
    snprintf(cmd, sizeof(cmd),
             "cd '%s/marketprice' && nohup ./marketprice.exe '%s' >/tmp/golden_marketprice_restart.log 2>&1 &",
             root, mp_cfg);
    system(cmd);
    if (!wait_port_c(MP_PORT, 15)) {
        diag_file("marketprice restart log", "/tmp/golden_marketprice_restart.log");
        system("tail -80 /var/log/trade/marketprice* 2>/dev/null >&2 || true");
        return false;
    }

    return true;
}

static bool runtime_table(const char *t)
{
    const char *pfx[] = {"operlog_", "slice_asset_", "slice_market_", "slice_order_", "slice_balance_"};
    for (size_t i = 0; i < sizeof(pfx) / sizeof(pfx[0]); i++) {
        size_t n = strlen(pfx[i]);
        if (!strncmp(t, pfx[i], n) && t[n] >= '0' && t[n] <= '9')
            return true;
    }
    return false;
}

static bool purge_runtime_persistence(void)
{
    char *tables = sql1("SHOW TABLES");
    if (!tables)
        return false;
    char *save = NULL;
    for (char *x = strtok_r(tables, "\n", &save); x; x = strtok_r(NULL, "\n", &save)) {
        if (runtime_table(x)) {
            char q[256];
            snprintf(q, sizeof(q), "DROP TABLE IF EXISTS `%s`", x);
            char *o = sql1(q);
            if (!o) {
                free(tables);
                return false;
            }
            free(o);
        }
    }
    free(tables);
    char *o = sql1("TRUNCATE TABLE slice_history");
    if (!o)
        return false;
    free(o);
    return true;
}

static json_t *http_list(const char *method)
{
    json_t *a = A0();
    json_t *r = http_call(method, a);
    json_decref(a);
    return r;
}

static bool wait_market_suspended_http(const char *name, bool expected, int seconds, json_t **last_response)
{
    if (last_response)
        *last_response = NULL;
    for (int i = 0; i < seconds * 5; i++) {
        json_t *r = http_list("market.list");
        json_t *m = r ? find_named(result(r), name) : NULL;
        bool actual = false;
        bool found = m && json_bool_value(json_object_get(m, "suspended"), &actual);
        if (found && actual == expected && is_ok(r)) {
            if (last_response)
                *last_response = r;
            else
                json_decref(r);
            return true;
        }
        if (last_response && *last_response)
            json_decref(*last_response);
        if (last_response)
            *last_response = r;
        else if (r)
            json_decref(r);
        usleep(200000);
    }
    return false;
}

static json_t *market_status_call(const char *market)
{
    json_t *a = json_array();
    json_array_append_new(a, json_string(market));
    json_array_append_new(a, json_integer(60));
    json_t *r = http_call("market.status", a);
    json_decref(a);
    return r;
}

static bool wait_marketprice_available(const char *market, int seconds, json_t **last_response)
{
    if (last_response)
        *last_response = NULL;

    for (int i = 0; i < seconds * 5; i++) {
        json_t *r = market_status_call(market);
        if (last_http_status == 200 && is_ok(r) && json_is_object(result(r))) {
            if (last_response)
                *last_response = r;
            else
                json_decref(r);
            return true;
        }

        if (last_response && *last_response)
            json_decref(*last_response);
        if (last_response)
            *last_response = r;
        else if (r)
            json_decref(r);
        usleep(200000);
    }
    return false;
}

/*
 * The delivered golden implementation records refresh receipt in marketprice's
 * service log. Its market.status payload does not serialize the internal
 * suspended flag, so use the service's runtime refresh event plus a successful
 * market.status request as the black-box evidence that the refreshed registry
 * is live in marketprice.
 */
static long count_marketprice_refresh_events(void)
{
    /*
     * dlog writes marketprice info messages under /var/log/trade/marketprice*
     * while nohup only captures startup stderr. Count both locations so the
     * assertion works with the service's configured logger.
     */
    const char *fixed[] = {
        "/tmp/golden_marketprice.log",
        "/tmp/golden_marketprice_restart.log",
    };
    long count = 0;

    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        FILE *f = fopen(fixed[i], "r");
        if (!f)
            continue;
        char line[4096];
        while (fgets(line, sizeof(line), f))
            if (strstr(line, "refresh data from:"))
                count++;
        fclose(f);
    }

    glob_t g = {0};
    if (glob("/var/log/trade/marketprice*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            FILE *f = fopen(g.gl_pathv[i], "r");
            if (!f)
                continue;
            char line[4096];
            while (fgets(line, sizeof(line), f))
                if (strstr(line, "refresh data from:"))
                    count++;
            fclose(f);
        }
    }
    globfree(&g);
    return count;
}

static bool wait_for_marketprice_refresh_after(long before, int seconds)
{
    for (int i = 0; i < seconds * 5; i++) {
        if (count_marketprice_refresh_events() > before)
            return true;
        usleep(200000);
    }
    return false;
}

/* ----------------------------- F2P test cases ----------------------------- */

static void test_f2p_fresh_asset_list(f2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "fresh install: HTTP asset.list is empty");
    json_t *r = http_list("asset.list");
    assert_http_success(&t, r, "asset.list");
    assert_result_array(&t, r, "asset.list");
    if (json_is_array(result(r)))
        assert_array_size(&t, result(r), 0, "fresh asset.list result");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_fresh_market_list_rpc(f2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "fresh install: direct matchengine RPC market.list is empty");
    json_t *a = A0();
    json_t *r = rpc_call(CMD_MARKET_LIST, a);
    json_decref(a);
    assert_rpc_success(&t, r, "RPC market.list");
    assert_result_array(&t, r, "RPC market.list");
    if (json_is_array(result(r)))
        assert_array_size(&t, result(r), 0, "fresh RPC market.list result");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_asset_create_missing_field(f2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "asset.create rejects missing required prec_show");
    json_t *a = json_array();
    json_array_append_new(a, json_string("BADASSET"));
    json_array_append_new(a, json_integer(12));
    json_t *r = http_call("asset.create", a);
    json_decref(a);
    assert_http_rpc_error(&t, r, "asset.create missing prec_show");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_asset_create_a(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "HTTP asset.create creates first asset");
    json_t *a = asset_params(c->asset_a, 12, 8);
    json_t *r = http_call("asset.create", a);
    json_decref(a);
    assert_http_success(&t, r, "asset.create GOLDENA");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_asset_create_b(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "HTTP asset.create creates second asset");
    json_t *a = asset_params(c->asset_b, 12, 8);
    json_t *r = http_call("asset.create", a);
    json_decref(a);
    assert_http_success(&t, r, "asset.create GOLDENB");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_asset_list_sorted(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "asset.list reflects creates and has deterministic sorted order");
    json_t *r = http_list("asset.list");
    assert_http_success(&t, r, "asset.list");
    assert_result_array(&t, r, "asset.list");
    json_t *a = result(r);
    if (json_is_array(a)) {
        assert_array_size(&t, a, 2, "asset.list after two HTTP creates");
        assert_named_present(&t, a, c->asset_a, "asset.list");
        assert_named_present(&t, a, c->asset_b, "asset.list");
        assertion(&t, names_sorted(a), "asset.list names were not in deterministic ascending order");
        if (!names_sorted(a)) diag_json("actual result", a);
    }
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_market_create_missing_fields(f2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "market.create rejects missing required market fields");
    json_t *a = json_array();
    json_array_append_new(a, json_string("BADMARKET"));
    json_array_append_new(a, json_integer(4));
    json_t *r = http_call("market.create", a);
    json_decref(a);
    assert_http_rpc_error(&t, r, "incomplete market.create");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_cli_asset_create(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "live CLI asset create operation succeeds");
    char command[256];
    snprintf(command, sizeof(command), "asset create %s 12 8", c->asset_c);
    char *out = cli_call(command);
    assert_cli_success(&t, command, out);
    free(out);
    end_test(&t);
}

static void test_f2p_cli_asset_visible_http(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "asset created through CLI is visible through HTTP asset.list");
    json_t *r = http_list("asset.list");
    assert_http_success(&t, r, "asset.list");
    assert_result_array(&t, r, "asset.list");
    if (json_is_array(result(r))) {
        assert_array_size(&t, result(r), 3, "asset.list after CLI create");
        assert_named_present(&t, result(r), c->asset_c, "asset.list");
    }
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_cli_market_create(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "live CLI market create operation succeeds");
    char command[512];
    snprintf(command, sizeof(command), "market create %s 4 0.001 %s 4 %s 4",
             c->market_cli, c->asset_c, c->asset_b);
    char *out = cli_call(command);
    assert_cli_success(&t, command, out);
    free(out);

    json_t *r = http_list("market.list");
    assert_http_success(&t, r, "market.list after CLI market create");
    if (r && json_is_array(result(r)))
        assert_named_present(&t, result(r), c->market_cli, "market.list after CLI market create");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_cli_market_suspend(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "live CLI market suspend operation succeeds");
    char command[256];
    snprintf(command, sizeof(command), "market suspend %s", c->market_cli);
    char *out = cli_call(command);
    assert_cli_success(&t, command, out);
    free(out);
    end_test(&t);
}

static void test_f2p_cli_suspend_visible(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "CLI suspension is immediately visible through HTTP market.list");
    json_t *r = NULL;
    bool observed = wait_market_suspended_http(c->market_cli, true, 3, &r);
    assertion(&t, observed, "market.list did not report '%s' with suspended=true within 3 seconds", c->market_cli);
    if (!observed) {
        assertion(&t, last_http_status == 200, "last market.list HTTP status expected 200, got %ld", last_http_status);
        diag_json("last market.list response", r);
    }
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_cli_market_resume(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "live CLI market resume operation succeeds");
    char command[256];
    snprintf(command, sizeof(command), "market resume %s", c->market_cli);
    char *out = cli_call(command);
    assert_cli_success(&t, command, out);
    free(out);
    json_t *r = NULL;
    bool observed = wait_market_suspended_http(c->market_cli, false, 3, &r);
    assertion(&t, observed, "market.list did not report '%s' with suspended=false after CLI resume", c->market_cli);
    if (!observed) diag_json("last market.list response", r);
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_cli_market_delist(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "live CLI market delist operation succeeds");
    char command[256];
    snprintf(command, sizeof(command), "market delist %s", c->market_cli);
    char *out = cli_call(command);
    assert_cli_success(&t, command, out);
    free(out);
    end_test(&t);
}

static void test_f2p_cli_delist_visible(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "CLI-delisted market disappears from HTTP market.list");
    json_t *r = http_list("market.list");
    assert_http_success(&t, r, "market.list after CLI delist");
    if (json_is_array(result(r)))
        assert_named_absent(&t, result(r), c->market_cli, "market.list");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_http_market_create(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "HTTP market.create creates a tradable market");
    json_t *a = market_params(c->market_main, c->asset_a, c->asset_b);
    json_t *r = http_call("market.create", a);
    json_decref(a);
    assert_http_success(&t, r, "market.create GOLDENAB");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_rpc_market_list_suspended_false(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "direct RPC market.list exposes new market with suspended=false");
    json_t *a = A0();
    json_t *r = rpc_call(CMD_MARKET_LIST, a);
    json_decref(a);
    assert_rpc_success(&t, r, "RPC market.list");
    assert_result_array(&t, r, "RPC market.list");
    if (json_is_array(result(r))) {
        assert_array_size(&t, result(r), 1, "market.list after CLI market was delisted and GOLDENAB created");
        json_t *m = find_named(result(r), c->market_main);
        assert_named_present(&t, result(r), c->market_main, "RPC market.list");
        if (m) assert_suspended(&t, m, false, c->market_main);
    }
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_marketprice_create_refresh(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "marketprice sees dynamically created market without restart");

    /*
     * Step 1: Poll market.status through accesshttp. Immediately after
     * market.create, marketprice may briefly reject the market until the pushed
     * registry refresh is consumed.
     */
    json_t *r = NULL;
    bool observed = wait_marketprice_available(c->market_main, 5, &r);

    /* Step 2: The same already-running marketprice must learn the new market. */
    assertion(&t, observed,
              "market.status did not become callable for dynamically created market '%s' within 5 seconds",
              c->market_main);
    if (!observed) {
        assertion(&t, last_http_status == 200,
                  "last market.status HTTP status expected 200, got %ld; body=%s",
                  last_http_status, last_http_body[0] ? last_http_body : "<empty>");
        diag_json("last market.status response", r);
    } else {
        assertion(&t, json_is_object(result(r)),
                  "market.status for '%s' expected an object result", c->market_main);
    }

    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_balance_fund(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "direct RPC balance.update funds stock asset for order tests");
    json_t *r = balance_update(c->user_id, c->asset_a, "10", ++c->business_id);
    assert_rpc_success(&t, r, "balance.update GOLDENA +10");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_active_order(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "direct RPC order.put_limit accepts order on active market");
    json_t *r = put_limit(false, c->user_id, c->market_main, 1, "1", "1");
    assert_rpc_success(&t, r, "RPC order.put_limit on active GOLDENAB");
    c->active_order_id = order_id(r);
    assertion(&t, c->active_order_id != 0, "order.put_limit success response did not contain a nonzero order id");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_http_suspend(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "HTTP market.suspend succeeds");

    /* Step 1: Record marketprice refresh activity before the state change. */
    c->marketprice_refresh_before_suspend = count_marketprice_refresh_events();

    /* Step 2: Suspend the market through the public HTTP administration API. */
    json_t *a = json_array();
    json_array_append_new(a, json_string(c->market_main));
    json_t *r = http_call("market.suspend", a);
    json_decref(a);
    assert_http_success(&t, r, "market.suspend GOLDENAB");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_http_list_suspended(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "HTTP market.list reports suspended=true after suspend");
    json_t *r = NULL;
    bool observed = wait_market_suspended_http(c->market_main, true, 3, &r);
    assertion(&t, observed, "market.list did not report '%s' suspended=true within 3 seconds", c->market_main);
    if (!observed) diag_json("last market.list response", r);
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_marketprice_suspend_refresh(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "marketprice refresh propagates suspended=true to market.status");

    /*
     * Step 1: Verify the suspend operation caused a new automatic refresh to be
     * received by the already-running marketprice component.
     */
    bool refreshed = wait_for_marketprice_refresh_after(
        c->marketprice_refresh_before_suspend, 10);
    assertion(&t, refreshed,
              "marketprice did not record an automatic registry refresh within 10 seconds after market.suspend");

    /*
     * Step 2: Verify marketprice still recognizes the refreshed market through
     * its market.status API. The delivered golden implementation stores the
     * suspended bit internally but does not serialize that bit in market.status;
     * market.list is the externally visible suspended-field assertion and is
     * tested immediately before this case.
     */
    json_t *r = NULL;
    bool available = wait_marketprice_available(c->market_main, 5, &r);
    assertion(&t, available,
              "market.status stopped recognizing '%s' after suspension refresh",
              c->market_main);
    if (!available) {
        assertion(&t, last_http_status == 200,
                  "last market.status HTTP status expected 200, got %ld; body=%s",
                  last_http_status, last_http_body[0] ? last_http_body : "<empty>");
        diag_json("last market.status response", r);
    }

    if (r) json_decref(r);
    end_test(&t);
}

static void assert_market_is_currently_suspended(test_state *t, const char *market)
{
    json_t *r = http_list("market.list");
    assert_http_success(t, r, "market.list suspension precondition");
    json_t *m = r ? find_named(result(r), market) : NULL;
    assert_named_present(t, result(r), market, "market.list suspension precondition");
    if (m) assert_suspended(t, m, true, market);
    if (r) json_decref(r);
}

static void test_f2p_http_order_rejected_suspended(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "HTTP order.put_limit is rejected while market is suspended");
    assert_market_is_currently_suspended(&t, c->market_main);
    json_t *r = put_limit(true, c->user_id, c->market_main, 1, "1", "1");
    assert_http_rpc_error(&t, r, "HTTP order.put_limit on suspended market");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_rpc_order_rejected_suspended(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "direct RPC order.put_limit is rejected while market is suspended");
    assert_market_is_currently_suspended(&t, c->market_main);
    json_t *r = put_limit(false, c->user_id, c->market_main, 1, "1", "1");
    assertion(&t, r != NULL, "RPC order.put_limit returned no response");
    if (r)
        assertion(&t, !is_ok(r), "RPC order.put_limit unexpectedly succeeded while market was suspended");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_cancel_while_suspended(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "direct RPC order.cancel remains allowed while market is suspended");
    assertion(&t, c->active_order_id != 0, "pre-suspension order id is unavailable; active-order setup did not succeed");
    json_t *r = c->active_order_id ? cancel_order(c->user_id, c->market_main, c->active_order_id) : NULL;
    assert_rpc_success(&t, r, "RPC order.cancel while suspended");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_http_resume(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "HTTP market.resume succeeds");
    json_t *a = json_array();
    json_array_append_new(a, json_string(c->market_main));
    json_t *r = http_call("market.resume", a);
    json_decref(a);
    assert_http_success(&t, r, "market.resume GOLDENAB");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_order_after_resume(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "orders are accepted again after resume");
    json_t *state = NULL;
    bool resumed = wait_market_suspended_http(c->market_main, false, 3, &state);
    assertion(&t, resumed, "market.list did not show '%s' resumed before order submission", c->market_main);
    if (!resumed) diag_json("last market.list response", state);
    if (state) json_decref(state);

    json_t *r = put_limit(false, c->user_id, c->market_main, 1, "1", "1.5");
    assert_rpc_success(&t, r, "RPC order.put_limit after resume");
    c->slice_order_id = order_id(r);
    assertion(&t, c->slice_order_id != 0, "resumed order response did not contain an order id");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_cli_makeslice(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "live matchengine CLI makeslice command succeeds");
    c->slice_before_id = sql_long("SELECT COALESCE(MAX(id),0) FROM slice_history", 0);
    char *out = cli_call("makeslice");
    assert_cli_success(&t, "makeslice", out);
    assertion(&t, out && strcasestr(out, "OK"), "makeslice CLI response expected to contain OK; actual=%s", out ? out : "<null>");
    free(out);
    end_test(&t);
}

static void test_f2p_slice_history_row(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "makeslice writes a slice_history row to deployed MariaDB");
    bool found = wait_for_new_slice(c->slice_before_id, &c->slice_id, c->slice_ts, sizeof(c->slice_ts), 10);
    assertion(&t, found, "no new slice_history row appeared within 10 seconds after makeslice (previous max id=%ld)", c->slice_before_id);
    if (!found) {
        char *rows = sql1("SELECT id,time,end_oper_id,end_order_id,end_deals_id FROM slice_history ORDER BY id DESC LIMIT 5");
        fprintf(stderr, "    DIAG: current slice_history rows: %s\n", rows && *rows ? rows : "<none>");
        free(rows);
        system("tail -80 /tmp/golden_matchengine.log 2>/dev/null >&2 || true");
        system("tail -80 /var/log/trade/matchengine* 2>/dev/null >&2 || true");
    }
    end_test(&t);
}

static void test_f2p_slice_tables(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "each runtime slice creates asset, market, order, and balance tables");
    assertion(&t, c->slice_ts[0] != '\0', "slice timestamp is unavailable because slice_history test did not observe a row");
    if (c->slice_ts[0]) {
        char q[1024];
        snprintf(q, sizeof(q),
                 "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='trade_log' AND table_name IN "
                 "('slice_asset_%s','slice_market_%s','slice_order_%s','slice_balance_%s')",
                 c->slice_ts, c->slice_ts, c->slice_ts, c->slice_ts);
        long count = sql_long(q, -1);
        assertion(&t, count == 4, "expected 4 timestamped slice tables for ts=%s, found %ld", c->slice_ts, count);
        if (count != 4) {
            char *tables = sql1("SHOW TABLES LIKE 'slice_%'");
            fprintf(stderr, "    DIAG: actual slice tables:\n%s\n", tables ? tables : "<query failed>");
            free(tables);
        }
    }
    end_test(&t);
}

static void test_f2p_unknown_deal_no_market(f2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "deal message for unknown market does not create marketprice state");
    int rc = system("printf '%s\\n' '[1700000000.0,\"GHOSTDEAL\",1,2,11,12,\"1\",\"1\",\"0\",\"0\",1,999999,\"GOLDENA\",\"GOLDENB\"]' | /opt/kafka/bin/kafka-console-producer.sh --broker-list 127.0.0.1:9092 --topic deals >/dev/null 2>&1");
    assertion(&t, rc == 0, "Kafka producer failed while sending unknown-market deal (system rc=%d)", rc);
    sleep(1);
    json_t *r = market_status_call("GHOSTDEAL");
    assertion(&t, last_http_status == 200, "market.status expected HTTP 200 JSON-RPC response, got %ld", last_http_status);
    assertion(&t, r && !is_ok(r), "market.status for GHOSTDEAL unexpectedly succeeded; deal message must not create a market");
    if (r && is_ok(r)) diag_json("actual response", r);
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_post_slice_market_create(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "market can be created after the latest slice");
    json_t *a = market_params(c->market_post, c->asset_a, c->asset_b);
    json_t *r = http_call("market.create", a);
    json_decref(a);
    assert_http_success(&t, r, "market.create GOLDENPOST");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_post_slice_order(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "order can be placed on a market created after the latest slice");
    json_t *r = put_limit(false, c->user_id, c->market_post, 1, "1", "2");
    assert_rpc_success(&t, r, "RPC order.put_limit on GOLDENPOST");
    c->post_order_id = order_id(r);
    assertion(&t, c->post_order_id != 0, "post-slice order response did not contain an order id");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_post_slice_suspend(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "post-slice market can be suspended before restart");
    json_t *a = json_array();
    json_array_append_new(a, json_string(c->market_post));
    json_t *r = http_call("market.suspend", a);
    json_decref(a);
    assert_http_success(&t, r, "market.suspend GOLDENPOST");
    if (r) json_decref(r);
    json_t *state = NULL;
    bool observed = wait_market_suspended_http(c->market_post, true, 3, &state);
    assertion(&t, observed, "GOLDENPOST did not become suspended before restart");
    if (!observed) diag_json("last market.list response", state);
    if (state) json_decref(state);
    end_test(&t);
}

static void test_f2p_restart(f2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "matchengine restarts successfully against slice_history + operlog");
    bool ok = restart_service_stack();
    assertion(&t, ok, "dependent stack did not restart successfully; check /tmp/golden_*_restart.log");
    end_test(&t);
}

static void test_f2p_restart_markets(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "restart restores sliced market and post-slice suspended market");
    json_t *a = A0();
    json_t *r = rpc_call(CMD_MARKET_LIST, a);
    json_decref(a);
    assert_rpc_success(&t, r, "RPC market.list after restart");
    assert_result_array(&t, r, "RPC market.list after restart");
    json_t *m1 = r ? find_named(result(r), c->market_main) : NULL;
    json_t *m2 = r ? find_named(result(r), c->market_post) : NULL;
    if (json_is_array(result(r))) {
        assert_named_present(&t, result(r), c->market_main, "market.list after restart");
        assert_named_present(&t, result(r), c->market_post, "market.list after restart");
    }
    if (m2) assert_suspended(&t, m2, true, c->market_post);
    if (!m1 || !m2) diag_json("actual response", r);
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_restart_slice_order(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "restart restores open order captured in slice");
    assertion(&t, c->slice_order_id != 0, "slice order id unavailable from pre-slice setup");
    json_t *r = pending(c->user_id, c->market_main);
    assert_rpc_success(&t, r, "order.pending on sliced market after restart");
    if (c->slice_order_id)
        assertion(&t, pending_has(r, c->slice_order_id), "order.pending did not contain sliced order id %llu",
                  (unsigned long long)c->slice_order_id);
    if (!t.ok) diag_json("actual response", r);
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_restart_post_order(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "restart replays post-slice order placed before suspension");
    assertion(&t, c->post_order_id != 0, "post-slice order id unavailable from setup");
    json_t *r = pending(c->user_id, c->market_post);
    assert_rpc_success(&t, r, "order.pending on post-slice market after restart");
    if (c->post_order_id)
        assertion(&t, pending_has(r, c->post_order_id), "order.pending did not contain post-slice order id %llu",
                  (unsigned long long)c->post_order_id);
    if (!t.ok) diag_json("actual response", r);
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_replayed_suspend_rejects(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "replayed suspension still rejects new direct RPC orders");
    assert_market_is_currently_suspended(&t, c->market_post);
    json_t *r = put_limit(false, c->user_id, c->market_post, 1, "1", "2");
    assertion(&t, r != NULL, "RPC order.put_limit returned no response after restart");
    if (r)
        assertion(&t, !is_ok(r), "new order unexpectedly succeeded on replayed suspended market");
    if (r) json_decref(r);
    end_test(&t);
}

static bool resume_market_http(const char *market)
{
    json_t *a = json_array();
    json_array_append_new(a, json_string(market));
    json_t *r = http_call("market.resume", a);
    json_decref(a);
    bool ok = last_http_status == 200 && is_ok(r);
    if (r) json_decref(r);
    return ok;
}

static void test_f2p_delist_open_order(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "HTTP market.delist succeeds with an open order present");

    /*
     * Step 1: Resume the post-slice market so we can create one new open order
     * whose frozen balance will be attributable specifically to this test.
     */
    assertion(&t, resume_market_http(c->market_post),
              "could not resume GOLDENPOST before preparing delist order");

    /*
     * Step 2: Capture the user's existing frozen GOLDENA balance. Earlier test
     * cases intentionally leave another order open on GOLDENAB, so the global
     * freeze balance is not expected to start at zero.
     */
    json_t *before = balance_query(c->user_id, c->asset_a);
    assert_rpc_success(&t, before, "balance.query before delist-specific order");
    const char *before_freeze = before ? bal_field(before, c->asset_a, "freeze") : NULL;
    assertion(&t, before_freeze != NULL,
              "balance.query before delist-specific order missing %s.freeze", c->asset_a);
    c->delist_freeze_before = before_freeze ? strtod(before_freeze, NULL) : -1.0;
    if (before) json_decref(before);

    /* Step 3: Place a fresh order on GOLDENPOST that delist must cancel. */
    json_t *r = put_limit(false, c->user_id, c->market_post, 1, "1", "3");
    assert_rpc_success(&t, r, "order.put_limit before delist");
    c->delist_order_id = order_id(r);
    assertion(&t, c->delist_order_id != 0,
              "pre-delist order did not receive an id");
    if (r) json_decref(r);

    /*
     * Step 4: Prove the new order actually increased the frozen balance. This
     * prevents the later refund assertion from passing without an open order.
     */
    json_t *with_order = balance_query(c->user_id, c->asset_a);
    assert_rpc_success(&t, with_order, "balance.query after delist-specific order");
    const char *with_freeze = with_order ? bal_field(with_order, c->asset_a, "freeze") : NULL;
    assertion(&t, with_freeze != NULL,
              "balance.query after delist-specific order missing %s.freeze", c->asset_a);
    c->delist_freeze_with_order = with_freeze ? strtod(with_freeze, NULL) : -1.0;
    if (with_freeze) {
        assertion(&t, c->delist_freeze_with_order > c->delist_freeze_before,
                  "placing delist-specific order should increase %s.freeze: before=%.12g after=%.12g",
                  c->asset_a, c->delist_freeze_before, c->delist_freeze_with_order);
    }
    if (with_order) json_decref(with_order);

    /* Step 5: Delist the market through the public administration API. */
    json_t *a = json_array();
    json_array_append_new(a, json_string(c->market_post));
    r = http_call("market.delist", a);
    json_decref(a);
    assert_http_success(&t, r, "market.delist GOLDENPOST");
    if (r) json_decref(r);
    usleep(500000);
    end_test(&t);
}

static void test_f2p_delist_refund(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "delist refunds frozen balance from canceled open orders");

    /* Step 1: Query the same stock balance after market.delist completes. */
    json_t *r = balance_query(c->user_id, c->asset_a);
    assert_rpc_success(&t, r, "balance.query after delist");
    const char *freeze = bal_field(r, c->asset_a, "freeze");
    assertion(&t, freeze != NULL,
              "balance.query result missing %s.freeze", c->asset_a);

    /*
     * Step 2: Compare against the baseline captured immediately before the
     * delist-specific order. Other markets may legitimately still have open
     * orders freezing the same asset, so total freeze need not be zero.
     */
    if (freeze) {
        double after = strtod(freeze, NULL);
        double delta = after - c->delist_freeze_before;
        assertion(&t, fabs(delta) < 1e-9,
                  "delist should refund only its canceled order's freeze: baseline=%.12g with_order=%.12g after=%.12g",
                  c->delist_freeze_before, c->delist_freeze_with_order, after);
    }

    if (!t.ok) diag_json("actual response", r);
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_delist_removed(f2p_ctx *c)
{
    test_state t;
    begin_test(&t, "delisted market disappears from market.list");
    json_t *r = http_list("market.list");
    assert_http_success(&t, r, "market.list after delist");
    if (json_is_array(result(r)))
        assert_named_absent(&t, result(r), c->market_post, "market.list after delist");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_f2p_admin_operlog(f2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "asset/market admin state changes are persisted as operlog entries");
    sleep(1);
    char *table = sql1("SELECT table_name FROM information_schema.tables WHERE table_schema='trade_log' AND table_name REGEXP '^operlog_[0-9]{8}$' ORDER BY table_name DESC LIMIT 1");
    assertion(&t, table && *table, "no dated runtime operlog_YYYYMMDD table exists (operlog_example is only a schema template)");
    if (!(table && *table)) {
        char *tables = sql1("SELECT table_name FROM information_schema.tables WHERE table_schema='trade_log' AND table_name LIKE 'operlog\_%' ORDER BY table_name");
        fprintf(stderr, "    DIAG: available operlog tables:\n%s\n", tables && *tables ? tables : "<none>");
        free(tables);
    }
    if (table && *table) {
        char q[1024];
        snprintf(q, sizeof(q), "SELECT COUNT(*) FROM `%s` WHERE detail LIKE '%%GOLDENPOST%%'", table);
        long count = sql_long(q, -1);
        assertion(&t, count >= 5, "expected at least 5 GOLDENPOST-related operlog rows (create/order/suspend/resume/delist), found %ld in %s", count, table);
        if (count < 5) {
            snprintf(q, sizeof(q), "SELECT id,detail FROM `%s` WHERE detail LIKE '%%GOLDENPOST%%' ORDER BY id", table);
            char *rows = sql1(q);
            fprintf(stderr, "    DIAG: matching operlog rows:\n%s\n", rows && *rows ? rows : "<none>");
            free(rows);
        }
    }
    free(table);
    end_test(&t);
}

static void test_f2p_slice_retention(f2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "slice retention cleanup deletes aged asset/market/order/balance slice tables");

    long before = sql_long("SELECT COALESCE(MAX(id),0) FROM slice_history", 0);
    char *out = cli_call("makeslice");
    assertion(&t, cli_output_success(out), "pre-retention makeslice command failed: %s", out ? out : "<null>");
    free(out);

    long slice_id = 0;
    char ts[64] = {0};
    bool got = wait_for_new_slice(before, &slice_id, ts, sizeof(ts), 10);
    assertion(&t, got, "pre-retention makeslice did not create a slice within 10 seconds");

    bool aged = false;
    long fake = 0;
    if (got) {
        fake = strtol(ts, NULL, 10) - 400000;
        char q[4096];
        snprintf(q, sizeof(q),
                 "RENAME TABLE slice_asset_%s TO slice_asset_%ld, slice_market_%s TO slice_market_%ld, "
                 "slice_order_%s TO slice_order_%ld, slice_balance_%s TO slice_balance_%ld",
                 ts, fake, ts, fake, ts, fake, ts, fake);
        char *o = sql1(q);
        assertion(&t, o != NULL, "failed to rename latest slice tables to an aged timestamp for retention setup");
        if (o) {
            free(o);
            snprintf(q, sizeof(q), "UPDATE slice_history SET time=%ld WHERE id=%ld", fake, slice_id);
            o = sql1(q);
            aged = o != NULL;
            assertion(&t, aged, "failed to age slice_history row id=%ld", slice_id);
            free(o);
        }
    }

    if (aged) {
        before = sql_long("SELECT COALESCE(MAX(id),0) FROM slice_history", 0);
        out = cli_call("makeslice");
        assertion(&t, cli_output_success(out), "cleanup-trigger makeslice command failed: %s", out ? out : "<null>");
        free(out);
        long new_id = 0;
        char new_ts[64] = {0};
        assertion(&t, wait_for_new_slice(before, &new_id, new_ts, sizeof(new_ts), 10),
                  "cleanup-trigger makeslice did not create a fresh slice");

        bool removed = false;
        for (int i = 0; i < 40; i++) {
            char q[1024];
            snprintf(q, sizeof(q),
                     "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='trade_log' AND table_name IN "
                     "('slice_asset_%ld','slice_market_%ld','slice_order_%ld','slice_balance_%ld')",
                     fake, fake, fake, fake);
            long table_count = sql_long(q, -1);
            snprintf(q, sizeof(q), "SELECT COUNT(*) FROM slice_history WHERE id=%ld", slice_id);
            long history_count = sql_long(q, -1);
            if (table_count == 0 && history_count == 0) {
                removed = true;
                break;
            }
            usleep(250000);
        }
        assertion(&t, removed, "aged slice id=%ld time=%ld was not fully removed within 10 seconds", slice_id, fake);
    }

    end_test(&t);
}

static void test_f2p_remove_persistence_restart(f2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "test can remove slice_history + operlog and restart matchengine");
    stop_exchange_stack();
    bool purged = purge_runtime_persistence();
    assertion(&t, purged, "failed to remove runtime slice/operlog tables and truncate slice_history");
    bool restarted = purged && restart_service_stack();
    assertion(&t, restarted, "service stack did not restart after persistence purge");
    end_test(&t);
}

static void test_f2p_no_aux_source(f2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "registry has no auxiliary always-current source beyond slices + operlog");
    json_t *a = A0();
    json_t *ar = rpc_call(CMD_ASSET_LIST, a);
    json_decref(a);
    assert_rpc_success(&t, ar, "RPC asset.list after persistence purge");
    if (json_is_array(result(ar)))
        assert_array_size(&t, result(ar), 0, "asset registry after deleting slice/operlog persistence");
    else
        assert_result_array(&t, ar, "RPC asset.list after persistence purge");

    a = A0();
    json_t *mr = rpc_call(CMD_MARKET_LIST, a);
    json_decref(a);
    assert_rpc_success(&t, mr, "RPC market.list after persistence purge");
    if (json_is_array(result(mr)))
        assert_array_size(&t, result(mr), 0, "market registry after deleting slice/operlog persistence");
    else
        assert_result_array(&t, mr, "RPC market.list after persistence purge");

    if (ar) json_decref(ar);
    if (mr) json_decref(mr);
    end_test(&t);
}

struct f2p_case {
    const char *name;
    void (*fn)(f2p_ctx *);
};

static const struct f2p_case F2P_CASES[] = {
    { "fresh install: HTTP asset.list is empty", test_f2p_fresh_asset_list },
    { "fresh install: direct matchengine RPC market.list is empty", test_f2p_fresh_market_list_rpc },
    { "asset.create rejects missing required prec_show", test_f2p_asset_create_missing_field },
    { "HTTP asset.create creates first asset", test_f2p_asset_create_a },
    { "HTTP asset.create creates second asset", test_f2p_asset_create_b },
    { "asset.list reflects creates and has deterministic sorted order", test_f2p_asset_list_sorted },
    { "market.create rejects missing required market fields", test_f2p_market_create_missing_fields },
    { "live CLI asset create operation succeeds", test_f2p_cli_asset_create },
    { "asset created through CLI is visible through HTTP asset.list", test_f2p_cli_asset_visible_http },
    { "live CLI market create operation succeeds", test_f2p_cli_market_create },
    { "live CLI market suspend operation succeeds", test_f2p_cli_market_suspend },
    { "CLI suspension is immediately visible through HTTP market.list", test_f2p_cli_suspend_visible },
    { "live CLI market resume operation succeeds", test_f2p_cli_market_resume },
    { "live CLI market delist operation succeeds", test_f2p_cli_market_delist },
    { "CLI-delisted market disappears from HTTP market.list", test_f2p_cli_delist_visible },
    { "HTTP market.create creates a tradable market", test_f2p_http_market_create },
    { "direct RPC market.list exposes new market with suspended=false", test_f2p_rpc_market_list_suspended_false },
    { "marketprice sees dynamically created market without restart", test_f2p_marketprice_create_refresh },
    { "direct RPC balance.update funds stock asset for order tests", test_f2p_balance_fund },
    { "direct RPC order.put_limit accepts order on active market", test_f2p_active_order },
    { "HTTP market.suspend succeeds", test_f2p_http_suspend },
    { "HTTP market.list reports suspended=true after suspend", test_f2p_http_list_suspended },
    { "marketprice refresh propagates suspended=true to market.status", test_f2p_marketprice_suspend_refresh },
    { "HTTP order.put_limit is rejected while market is suspended", test_f2p_http_order_rejected_suspended },
    { "direct RPC order.put_limit is rejected while market is suspended", test_f2p_rpc_order_rejected_suspended },
    { "direct RPC order.cancel remains allowed while market is suspended", test_f2p_cancel_while_suspended },
    { "HTTP market.resume succeeds", test_f2p_http_resume },
    { "orders are accepted again after resume", test_f2p_order_after_resume },
    { "live matchengine CLI makeslice command succeeds", test_f2p_cli_makeslice },
    { "makeslice writes a slice_history row to deployed MariaDB", test_f2p_slice_history_row },
    { "each runtime slice creates asset, market, order, and balance tables", test_f2p_slice_tables },
    { "deal message for unknown market does not create marketprice state", test_f2p_unknown_deal_no_market },
    { "market can be created after the latest slice", test_f2p_post_slice_market_create },
    { "order can be placed on a market created after the latest slice", test_f2p_post_slice_order },
    { "post-slice market can be suspended before restart", test_f2p_post_slice_suspend },
    { "matchengine restarts successfully against slice_history + operlog", test_f2p_restart },
    { "restart restores sliced market and post-slice suspended market", test_f2p_restart_markets },
    { "restart restores open order captured in slice", test_f2p_restart_slice_order },
    { "restart replays post-slice order placed before suspension", test_f2p_restart_post_order },
    { "replayed suspension still rejects new direct RPC orders", test_f2p_replayed_suspend_rejects },
    { "HTTP market.delist succeeds with an open order present", test_f2p_delist_open_order },
    { "delist refunds frozen balance from canceled open orders", test_f2p_delist_refund },
    { "delisted market disappears from market.list", test_f2p_delist_removed },
    { "asset/market admin state changes are persisted as operlog entries", test_f2p_admin_operlog },
    { "slice retention cleanup deletes aged asset/market/order/balance slice tables", test_f2p_slice_retention },
    { "test can remove slice_history + operlog and restart matchengine", test_f2p_remove_persistence_restart },
    { "registry has no auxiliary always-current source beyond slices + operlog", test_f2p_no_aux_source },
};

static void f2p(void)
{
    f2p_ctx c = {
        .asset_a = "GOLDENA",
        .asset_b = "GOLDENB",
        .asset_c = "GOLDENC",
        .market_main = "GOLDENAB",
        .market_cli = "GOLDENCLI",
        .market_post = "GOLDENPOST",
        .user_id = 910001,
        .business_id = 9000000,
    };

    for (size_t i = 0; i < sizeof(F2P_CASES) / sizeof(F2P_CASES[0]); i++)
        F2P_CASES[i].fn(&c);
}

/* ----------------------------- P2P test cases ----------------------------- */

static bool same_name_set(json_t *a, json_t *b)
{
    if (!json_is_array(a) || !json_is_array(b) || json_array_size(a) != json_array_size(b))
        return false;
    size_t i;
    json_t *x;
    json_array_foreach(a, i, x) {
        json_t *n = json_object_get(x, "name");
        if (!json_is_string(n) || !array_has_named(b, json_string_value(n)))
            return false;
    }
    return true;
}

static void p2p_discover_fixture(p2p_ctx *c)
{
    if (c->http_markets && json_is_array(result(c->http_markets)) && json_array_size(result(c->http_markets))) {
        json_t *m = json_array_get(result(c->http_markets), 0);
        json_t *n = json_object_get(m, "name");
        json_t *s = json_object_get(m, "stock");
        json_t *money = json_object_get(m, "money");
        if (json_is_string(n)) snprintf(c->market, sizeof(c->market), "%s", json_string_value(n));
        if (json_is_string(s)) snprintf(c->stock, sizeof(c->stock), "%s", json_string_value(s));
        if (json_is_string(money)) snprintf(c->money, sizeof(c->money), "%s", json_string_value(money));
    }
    if (c->stock[0])
        snprintf(c->asset, sizeof(c->asset), "%s", c->stock);
    else if (c->http_assets && json_is_array(result(c->http_assets)) && json_array_size(result(c->http_assets))) {
        json_t *n = json_object_get(json_array_get(result(c->http_assets), 0), "name");
        if (json_is_string(n)) snprintf(c->asset, sizeof(c->asset), "%s", json_string_value(n));
    }
}

static bool ensure_p2p_fixture(p2p_ctx *c, test_state *t)
{
    p2p_discover_fixture(c);
    if (c->market[0] && c->stock[0])
        return true;

    json_t *p = asset_params("P2PAAA", 12, 8);
    json_t *r = http_call("asset.create", p);
    json_decref(p);
    if (!is_ok(r)) {
        assertion(t, false, "P2P fixture asset.create P2PAAA failed (HTTP %ld)", last_http_status);
        diag_json("actual response", r);
    }
    if (r) json_decref(r);

    p = asset_params("P2PBBB", 12, 8);
    r = http_call("asset.create", p);
    json_decref(p);
    if (!is_ok(r)) {
        assertion(t, false, "P2P fixture asset.create P2PBBB failed (HTTP %ld)", last_http_status);
        diag_json("actual response", r);
    }
    if (r) json_decref(r);

    p = market_params("P2PAAB", "P2PAAA", "P2PBBB");
    r = http_call("market.create", p);
    json_decref(p);
    if (!is_ok(r)) {
        assertion(t, false, "P2P fixture market.create P2PAAB failed (HTTP %ld); valid precisions are fee=4 stock=4 money=4", last_http_status);
        diag_json("actual response", r);
    } else {
        snprintf(c->asset, sizeof(c->asset), "P2PAAA");
        snprintf(c->stock, sizeof(c->stock), "P2PAAA");
        snprintf(c->money, sizeof(c->money), "P2PBBB");
        snprintf(c->market, sizeof(c->market), "P2PAAB");
    }
    if (r) json_decref(r);

    return c->market[0] && c->stock[0];
}

static void test_p2p_http_asset_list(p2p_ctx *c)
{
    test_state t;
    begin_test(&t, "existing HTTP asset.list remains callable");
    if (c->http_assets) { json_decref(c->http_assets); c->http_assets = NULL; }
    c->http_assets = http_list("asset.list");
    assert_http_success(&t, c->http_assets, "asset.list");
    assert_result_array(&t, c->http_assets, "asset.list");
    end_test(&t);
}

static void test_p2p_rpc_asset_list(p2p_ctx *c)
{
    test_state t;
    begin_test(&t, "direct RPC asset.list remains compatible with HTTP asset.list");
    json_t *a = A0();
    json_t *r = rpc_call(CMD_ASSET_LIST, a);
    json_decref(a);
    assert_rpc_success(&t, r, "RPC asset.list");
    assert_result_array(&t, r, "RPC asset.list");
    assertion(&t, c->http_assets != NULL, "HTTP asset.list baseline response unavailable");
    if (c->http_assets && r) {
        assertion(&t, same_name_set(result(c->http_assets), result(r)),
                  "HTTP and direct RPC asset.list returned different asset name sets");
        if (!same_name_set(result(c->http_assets), result(r))) {
            diag_json("HTTP result", result(c->http_assets));
            diag_json("RPC result", result(r));
        }
    }
    if (r) json_decref(r);
    end_test(&t);
}

static void test_p2p_http_market_list(p2p_ctx *c)
{
    test_state t;
    begin_test(&t, "existing HTTP market.list remains callable");
    if (c->http_markets) { json_decref(c->http_markets); c->http_markets = NULL; }
    c->http_markets = http_list("market.list");
    assert_http_success(&t, c->http_markets, "market.list");
    assert_result_array(&t, c->http_markets, "market.list");
    end_test(&t);
}

static void test_p2p_rpc_market_list(p2p_ctx *c)
{
    test_state t;
    begin_test(&t, "direct RPC market.list remains compatible with HTTP market.list");
    json_t *a = A0();
    json_t *r = rpc_call(CMD_MARKET_LIST, a);
    json_decref(a);
    assert_rpc_success(&t, r, "RPC market.list");
    assert_result_array(&t, r, "RPC market.list");
    assertion(&t, c->http_markets != NULL, "HTTP market.list baseline response unavailable");
    if (c->http_markets && r) {
        assertion(&t, same_name_set(result(c->http_markets), result(r)),
                  "HTTP and direct RPC market.list returned different market name sets");
        if (!same_name_set(result(c->http_markets), result(r))) {
            diag_json("HTTP result", result(c->http_markets));
            diag_json("RPC result", result(r));
        }
    }
    if (r) json_decref(r);
    end_test(&t);
}

static void test_p2p_asset_summary(p2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "existing HTTP asset.summary remains callable");
    json_t *r = http_list("asset.summary");
    assert_http_success(&t, r, "asset.summary");
    assert_result_array(&t, r, "asset.summary");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_p2p_market_summary(p2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "existing HTTP market.summary remains callable");
    json_t *r = http_list("market.summary");
    assert_http_success(&t, r, "market.summary");
    assert_result_array(&t, r, "market.summary");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_p2p_balance_update(p2p_ctx *c)
{
    test_state t;
    begin_test(&t, "existing direct RPC balance.update remains operational");
    bool fixture = ensure_p2p_fixture(c, &t);
    assertion(&t, fixture, "could not discover or bootstrap a valid market fixture");
    json_t *r = fixture ? balance_update(c->user_id, c->stock, "3", (uint64_t)++req_id) : NULL;
    if (fixture)
        assert_rpc_success(&t, r, "balance.update stock asset +3");
    if (r) json_decref(r);
    end_test(&t);
}

static void test_p2p_balance_query(p2p_ctx *c)
{
    test_state t;
    begin_test(&t, "existing direct RPC balance.query returns updated balance");
    assertion(&t, c->stock[0] != '\0', "stock asset unavailable because fixture setup did not succeed");
    json_t *r = c->stock[0] ? balance_query(c->user_id, c->stock) : NULL;
    if (c->stock[0])
        assert_rpc_success(&t, r, "balance.query funded stock asset");
    const char *available = r ? bal_field(r, c->stock, "available") : NULL;
    assertion(&t, available != NULL, "balance.query result missing %s.available", c->stock[0] ? c->stock : "<stock>");
    if (available)
        assertion(&t, strcmp(available, "0") != 0, "expected funded %s.available to be nonzero, got %s", c->stock, available);
    if (!t.ok) diag_json("actual response", r);
    if (r) json_decref(r);
    end_test(&t);
}

static void test_p2p_order_put_limit(p2p_ctx *c)
{
    test_state t;
    begin_test(&t, "existing direct RPC order.put_limit still places a limit order");
    assertion(&t, c->market[0] != '\0', "market fixture unavailable");
    json_t *r = c->market[0] ? put_limit(false, c->user_id, c->market, 1, "1", "1") : NULL;
    if (c->market[0])
        assert_rpc_success(&t, r, "RPC order.put_limit regression check");
    c->order_id = order_id(r);
    assertion(&t, c->order_id != 0, "order.put_limit did not return a nonzero order id");
    if (!t.ok) diag_json("actual response", r);
    if (r) json_decref(r);
    end_test(&t);
}

static void test_p2p_order_cancel(p2p_ctx *c)
{
    test_state t;
    begin_test(&t, "existing direct RPC order.cancel still cancels an open order");
    assertion(&t, c->order_id != 0, "no open order id available from preceding order.put_limit test");
    json_t *r = c->order_id ? cancel_order(c->user_id, c->market, c->order_id) : NULL;
    if (c->order_id)
        assert_rpc_success(&t, r, "RPC order.cancel regression check");
    if (!t.ok) diag_json("actual response", r);
    if (r) json_decref(r);
    end_test(&t);
}

static void test_p2p_order_depth(p2p_ctx *c)
{
    test_state t;
    begin_test(&t, "existing HTTP order.depth remains callable");
    assertion(&t, c->market[0] != '\0', "market fixture unavailable for order.depth");
    json_t *r = NULL;
    if (c->market[0]) {
        json_t *a = json_array();
        json_array_append_new(a, json_string(c->market));
        json_array_append_new(a, json_integer(10));
        json_array_append_new(a, json_string("0"));
        r = http_call("order.depth", a);
        json_decref(a);
        assert_http_success(&t, r, "order.depth");
    }
    if (!t.ok) diag_json("actual response", r);
    if (r) json_decref(r);
    end_test(&t);
}

static void test_p2p_cli_status(p2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "existing matchengine CLI status command remains operational");
    char *out = cli_call("status");
    assertion(&t, out && *out, "CLI status returned no output");
    if (!t.ok && out) fprintf(stderr, "    actual CLI output: %s\n", out);
    free(out);
    end_test(&t);
}

static void test_p2p_cli_balance_summary(p2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "existing matchengine CLI balance summary remains operational");
    char *out = cli_call("balance summary");
    assertion(&t, out && strcasestr(out, "asset"), "CLI balance summary missing expected 'asset' header; actual=%s", out ? out : "<null>");
    free(out);
    end_test(&t);
}

static void test_p2p_cli_market_summary(p2p_ctx *c)
{
    (void)c;
    test_state t;
    begin_test(&t, "existing matchengine CLI market summary remains operational");
    char *out = cli_call("market summary");
    assertion(&t, out && strcasestr(out, "market"), "CLI market summary missing expected 'market' header; actual=%s", out ? out : "<null>");
    free(out);
    end_test(&t);
}

struct p2p_case {
    const char *name;
    void (*fn)(p2p_ctx *);
};

static const struct p2p_case P2P_CASES[] = {
    { "existing HTTP asset.list remains callable", test_p2p_http_asset_list },
    { "direct RPC asset.list remains compatible with HTTP asset.list", test_p2p_rpc_asset_list },
    { "existing HTTP market.list remains callable", test_p2p_http_market_list },
    { "direct RPC market.list remains compatible with HTTP market.list", test_p2p_rpc_market_list },
    { "existing HTTP asset.summary remains callable", test_p2p_asset_summary },
    { "existing HTTP market.summary remains callable", test_p2p_market_summary },
    { "existing direct RPC balance.update remains operational", test_p2p_balance_update },
    { "existing direct RPC balance.query returns updated balance", test_p2p_balance_query },
    { "existing direct RPC order.put_limit still places a limit order", test_p2p_order_put_limit },
    { "existing direct RPC order.cancel still cancels an open order", test_p2p_order_cancel },
    { "existing HTTP order.depth remains callable", test_p2p_order_depth },
    { "existing matchengine CLI status command remains operational", test_p2p_cli_status },
    { "existing matchengine CLI balance summary remains operational", test_p2p_cli_balance_summary },
    { "existing matchengine CLI market summary remains operational", test_p2p_cli_market_summary },
};

static void p2p(void)
{
    p2p_ctx c = {.user_id = 920001};
    for (size_t i = 0; i < sizeof(P2P_CASES) / sizeof(P2P_CASES[0]); i++)
        P2P_CASES[i].fn(&c);
    if (c.http_assets) json_decref(c.http_assets);
    if (c.http_markets) json_decref(c.http_markets);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    HTTP_URL = getenv("GOLDEN_HTTP_URL");
    if (!HTTP_URL) HTTP_URL = "http://127.0.0.1:8080";
    ME_HOST = getenv("GOLDEN_HOST");
    if (!ME_HOST) ME_HOST = "127.0.0.1";
    ME_PORT = getenv("GOLDEN_ME_PORT") ? atoi(getenv("GOLDEN_ME_PORT")) : 7316;
    MP_PORT = getenv("GOLDEN_MP_PORT") ? atoi(getenv("GOLDEN_MP_PORT")) : 7416;
    CLI_PORT = getenv("GOLDEN_CLI_PORT") ? atoi(getenv("GOLDEN_CLI_PORT")) : 7317;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (argc != 2) {
        fprintf(stderr, "usage: %s f2p|p2p\n", argv[0]);
        return 2;
    }

    if (!strcmp(argv[1], "f2p"))
        f2p();
    else if (!strcmp(argv[1], "p2p"))
        p2p();
    else
        return 2;

    printf("\nRESULT: %d passed, %d failed, %d total\n", passed, failed, passed + failed);
    curl_global_cleanup();
    return failed ? 1 : 0;
}
