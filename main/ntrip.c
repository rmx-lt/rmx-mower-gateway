#include "ntrip.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "gw_link.h"
#include "gw_tasks.h"
#include "gwstate.h"
#include "metrics.h"
#include "ntrip_codec.h"
#include "remote_cmd.h"
#include "settings.h"
#include "sdkconfig.h"

static const char *TAG = "ntrip";

#define NS "ntrip"
#define FIELD_LEN 64
#define REQ_MAX 640              /* request line, headers, Base64 of user:password, GGA */
#define HDR_TIMEOUT_MS 15000     /* connect to the caster's answer */
#define STALL_MS 30000           /* no bytes from the caster for this long: reconnect */
#define RETRY_MS 10000
#define RETRY_MAX_MS 120000

typedef struct {
    char host[FIELD_LEN + 1];
    int port;
    char mount[FIELD_LEN + 1];
    char user[FIELD_LEN + 1];
    char pass[FIELD_LEN + 1];
} cfg_t;

static SemaphoreHandle_t s_lock;   /* s_cfg, s_have_cfg, s_gen, s_gga */
static cfg_t s_cfg;
static bool s_have_cfg;
static uint32_t s_gen;             /* bumped by ntrip_set / ntrip_clear: the session ends */
static ntrip_gga_t s_gga;
static ntrip_sink_fn s_sink;
static TaskHandle_t s_task;
static const char *s_state = "";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_up, s_down;

/* ntrip task only: one session at a time */
static char s_req[REQ_MAX];
static uint8_t s_buf[NTRIP_HDR_MAX];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) {
        *v++ = 0;
    }
}

static void count(bool up, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    *(up ? &s_up : &s_down) += n;
    portEXIT_CRITICAL(&s_mux);
}

static bool read_count(void *ctx, double *v)
{
    portENTER_CRITICAL(&s_mux);
    *v = (double)(ctx ? s_up : s_down);
    portEXIT_CRITICAL(&s_mux);
    return true;
}

static const metric_def_t s_defs[2] = {
    { .name = "usage.ntrip.up", .label = "Correction data sent",
        .description = "Bytes the NTRIP client sent to the caster (request and positions) since the gateway started, without TCP/IP headers.",
        .unit = "B", .part = "usage", .counter = true, .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 },
    { .name = "usage.ntrip.down", .label = "Correction data received",
        .description = "Bytes the NTRIP client received from the caster (answer and corrections) since the gateway started, without TCP/IP headers.",
        .unit = "B", .part = "usage", .counter = true, .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 },
};

static void set_state(const char *st)
{
    if (strcmp(s_state, st) == 0) {
        return;
    }
    s_state = st;
    ESP_LOGI(TAG, "state %s", st);
    gw_state_set("ntrip", st);
    gw_link_set(GW_NTRIP_STREAM, strcmp(st, "streaming") == 0);
}

static bool gen_changed(uint32_t gen)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_gen != gen;
    xSemaphoreGive(s_lock);
    return changed;
}

/* The GGA sentence due now (with CR LF) into out, or false. */
static bool gga_due(char *out, bool reconnected)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (reconnected) {
        ntrip_gga_reconnected(&s_gga);
    }
    const char *g = ntrip_gga_due(&s_gga, now_ms());
    if (g) {
        snprintf(out, NTRIP_GGA_MAX + 3, "%s", g);
    }
    xSemaphoreGive(s_lock);
    return g != NULL;
}

static bool send_all(int s, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len) {
        int k = send(s, p, len, 0);
        if (k <= 0) {
            return false;
        }
        count(true, k);
        p += k;
        len -= k;
    }
    return true;
}

