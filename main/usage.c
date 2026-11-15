#include "usage.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

static const char *TAG = "usage";

#define NAME_LEN 56

typedef struct {
    char name[NAME_LEN];
    uint64_t up;
    uint64_t down;
} topic_use_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static topic_use_t s_topics[USAGE_MAX_TOPICS];
static int s_n;
static uint64_t s_total_up;
static uint64_t s_total_down;
static char s_prefix[48];
static size_t s_prefix_len;
static struct netif *s_ppp;
static netif_output_fn s_orig_output;
static bool s_full_logged;

static void add_total(bool up, uint32_t bytes)
{
    portENTER_CRITICAL(&s_mux);
    if (up) {
        s_total_up += bytes;
    } else {
        s_total_down += bytes;
    }
    portEXIT_CRITICAL(&s_mux);
}

/* Up: wraps the PPP netif's output function. */
static err_t out_hook(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
    add_total(true, p->tot_len);
    return s_orig_output(netif, p, ipaddr);
}

/* Down: lwIP's PPP code calls ip4_input() directly (not netif->input), so the
 * link step wraps it (-Wl,--wrap=ip4_input in main/CMakeLists.txt). */
err_t __real_ip4_input(struct pbuf *p, struct netif *inp);

err_t __wrap_ip4_input(struct pbuf *p, struct netif *inp)
{
    if (inp != NULL && inp == s_ppp) {
        add_total(false, p->tot_len);
    }
    return __real_ip4_input(p, inp);
}

void usage_attach_netif(esp_netif_t *ppp)
{
    struct netif *n = esp_netif_get_netif_impl(ppp);
    if (!n) {
        return;
    }
    if (n->output != out_hook) {
        s_orig_output = n->output;
        n->output = out_hook;
    }
    s_ppp = n;
}

uint64_t usage_total_up(void)
{
    portENTER_CRITICAL(&s_mux);
    uint64_t v = s_total_up;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

uint64_t usage_total_down(void)
{
    portENTER_CRITICAL(&s_mux);
    uint64_t v = s_total_down;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

void usage_set_prefix(const char *prefix)
{
    snprintf(s_prefix, sizeof(s_prefix), "%s", prefix);
    s_prefix_len = strlen(s_prefix);
}

void usage_mqtt_count(bool up, const char *topic, size_t topic_len, size_t payload_len, int qos)
{
    size_t remaining = 2 + topic_len + (qos > 0 ? 2 : 0) + payload_len;
    size_t header = 1 + (remaining < 128 ? 1 : remaining < 16384 ? 2 : remaining < 2097152 ? 3 : 4);
    uint64_t bytes = header + remaining;

    if (s_prefix_len && topic_len > s_prefix_len && memcmp(topic, s_prefix, s_prefix_len) == 0) {
        topic += s_prefix_len;
        topic_len -= s_prefix_len;
    }
    if (topic_len >= NAME_LEN) {
        topic_len = NAME_LEN - 1;
    }

    portENTER_CRITICAL(&s_mux);
    int i;
    for (i = 0; i < s_n; i++) {
        if (strlen(s_topics[i].name) == topic_len && memcmp(s_topics[i].name, topic, topic_len) == 0) {
            break;
        }
    }
    bool full = false;
    if (i == s_n) {
        if (s_n < USAGE_MAX_TOPICS) {
            memcpy(s_topics[i].name, topic, topic_len);
            s_topics[i].name[topic_len] = '\0';
            s_n++;
        } else {
            full = true;
        }
    }
    if (!full) {
        if (up) {
            s_topics[i].up += bytes;
        } else {
            s_topics[i].down += bytes;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (full && !s_full_logged) {
        s_full_logged = true;
        ESP_LOGW(TAG, "more than %d topics, the rest is not counted per topic", USAGE_MAX_TOPICS);
    }
}

int usage_topic_count(void)
{
    return s_n;
}

const char *usage_topic_name(int idx)
{
    return (idx >= 0 && idx < s_n) ? s_topics[idx].name : "";
}

uint64_t usage_topic_up(int idx)
{
    portENTER_CRITICAL(&s_mux);
    uint64_t v = (idx >= 0 && idx < s_n) ? s_topics[idx].up : 0;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

uint64_t usage_topic_down(int idx)
{
    portENTER_CRITICAL(&s_mux);
    uint64_t v = (idx >= 0 && idx < s_n) ? s_topics[idx].down : 0;
    portEXIT_CRITICAL(&s_mux);
    return v;
}