static int open_socket(const cfg_t *c)
{
    char port[8];
    snprintf(port, sizeof(port), "%d", c->port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    if (getaddrinfo(c->host, port, &hints, &ai) != 0 || !ai) {
        ESP_LOGW(TAG, "%s: name not resolved", c->host);
        return -1;
    }
    int s = socket(ai->ai_family, ai->ai_socktype, 0);
    if (s >= 0) {
        /* non-blocking connect with a 10 s limit, then blocking I/O with timeouts */
        fcntl(s, F_SETFL, O_NONBLOCK);
        int r = connect(s, ai->ai_addr, ai->ai_addrlen);
        if (r != 0 && errno == EINPROGRESS) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(s, &w);
            struct timeval tv = { .tv_sec = 10 };
            int err = 0;
            socklen_t el = sizeof(err);
            r = (select(s + 1, NULL, &w, NULL, &tv) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 &&
                 err == 0) ? 0 : -1;
        }
        if (r != 0) {
            ESP_LOGW(TAG, "%s:%d: connect failed", c->host, c->port);
            close(s);
            s = -1;
        } else {
            fcntl(s, F_SETFL, 0);
            struct timeval rcv = { .tv_sec = 1 }, snd = { .tv_sec = 10 };
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
        }
    }
    freeaddrinfo(ai);
    return s;
}

/* Corrections (chunked when rev 2 asked for it) to the sink. false: malformed. */
static bool deliver(ntrip_chunk_t *dec, bool chunked, uint8_t *data, size_t len)
{
    if (chunked) {
        int n = ntrip_chunk_decode(dec, data, len, data);
        if (n < 0) {
            return false;
        }
        len = n;
    }
    ntrip_sink_fn sink = s_sink;
    if (len && sink) {
        sink(data, len);
    }
    return true;
}

/* One connection to the caster, until it fails or the settings change.
 * *fatal: the caster refused the settings (wrong login or mount point). */
static bool session(const cfg_t *c, uint32_t gen, bool *fatal)
{
    *fatal = false;
    int s = open_socket(c);
    if (s < 0) {
        return false;
    }
    bool streamed = false;
    char gga[NTRIP_GGA_MAX + 3] = "";
    gga_due(gga, true);
    ntrip_cfg_t nc = { c->host, c->port, c->mount, c->user, c->pass };
    int n = ntrip_build_request(s_req, sizeof(s_req), &nc, NTRIP_V1, gga[0] ? gga : NULL);
    if (n < 0) {
        *fatal = true;
        goto out;
    }
    if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
        static char masked[REQ_MAX];
        if (ntrip_request_masked(s_req, n, masked, sizeof(masked)) >= 0) {
            ESP_LOGD(TAG, "request:\n%s", masked);
        }
    }
    bool sent = send_all(s, s_req, n);
    wipe(s_req, sizeof(s_req));
    if (!sent) {
        goto out;
    }

    /* the caster's answer */
    size_t have = 0, hdr = 0;
    bool chunked = false;
    int64_t start = now_ms();
    for (;;) {
        gw_task_alive(GW_T_NTRIP);
        if (gen_changed(gen)) {
            goto out;
        }
        int k = recv(s, s_buf + have, sizeof(s_buf) - have, 0);
        if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            ESP_LOGW(TAG, "caster closed before answering");
            goto out;
        }
        if (k < 0) {
            if (now_ms() - start > HDR_TIMEOUT_MS) {
                ESP_LOGW(TAG, "no answer from the caster");
                goto out;
            }
            continue;
        }
        count(false, k);
        have += k;
        ntrip_resp_t r = ntrip_parse_response((const char *)s_buf, have, &hdr, &chunked);
        if (r == NTRIP_RESP_MORE && have < sizeof(s_buf)) {
            continue;
        }
        if (r != NTRIP_RESP_MORE) {
            int line = 0;
            while (line < (int)have && line < 64 && s_buf[line] >= 0x20) {
                line++;
            }
            ESP_LOGI(TAG, "caster answered \"%.*s\"", line, (const char *)s_buf);
        }
        if (r == NTRIP_RESP_STREAMING) {
            break;
        }
        *fatal = r == NTRIP_RESP_UNAUTHORIZED || r == NTRIP_RESP_NO_MOUNT;
        ESP_LOGW(TAG, "caster refused: %s", r == NTRIP_RESP_UNAUTHORIZED ? "unauthorized (user or password)"
                                            : r == NTRIP_RESP_NO_MOUNT ? "no such mount point" : "bad answer");
        goto out;
    }
    ESP_LOGI(TAG, "streaming from %s:%d/%s%s", c->host, c->port, c->mount, chunked ? " (chunked)" : "");
    set_state("streaming");
    streamed = true;
    ntrip_chunk_t dec = { 0 };
    if (!deliver(&dec, chunked, s_buf + hdr, have - hdr)) {
        goto out;
    }

    /* corrections in, a position out every 10 s */
    int64_t last_rx = now_ms();
    for (;;) {
        gw_task_alive(GW_T_NTRIP);
        if (gen_changed(gen)) {
            goto out;
        }
        if (gga_due(gga, false) && !send_all(s, gga, strlen(gga))) {
            ESP_LOGW(TAG, "sending the position failed");
            goto out;
        }
        int k = recv(s, s_buf, sizeof(s_buf), 0);
        if (k > 0) {
            count(false, k);
            last_rx = now_ms();
            if (!deliver(&dec, chunked, s_buf, k)) {
                ESP_LOGW(TAG, "bad chunked stream");
                goto out;
            }
        } else if (k == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            ESP_LOGW(TAG, "connection lost");
            goto out;
        } else if (now_ms() - last_rx > STALL_MS) {
            ESP_LOGW(TAG, "no corrections for %d s", STALL_MS / 1000);
            goto out;
        }
    }
out:
    close(s);
    return streamed;
}

static void ntrip_task(void *arg)
{
    int64_t retry_at = 0;
    int backoff = RETRY_MS;
    cfg_t c;
    for (;;) {
        gw_task_alive(GW_T_NTRIP);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool have_cfg = s_have_cfg, have_gga = s_gga.have;
        uint32_t gen = s_gen;
        c = s_cfg;
        xSemaphoreGive(s_lock);
        if (!have_cfg || !have_gga) {
            set_state(have_cfg ? "waiting" : "off");
            retry_at = 0;
            backoff = RETRY_MS;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }
        if (!(gw_link_get() & GW_CELL_UP) || now_ms() < retry_at) {
            if (strcmp(s_state, "error") != 0) {
                set_state("connecting");
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            if (gen_changed(gen)) {
                retry_at = 0;   /* new settings: try them at once */
            }
            continue;
        }
        if (strcmp(s_state, "error") != 0) {
            set_state("connecting");
        }
        bool fatal;
        bool streamed = session(&c, gen, &fatal);
        wipe(&c, sizeof(c));
        if (gen_changed(gen)) {
            retry_at = 0;
            backoff = RETRY_MS;
            continue;
        }
        if (streamed) {
            backoff = RETRY_MS;
        }
        set_state(fatal ? "error" : "connecting");
        retry_at = now_ms() + (fatal ? RETRY_MAX_MS : backoff);
        backoff = backoff * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : backoff * 2;
    }
}

static void wake(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static const char *str_arg(const cJSON *cmd, const char *name)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(cmd, name));
}

/* ntrip_set (§6.4), registered with CMD_SECRET_ARGS: the password goes to NVS and
 * the request only. */
static void cmd_ntrip_set(const char *id, const cJSON *cmd, cmd_reply_t *r)
{
    const char *host = str_arg(cmd, "host"), *mount = str_arg(cmd, "mountpoint");
    const char *user = str_arg(cmd, "user"), *pass = str_arg(cmd, "password");
    const cJSON *port = cJSON_GetObjectItem(cmd, "port");
    double p = cJSON_IsNumber(port) ? cJSON_GetNumberValue(port) : 0;
    r->error = CMD_E_BAD_REQUEST;
    if (!host || !mount || !user || !pass || p != (int)p) {
        r->message = "ntrip_set needs host, port, mountpoint, user and password";
        return;
    }
    if (strlen(host) > FIELD_LEN || strlen(mount) > FIELD_LEN || strlen(user) > FIELD_LEN ||
            strlen(pass) > FIELD_LEN) {
        r->message = "a setting is longer than 64 characters";
        return;
    }
    cfg_t c = { .port = (int)p };
    strcpy(c.host, host);
    strcpy(c.mount, mount);
    strcpy(c.user, user);
    strcpy(c.pass, pass);
    ntrip_cfg_t nc = { c.host, c.port, c.mount, c.user, c.pass };
    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%d", c.port);
    const settings_kv_t kv[] = { { "host", c.host }, { "port", port_s }, { "mount", c.mount }, { "user", c.user },
                                 { "pass", c.pass } };
    char req[REQ_MAX];   /* s_req belongs to the ntrip task */
    bool valid = ntrip_build_request(req, sizeof(req), &nc, NTRIP_V1, NULL) >= 0;
    wipe(req, sizeof(req));
    if (!valid) {
        r->message = "a setting has a character the request cannot carry, or the port is out of range";
    } else if (settings_set_many(NS, kv, 5) != ESP_OK) {
        r->error = CMD_E_FAILED;
        r->message = "could not store the settings";
    } else {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg = c;
        s_have_cfg = true;
        s_gen++;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "new settings: %s:%d/%s", c.host, c.port, c.mount);
        r->error = NULL;
        wake();
    }
    wipe(&c, sizeof(c));
}

static void cmd_ntrip_clear(const char *id, const cJSON *cmd, cmd_reply_t *r)
{
    const settings_kv_t kv[] = { { "host", NULL }, { "port", NULL }, { "mount", NULL }, { "user", NULL },
                                 { "pass", NULL } };
    if (settings_set_many(NS, kv, 5) != ESP_OK) {
        r->error = CMD_E_FAILED;
        r->message = "could not remove the settings";
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wipe(&s_cfg, sizeof(s_cfg));
    s_have_cfg = false;
    s_gen++;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "settings removed");
    wake();
}

static void load(void)
{
    char port[8];
    s_have_cfg = settings_get_str(NS, "host", s_cfg.host, sizeof(s_cfg.host)) == ESP_OK &&
                 settings_get_str(NS, "port", port, sizeof(port)) == ESP_OK &&
                 settings_get_str(NS, "mount", s_cfg.mount, sizeof(s_cfg.mount)) == ESP_OK &&
                 settings_get_str(NS, "user", s_cfg.user, sizeof(s_cfg.user)) == ESP_OK &&
                 settings_get_str(NS, "pass", s_cfg.pass, sizeof(s_cfg.pass)) == ESP_OK;
    if (s_have_cfg) {
        s_cfg.port = atoi(port);
        ESP_LOGI(TAG, "settings: %s:%d/%s", s_cfg.host, s_cfg.port, s_cfg.mount);
    } else {
        wipe(&s_cfg, sizeof(s_cfg));
    }
}

void ntrip_feed_gga(const char *nmea, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool had = s_gga.have;
    ntrip_gga_offer(&s_gga, nmea, len);
    bool first = !had && s_gga.have;
    xSemaphoreGive(s_lock);
    if (first) {
        wake();
    }
}

void ntrip_set_sink(ntrip_sink_fn fn)
{
    s_sink = fn;
}

void ntrip_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    load();
    cmd_register("ntrip_set", cmd_ntrip_set, CMD_SECRET_ARGS);
    cmd_register("ntrip_clear", cmd_ntrip_clear, 0);
    metrics_register(&s_defs[0], read_count, (void *)1);
    metrics_register(&s_defs[1], read_count, NULL);
#if CONFIG_GATEWAY_TEST_NTRIP_GGA
    /* Test only: a fixed position (Vilnius Cathedral Square, a public place) instead of the autopilot's. */
    static const char test_gga[] = "$GPGGA,120000.00,5441.1500,N,02517.2500,E,1,12,0.8,112.0,M,28.0,M,,*6A";
    ESP_LOGW(TAG, "TEST: fixed GGA instead of the autopilot's");
    ntrip_feed_gga(test_gga, sizeof(test_gga) - 1);
#endif
    gw_task_create(GW_T_NTRIP, ntrip_task, NULL, &s_task);
}
