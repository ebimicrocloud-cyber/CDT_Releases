/* main.c (merged) - ESP-IDF v5.3 (ESP32-C6)
 * Merged UART handling from your working UART1 example into your full app.
 * Changes: sets UART pins, increases UART RX buffer size, robust line handling,
 * and logs received lines. "AP" sent from STM32 will now trigger AP mode.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_mac.h"

#include "mqtt_client.h"

#include "esp_ota_ops.h"
//#include "esp_http_client.h"  //for HTTP 
#include "esp_https_ota.h"

#define OTA_URL "https://raw.githubusercontent.com/ebimicrocloud-cyber/CDT_Releases/main/CDT.bin"


#define FW_Ver "3.0"

#define TAG "EBIMICRO"

#define TB_PROVISION_URL "https://eu.thingsboard.cloud/api/v1/provision"

#define TB_PROVISION_KEY     "qzccpgn1i3yuw085amli"
#define TB_PROVISION_SECRET "80uw58umgptbk8u94xnf"
static TaskHandle_t provision_task_handle = NULL;
static bool provisioning_attempted = false;

extern const uint8_t _binary_thingsboard_root_ca_pem_start[];
extern const uint8_t _binary_thingsboard_root_ca_pem_end[];

static char prov_resp[256];
static int prov_resp_len = 0;

static char g_user_email[96] = {0};

// ================ USER CONFIG ================
#define LED_PIN             GPIO_NUM_10
#define BUTTON_PIN          GPIO_NUM_7   // active-low to enter AP
#define UART_NUM            UART_NUM_1
#define UART_BUF_SIZE       1024  // increased from 256
#define WIFI_CHECK_INTERVAL_MS 10000
#define MAX_WIFI_RETRIES    5

// pins for UART1 (change if needed)
#define UART_TX_PIN GPIO_NUM_4
#define UART_RX_PIN GPIO_NUM_5

// AP parameters (AP will always require a password >= 8 chars)
#define AP_SSID "EBIMICRO-CDT_A"
//Dynamic pass
static char g_ap_password[13] = {0}; // 12 chars + NULL
static esp_timer_handle_t ap_timeout_timer = NULL;

// ThingsBoard (HTTP using token saved in NVS)
static char g_server_url[192] = {0}; // constructed from token

// ================ GLOBALS ====================
static char g_ssid[33] = {0};
static char g_wifi_pass[65] = {0};
static char g_token[65] = {0};

static char g_device_name[32];

static bool g_ap_mode = false;
static bool g_sta_mode = true;
static bool g_sta_connected = false;

static int wifi_retry_count = 0;
static httpd_handle_t g_httpd = NULL;
static esp_event_handler_instance_t wifi_event_instance;
static esp_event_handler_instance_t ip_event_instance;

static SemaphoreHandle_t ap_request_sem = NULL;
static SemaphoreHandle_t payload_mutex = NULL;

extern const uint8_t ebimicro_logo_png_start[] asm("_binary_ebimicro_logo_png_start");
extern const uint8_t ebimicro_logo_png_end[]   asm("_binary_ebimicro_logo_png_end");

// forward declarations
static void start_webserver(void);
static void stop_webserver(void);
static void start_ap_mode(void);
static void connect_wifi_sta(void);
static void ap_manager_task(void *arg);
static void button_isr_handler(void* arg);
static void uart_task(void *arg);
static void wifi_check_task(void *arg);
static void led_task(void *arg);

/* ===================== MQTT ===================== */

typedef struct {
    char broker[128];
    int  port;
    char username[64];
    char password[64];
    char topic[128];
    bool enabled;
    bool tls;
} mqtt_cfg_t;

static mqtt_cfg_t g_mqtt = {0};
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

static char mqtt_base_topic[160];
char topic[192];

/* ===================== MQTT HANDLER ===================== */

static void mqtt_event_handler(void *arg,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *data)
{
    

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_connected = true;

        // Build base topic ONCE
        snprintf(mqtt_base_topic,
                sizeof(mqtt_base_topic),
                "%s/%s",
                g_mqtt.topic,     // example: cdta
                g_device_name);  // example: CDT-A-1051DB0C6FC0

        ESP_LOGI(TAG, "MQTT CONNECTED ✔");
        ESP_LOGI(TAG, "Base topic: %s", mqtt_base_topic);

        break;

    case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        //esp_mqtt_client_reconnect(mqtt_client);
        break;

    default:
        break;
    }
}

/* ===================== MQTT START ===================== */

static void mqtt_start(void)
{
    if (!g_mqtt.enabled) {
        ESP_LOGW(TAG, "MQTT is DISABLED");
        return;
    }

    if (g_mqtt.broker[0] == '\0') {
        ESP_LOGE(TAG, "MQTT broker is empty!");
        return;
    }

    if (g_mqtt.topic[0] == '\0') {
        ESP_LOGE(TAG, "MQTT topic is empty!");
        return;
    }

    char uri[160];
    snprintf(uri, sizeof(uri),
             "%s://%s:%d",
             g_mqtt.tls ? "mqtts" : "mqtt",
             g_mqtt.broker,
             g_mqtt.port ? g_mqtt.port : 1883);

    ESP_LOGI(TAG, "========== MQTT CONFIG ==========");
    ESP_LOGI(TAG, "Broker : %s", uri);
    ESP_LOGI(TAG, "Topic  : %s", g_mqtt.topic);
    ESP_LOGI(TAG, "User   : %s", g_mqtt.username);
    ESP_LOGI(TAG, "TLS    : %s", g_mqtt.tls ? "YES" : "NO");
    ESP_LOGI(TAG, "=================================");

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = g_mqtt.username,
        .credentials.authentication.password = g_mqtt.password,
        .session.keepalive = 60,
    };

    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(
        mqtt_client,
        ESP_EVENT_ANY_ID,
        mqtt_event_handler,
        NULL
    );

    ESP_LOGI(TAG, "Starting MQTT client...");
    esp_mqtt_client_start(mqtt_client);
}

/* ===================== MQTT PUBLISH ===================== */

static void mqtt_publish_field(const char *field, float value)
{
    if (!g_mqtt.enabled || !mqtt_connected)
        return;

    char topic[192];
    char payload[32];

    snprintf(topic, sizeof(topic),
             "%s/%s",
             mqtt_base_topic,
             field);

    snprintf(payload, sizeof(payload),
             "%.1f",
             value);

    int msg_id = esp_mqtt_client_publish(
        mqtt_client,
        topic,
        payload,
        0,
        1,
        0
    );

    // CLEAN LOG FORMAT
    ESP_LOGI(TAG,
        "MQTT >>> Topic: %-28s | Value: %-8s | MsgID:%d",
        topic,
        payload,
        msg_id);
}

/* ===================== NVS MQTT ===================== */

static void load_mqtt_config(void)
{
    nvs_handle_t h;
    size_t len = sizeof(g_mqtt);

    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "mqtt", &g_mqtt, &len) != ESP_OK) {
            memset(&g_mqtt, 0, sizeof(g_mqtt));
        }
        nvs_close(h);
    }
}

static void save_mqtt_config(void)
{
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "mqtt", &g_mqtt, sizeof(g_mqtt));
        nvs_commit(h);
        nvs_close(h);
    }
}

// ================ HTML TEMPLATE ==============
static const char *SETUP_HTML_TEMPLATE =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
"<style>"
":root{--bg:#0f141a;--card:#161d26;--accent:#0b74de;--text:#e8edf3;--muted:#9aa4b2;}"
"*{box-sizing:border-box}"
"body{margin:0;font-family:Inter,system-ui,Arial;background:var(--bg);"
"display:flex;align-items:center;justify-content:center;height:100vh;color:var(--text);}"
".card{background:var(--card);padding:24px;border-radius:14px;"
"box-shadow:0 10px 40px rgba(0,0,0,0.45);max-width:440px;width:94%;}"
".logo{display:flex;justify-content:center;margin-bottom:14px;}"
".logo img{width:100%;max-width:260px;}"
"h1{font-size:20px;margin:6px 0 6px;text-align:center;}"
"p{font-size:13px;color:var(--muted);text-align:center;margin-bottom:14px;}"

/* tabs */
".tabs{display:flex;margin-bottom:14px;}"
".tabs label{flex:1;text-align:center;padding:8px;font-size:13px;"
"background:#0f141a;color:var(--muted);cursor:pointer;border-radius:6px;}"
".tabs label+label{margin-left:6px;}"
"input[type=radio]{display:none;}"
"#tab-basic:checked ~ .tabs label[for=tab-basic],"
"#tab-adv:checked ~ .tabs label[for=tab-adv]{"
"background:var(--accent);color:#fff;}"

".section{display:none;}"
"#tab-basic:checked ~ .basic,"
"#tab-adv:checked ~ .advanced{display:block;}"

"input{width:100%;padding:12px;margin-top:8px;border-radius:8px;"
"border:1px solid #263041;background:#0f141a;color:var(--text);}"
"input:focus{outline:none;border-color:var(--accent);}"

".chk{margin-top:12px;display:flex;align-items:center;gap:8px;font-size:12px;color:var(--muted);}"
"button{margin-top:18px;width:100%;padding:12px;border-radius:8px;"
"border:none;background:var(--accent);color:#fff;font-size:15px;font-weight:600;}"
"footer{margin-top:12px;font-size:11px;color:#7f8a99;text-align:center;}"
"</style></head>"

"<body><div class='card'>"
"<div class='logo'><img src='/logo.png'></div>"
"<h1>CDT-A Setup</h1>"
"<p>Configure network and register your device</p>"

"<form method='POST' action='/save'>"

/* radio tabs */
"<input type='radio' id='tab-basic' name='tab' checked>"
"<input type='radio' id='tab-adv' name='tab'>"
"<div class='tabs'>"
"<label for='tab-basic'>Basic</label>"
"<label for='tab-adv'>Advanced</label>"
"</div>"

/* BASIC */
"<div class='section basic'>"
"<input name='ssid' value=\"%s\" placeholder='Wi-Fi SSID *' required>"
"<input name='password' type='password' placeholder='Wi-Fi Password'>"
"<input name='email' type='email' value=\"%s\" placeholder='ThingsBoard email *' required>"
"</div>"

/* ADVANCED */
"<div class='section advanced'>"
"<input name='mqtt_broker' value=\"%s\" placeholder='MQTT broker'>"
"<input name='mqtt_port' value=\"%s\" placeholder='MQTT port'>"
"<input name='mqtt_user' placeholder='MQTT username'>"
"<input name='mqtt_pass' type='password' placeholder='MQTT password'>"
"<input name='mqtt_topic' value=\"%s\" placeholder='MQTT topic'>"
"<div class='chk'>"
"<input type='checkbox' name='mqtt_en' %s> Enable MQTT"
"</div>"
"</div>"

"<button type='submit'>Save & Restart</button>"
"</form>"
"<footer>EBIMICRO © 2026 · Secure AP</footer>"
"</div></body></html>";

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (evt->data_len + prov_resp_len < sizeof(prov_resp) - 1) {
            memcpy(prov_resp + prov_resp_len, evt->data, evt->data_len);
            prov_resp_len += evt->data_len;
            prov_resp[prov_resp_len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// ================ UTIL: URL DECODE ============
static int hex2int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void url_decode(const char *src, char *dst, size_t dst_len) {
    size_t i = 0, j = 0;
    while (src[i] && j + 1 < dst_len) {
        if (src[i] == '%') {
            if (isxdigit((unsigned char)src[i+1]) && isxdigit((unsigned char)src[i+2])) {
                int hi = hex2int(src[i+1]);
                int lo = hex2int(src[i+2]);
                dst[j++] = (char)((hi << 4) | lo);
                i += 3;
                continue;
            }
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
            continue;
        }
        dst[j++] = src[i++];
    }
    dst[j] = '\0';
}

static void parse_urlencoded(const char *body, char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len, char *out_token, size_t token_len, char *out_email, size_t email_len) {
    const char *p = body;
    char key[64];
    char val[256];

    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        size_t keyl = (size_t)(eq - p);
        if (keyl >= sizeof(key)) keyl = sizeof(key)-1;
        memcpy(key, p, keyl); key[keyl] = '\0';

        const char *amp = strchr(eq + 1, '&');
        size_t vall = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
        if (vall >= sizeof(val)) vall = sizeof(val)-1;
        memcpy(val, eq + 1, vall); val[vall] = '\0';

        char dec_val[256];
        url_decode(val, dec_val, sizeof(dec_val));

        if (strcmp(key, "ssid") == 0) {
            strncpy(out_ssid, dec_val, ssid_len - 1);
            out_ssid[ssid_len - 1] = '\0';
        } else if (strcmp(key, "password") == 0) {
            strncpy(out_pass, dec_val, pass_len - 1);
            out_pass[pass_len - 1] = '\0';
        } else if (strcmp(key, "email") == 0) {
                strncpy(out_email, dec_val, email_len - 1);
        }else if (strcmp(key, "mqtt_broker") == 0) {
                strncpy(g_mqtt.broker, dec_val, sizeof(g_mqtt.broker)-1);
        }
        else if (strcmp(key, "mqtt_port") == 0) {
            g_mqtt.port = atoi(dec_val);
        }
        else if (strcmp(key, "mqtt_user") == 0) {
            strncpy(g_mqtt.username, dec_val, sizeof(g_mqtt.username)-1);
        }
        else if (strcmp(key, "mqtt_pass") == 0) {
            strncpy(g_mqtt.password, dec_val, sizeof(g_mqtt.password)-1);
        }
        else if (strcmp(key, "mqtt_topic") == 0) {
            strncpy(g_mqtt.topic, dec_val, sizeof(g_mqtt.topic)-1);
        }
        /*else if (strcmp(key, "mqtt_en") == 0) {
            g_mqtt.enabled = true;
        }*/


        if (!amp) break;
        p = amp + 1;
    }
}

// ================ OTA ================
void ota_update_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting HTTPS OTA");

    esp_err_t ret;

        esp_https_ota_config_t ota_cfg = {
            .http_config = &(esp_http_client_config_t){
                .url = OTA_URL,
                .cert_pem = NULL,
                .transport_type = HTTP_TRANSPORT_OVER_SSL,

                // Required for HTTPS when cert_pem = NULL
                .skip_cert_common_name_check = true,
                .use_global_ca_store = false,
                .keep_alive_enable = true,
            }
        };


    ret = esp_https_ota(&ota_cfg);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Succeeded, rebooting...");
        //mqtt_publish_kv("set/update", "done");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Failed: %s", esp_err_to_name(ret));
        //mqtt_publish_kv("set/update", "failed");
    }

    vTaskDelete(NULL);
}

//Generate random AP password (CRA-safe)
static void generate_ap_password(char *out, size_t len)
{
    const char charset[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZ"
        "abcdefghijkmnpqrstuvwxyz"
        "23456789"; // avoid confusing chars

    uint32_t r;
    for (size_t i = 0; i < len - 1; i++) {
        esp_fill_random(&r, sizeof(r));
        out[i] = charset[r % (sizeof(charset) - 1)];
    }
    out[len - 1] = '\0';
}

// ================ NVS STORAGE ================
static void save_wifi_credentials(const char *nssid, const char *npass) {
    nvs_handle_t handle;
    esp_err_t r = nvs_open("storage", NVS_READWRITE, &handle);
    if (r == ESP_OK) {
        nvs_set_str(handle, "ssid", nssid);
        nvs_set_str(handle, "pass", npass);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Saved credentials to NVS");

        // Notify STM32 immediately
const char *okmsg = "WIFI OK       \n";//send two times to make sure it has been recieved correctly
uart_write_bytes(UART_NUM, okmsg, strlen(okmsg));
vTaskDelay(pdMS_TO_TICKS(100));
uart_write_bytes(UART_NUM, okmsg, strlen(okmsg));
ESP_LOGI(TAG, "Sent to STM32 (after saving credentials): %s", okmsg);

// Give it a moment to flush UART before restart
vTaskDelay(pdMS_TO_TICKS(100));

// Restart ESP
esp_restart();


    } else {
        ESP_LOGW(TAG, "Failed to open NVS to save credentials: %d", r);
    }
}



static void get_device_name(char *out, size_t len)
{
    uint8_t mac[6];

    // Read the Wi-Fi STA MAC address
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read MAC: %s", esp_err_to_name(ret));
        snprintf(out, len, "CDT-A-UNKNOWN");
        return;
    }

    snprintf(out, len,
        "CDT-A-%02X%02X%02X%02X%02X%02X",
        mac[0], mac[1], mac[2],
        mac[3], mac[4], mac[5]);
}

static void tb_send_email_attribute(void)
{
    if (g_token[0] == '\0' || g_user_email[0] == '\0')
        return;

    char url[192];
    snprintf(url, sizeof(url),
        "https://eu.thingsboard.cloud/api/v1/%s/attributes",
        g_token);

    char payload[160];
    snprintf(payload, sizeof(payload),
        "{\"customer_email\":\"%s\"}",
        g_user_email);

    esp_http_client_config_t cfg = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = (const char *)_binary_thingsboard_root_ca_pem_start,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, payload, strlen(payload));
    esp_http_client_perform(c);
    esp_http_client_cleanup(c);

    ESP_LOGI(TAG, "User email sent to ThingsBoard");
}


static bool thingsboard_provision_tls(void)
{
    char device_name[64];
    get_device_name(device_name, sizeof(device_name));

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{"
        "\"deviceName\":\"%s\","
        "\"provisionDeviceKey\":\"%s\","
        "\"provisionDeviceSecret\":\"%s\""
        "}",
        device_name,
        TB_PROVISION_KEY,
        TB_PROVISION_SECRET
    );

    
    
    prov_resp_len = 0;
    memset(prov_resp, 0, sizeof(prov_resp));

    esp_http_client_config_t cfg = {
        .url = TB_PROVISION_URL,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = (const char *)_binary_thingsboard_root_ca_pem_start,
        .timeout_ms = 7000,
        .event_handler = http_event_handler,   //REQUIRED
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    
    int status = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "Provision HTTP status=%d", status);
    ESP_LOGI(TAG, "Provision response: %s", prov_resp);

    esp_http_client_cleanup(client);
    
    // Extract token
    char *p = strstr(prov_resp, "\"credentialsValue\":\"");
    if (!p) {
        ESP_LOGE(TAG, "Token not found");
        return false;
    }
    p += strlen("\"credentialsValue\":\"");
    char *e = strchr(p, '"');
    if (!e) return false;

    size_t len = e - p;
    strncpy(g_token, p, len);
    g_token[len] = '\0';

    // Save token
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "token", g_token);
        nvs_set_u8(h, "prov_done", 1);   // Use as a falg to check provisioning status at booting but I aaume I don't need to use it.
        nvs_commit(h);
        nvs_close(h);
    }

    snprintf(g_server_url, sizeof(g_server_url),
        "http://eu.thingsboard.cloud/api/v1/%s/telemetry",
        g_token);

    ESP_LOGI(TAG, "Secure provisioning OK");
    ESP_LOGW(TAG, "DEVICE PROVISIONED! TOKEN ISSUED BY SERVER");

    tb_send_email_attribute();
    
    return true;
}

static void provisioning_task(void *arg)
{
    ESP_LOGI(TAG, "Provisioning task started");

    if (thingsboard_provision_tls()) {
        ESP_LOGI(TAG, "Provisioning successful");
    } else {
        ESP_LOGE(TAG, "Provisioning failed → fallback to AP");
        g_ap_mode = true;
        if (ap_request_sem) xSemaphoreGive(ap_request_sem);
    }

    provision_task_handle = NULL;
    vTaskDelete(NULL);
}


static void load_or_create_ap_password(void)
{
    nvs_handle_t h;
    size_t len = sizeof(g_ap_password);

    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_str(h, "ap_pass", g_ap_password, &len) != ESP_OK) {
            generate_ap_password(g_ap_password, sizeof(g_ap_password));
            nvs_set_str(h, "ap_pass", g_ap_password);
            nvs_commit(h);
            ESP_LOGI(TAG, "Generated new AP password");
        } else {
            ESP_LOGI(TAG, "Loaded existing AP password");
        }
        nvs_close(h);
    }
}

static void load_wifi_credentials(void) {
    nvs_handle_t handle;
    esp_err_t r = nvs_open("storage", NVS_READONLY, &handle);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed (%d)", r);
        g_ap_mode = true;
        return;
    }

    size_t len;

    // SSID
    len = sizeof(g_ssid);
    if (nvs_get_str(handle, "ssid", g_ssid, &len) != ESP_OK) {
        g_ap_mode = true;
    }

    // PASS
    len = sizeof(g_wifi_pass);
    nvs_get_str(handle, "pass", g_wifi_pass, &len);

    // TOKEN 
    len = sizeof(g_token);
    if (nvs_get_str(handle, "token", g_token, &len) == ESP_OK) {
        snprintf(g_server_url, sizeof(g_server_url),
            "http://eu.thingsboard.cloud/api/v1/%s/telemetry",
            g_token);
        ESP_LOGI(TAG, "Token restored from NVS");
    } else {
        g_token[0] = '\0';
        ESP_LOGI(TAG, "No token in NVS");
    }

    nvs_close(handle);
}

static void save_user_email(const char *email)
{
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "email", email);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_user_email(void)
{
    nvs_handle_t h;
    size_t len = sizeof(g_user_email);

    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, "email", g_user_email, &len);
        nvs_close(h);
    }
}

// ================ WIFI EVENT HANDLER ===========
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED");
        g_sta_connected = false;
        if (wifi_retry_count < MAX_WIFI_RETRIES) {
            wifi_retry_count++;
            ESP_LOGI(TAG, "Retrying connect (%d/%d)", wifi_retry_count, MAX_WIFI_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Max retries reached, requesting AP mode");
            g_ap_mode = true;
            g_sta_mode = false;
            if (ap_request_sem) xSemaphoreGive(ap_request_sem);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP");
        wifi_retry_count = 0;
        g_sta_connected = true;
        g_ap_mode = false;
        g_sta_mode = true;

            // 🔐 Provision ONLY after IP
        if (!provisioning_attempted && g_token[0] == '\0') {

            provisioning_attempted = true;
            ESP_LOGI(TAG, "No token found → starting secure provisioning");

            if (provision_task_handle == NULL) {
                    xTaskCreate(
                        provisioning_task,
                        "provisioning_task",
                        8192,          // IMPORTANT
                        NULL,
                        5,
                        &provision_task_handle
                    );

                     ESP_LOGI(TAG, "Provisioning task created");
                } 
        }

        if (g_mqtt.enabled && mqtt_client == NULL) {
                    mqtt_start();
                }
        }
}

// ================ HTTP HANDLERS =================

static esp_err_t logo_get_handler(httpd_req_t *req)
{
    const size_t logo_size =
        ebimicro_logo_png_end - ebimicro_logo_png_start;

    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    httpd_resp_send(req,
        (const char *)ebimicro_logo_png_start,
        logo_size);

    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d",
             g_mqtt.port ? g_mqtt.port : 1883);

    const char *mqtt_checked = g_mqtt.enabled ? "checked" : "";

    char *page = malloc(4096);   // ✅ heap, not stack and better to be the same amount as Component Config->HTTP Server->HTTP Request Header Length limit
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    snprintf(page, 4096,
        SETUP_HTML_TEMPLATE,
        g_ssid,
        g_user_email,
        g_mqtt.broker,
        port_str,
        g_mqtt.topic,
        mqtt_checked
    );

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);

    free(page);   // ✅ important
    return ESP_OK;
}


static esp_err_t save_post_handler(httpd_req_t *req) {
    
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }
    char *body = malloc(total_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, body + received, total_len - received);
        if (ret <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[total_len] = '\0';

    bool mqtt_en_received = false;
    /* Checkbox presence = enabled */
    if (strstr(body, "mqtt_en=") != NULL) {
    mqtt_en_received = true;
    }

    
    char new_ssid[64] = {0}, new_pass[128] = {0};
    char new_email[96] = {0};
    
    /* Backup old MQTT password */
    char old_mqtt_pass[64];
    strncpy(old_mqtt_pass, g_mqtt.password, sizeof(old_mqtt_pass) - 1);
    old_mqtt_pass[sizeof(old_mqtt_pass) - 1] = '\0';

    /* Parse POST body */
    parse_urlencoded(body,
                    new_ssid, sizeof(new_ssid),
                    new_pass, sizeof(new_pass),
                    NULL, 0,                // token (unused)
                    new_email, sizeof(new_email));
    free(body);

    /* MQTT enable checkbox logic */
    g_mqtt.enabled = false;
    if (mqtt_en_received) {
        g_mqtt.enabled = true;
    }

    /* Restore MQTT password if field was left empty */
    if (strlen(g_mqtt.password) == 0) {
        strncpy(g_mqtt.password, old_mqtt_pass, sizeof(g_mqtt.password) - 1);
    }

    /* Save config */
    save_user_email(new_email);
    save_mqtt_config();


    if (new_ssid[0] != '\0') {
        
        save_wifi_credentials(new_ssid, new_pass);
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Saved! Rebooting to connect...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;
    } else {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Error: SSID and Token required");
        return ESP_OK;
    }
}

static void start_webserver(void)
{
    if (g_httpd) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    if (httpd_start(&g_httpd, &config) == ESP_OK) {

        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler
        };

        httpd_uri_t save = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = save_post_handler
        };

        httpd_uri_t logo = {
            .uri = "/logo.png",   
            .method = HTTP_GET,
            .handler = logo_get_handler
        };

        httpd_register_uri_handler(g_httpd, &root);
        httpd_register_uri_handler(g_httpd, &save);
        httpd_register_uri_handler(g_httpd, &logo);

        ESP_LOGI(TAG, "HTTP server started");
    }
}


static void stop_webserver(void) {
    if (g_httpd) {
        httpd_stop(g_httpd);
        g_httpd = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

// ================ AP MODE ======================
static void start_ap_mode(void) {
      esp_wifi_stop();

    wifi_config_t ap_config = {0};

    strncpy((char*)ap_config.ap.ssid, AP_SSID, sizeof(ap_config.ap.ssid)-1);
    strncpy((char*)ap_config.ap.password, g_ap_password, sizeof(ap_config.ap.password)-1);

    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.max_connection = 4;
    ap_config.ap.channel = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started SSID='%s' PASS='%s'", AP_SSID, g_ap_password);

    if (ap_timeout_timer) {
    esp_timer_stop(ap_timeout_timer);
    esp_timer_start_once(ap_timeout_timer, 10 * 60 * 1000000ULL);
}

    start_webserver();
}

// ================ STA CONNECT ==================
static void connect_wifi_sta(void) {
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    strncpy((char*)wifi_config.sta.ssid, g_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, g_wifi_pass, sizeof(wifi_config.sta.password) - 1);

    stop_webserver();

    ESP_LOGI(TAG, "Switching to STA mode, SSID='%s'", g_ssid);

    // Always stop before reconfiguring
    esp_wifi_disconnect();   // ignore error if not connected
    esp_wifi_stop();         // ignore error if already stopped

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Attempting to connect...");
    }
}


// ================ TELEMETRY SENDER ===============
static void send_telemetry(const char *payload) {
    if (g_server_url[0] == '\0') {
        ESP_LOGW(TAG, "Server URL empty, telemetry not sent");
        return;
    }
    if (!g_sta_connected) {
        ESP_LOGW(TAG, "Not connected to WiFi, telemetry not sent");
        return;
    }

    esp_http_client_config_t config = {
        .url = g_server_url,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "Failed to init HTTP client");
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Telemetry posted, status=%d, payload=%s", status, payload);
    } else {
        ESP_LOGW(TAG, "Telemetry post failed: %d", err);
    }
    esp_http_client_cleanup(client);
}


static bool tb_get_co2_hysteresis(int *out_hyst)
{
    char url[256];
    snprintf(url, sizeof(url),
        "https://eu.thingsboard.cloud/api/v1/%s/attributes?sharedKeys=co2_hysteresis",
        g_token);

    prov_resp_len = 0;
    memset(prov_resp, 0, sizeof(prov_resp));

    esp_http_client_config_t cfg = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = (const char *)_binary_thingsboard_root_ca_pem_start,
        .timeout_ms = 5000,
        .event_handler = http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return false;

    // Example response:
    // {"shared":{"co2_hysteresis":50}}

    char *p = strstr(prov_resp, "\"co2_hysteresis\":");
    if (!p) return false;

    *out_hyst = atoi(p + strlen("\"co2_hysteresis\":"));
    return true;
}


static bool tb_get_co2_setpoint(int *out_setpoint)
{
    char url[256];
    snprintf(url, sizeof(url),
        "https://eu.thingsboard.cloud/api/v1/%s/attributes?sharedKeys=co2_setpoint",
        g_token);

    

    esp_http_client_config_t cfg = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = (const char *)_binary_thingsboard_root_ca_pem_start,
        .timeout_ms = 5000,
        .event_handler = http_event_handler,
    };

    prov_resp_len = 0;
    memset(prov_resp, 0, sizeof(prov_resp));

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return false;

    // Example response:
    // {"shared":{"co2_setpoint":1200}}

    char *p = strstr(prov_resp, "\"co2_setpoint\":");
    if (!p) return false;

    *out_setpoint = atoi(p + strlen("\"co2_setpoint\":"));
    return true;
}

static bool tb_get_co2_min(int *out_min)
{
    char url[256];
    snprintf(url, sizeof(url),
        "https://eu.thingsboard.cloud/api/v1/%s/attributes?sharedKeys=co2_min",
        g_token);

    prov_resp_len = 0;
    memset(prov_resp, 0, sizeof(prov_resp));

    esp_http_client_config_t cfg = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = (const char *)_binary_thingsboard_root_ca_pem_start,
        .timeout_ms = 5000,
        .event_handler = http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return false;

    char *p = strstr(prov_resp, "\"co2_min\":");
    if (!p) return false;

    *out_min = atoi(p + strlen("\"co2_min\":"));
    return true;
}

static bool tb_get_co2_max(int *out_max)
{
    char url[256];
    snprintf(url, sizeof(url),
        "https://eu.thingsboard.cloud/api/v1/%s/attributes?sharedKeys=co2_max",
        g_token);

    prov_resp_len = 0;
    memset(prov_resp, 0, sizeof(prov_resp));

    esp_http_client_config_t cfg = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = (const char *)_binary_thingsboard_root_ca_pem_start,
        .timeout_ms = 5000,
        .event_handler = http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return false;

    char *p = strstr(prov_resp, "\"co2_max\":");
    if (!p) return false;

    *out_max = atoi(p + strlen("\"co2_max\":"));
    return true;
}

static bool tb_get_ma420_mode(int *out_mode)
{
    char url[256];
    snprintf(url, sizeof(url),
        "https://eu.thingsboard.cloud/api/v1/%s/attributes?sharedKeys=mA_mode",
        g_token);

    prov_resp_len = 0;
    memset(prov_resp, 0, sizeof(prov_resp));

    esp_http_client_config_t cfg = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = (const char *)_binary_thingsboard_root_ca_pem_start,
        .timeout_ms = 5000,
        .event_handler = http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return false;

    char *p = strstr(prov_resp, "\"mA_mode\":");
    if (!p) return false;

    *out_mode = atoi(p + strlen("\"mA_mode\":"));
    return true;
}



static void send_co2_hysteresis_to_stm32(int hyst)
{
    char cmd[20];
    snprintf(cmd, sizeof(cmd), "CO2HYST=%d\r\n", hyst);
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    vTaskDelay(pdMS_TO_TICKS(100));
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    ESP_LOGI(TAG, "Sent to STM32: %s", cmd);
}


static void send_co2_setpoint_to_stm32(int sp)
{
    char cmd[20];
    snprintf(cmd, sizeof(cmd), "CO2SP=%d\r\n", sp);  //Never use '_' character! STM32 sees it as \r or \n for any reason.
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    vTaskDelay(pdMS_TO_TICKS(100));
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    ESP_LOGI(TAG, "Sent to STM32: %s", cmd);
}

static void send_co2_min_to_stm32(int min)
{
    char cmd[20];
    snprintf(cmd, sizeof(cmd), "CO2MIN=%d\r\n", min);
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    vTaskDelay(pdMS_TO_TICKS(100));
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    ESP_LOGI(TAG, "Sent to STM32: %s", cmd);
}


static void send_co2_max_to_stm32(int max)
{
    char cmd[20];
    snprintf(cmd, sizeof(cmd), "CO2MAX=%d\r\n", max);
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    vTaskDelay(pdMS_TO_TICKS(100));
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    ESP_LOGI(TAG, "Sent to STM32: %s", cmd);
}

static void send_ma420_to_stm32(int mode)
{
    char cmd[20];
    snprintf(cmd, sizeof(cmd), "MA420=%d\r\n", mode);
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    vTaskDelay(pdMS_TO_TICKS(100));
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    ESP_LOGI(TAG, "Sent to STM32: %s", cmd);
}


static void tb_control_task(void *arg)
{
    int last_sp   = -1;
    int last_hyst = -1;

    int last_min = -1;
    int last_max = -1;

    int last_ma420 = -1;

    while (1) {
        if (g_sta_connected && g_token[0]) {

            // -------- CO2 SETPOINT --------
            int sp;
            if (tb_get_co2_setpoint(&sp)) {
                if (sp != last_sp && sp >= 400 && sp <= 10000) {
                    send_co2_setpoint_to_stm32(sp);
                    last_sp = sp;
                }
            }

            // -------- CO2 HYSTERESIS --------
            int hyst;
            if (tb_get_co2_hysteresis(&hyst)) {
                if (hyst != last_hyst && hyst >= 10 && hyst <= 500) {
                    send_co2_hysteresis_to_stm32(hyst);
                    last_hyst = hyst;
                }
            }

            // -------- CO2 MIN --------------
            int min;
            if (tb_get_co2_min(&min)) {
                if (min != last_min && min >= 400 && min <= 10000) {
                    send_co2_min_to_stm32(min);
                    last_min = min;
                }
            }

            // -------- CO2 MAX ---------------
            int max;
            if (tb_get_co2_max(&max)) {
                if (max != last_max && max >= 400 && max <= 10000) {
                    send_co2_max_to_stm32(max);
                    last_max = max;
                }
            }

            // -------- 4-20 mA mode ----------

            int mode;
            if (tb_get_ma420_mode(&mode)) {
                if (mode != last_ma420 && (mode == 0 || mode == 1)) {
                    send_ma420_to_stm32(mode);
                    last_ma420 = mode;
                }
            }



        }

        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 sec
    }
}

// ================ SERIAL PARSER ==================
static void process_serial_input(const char *line) {
    if (!line) return;
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' ')) len--;
    if (len == 0) return;

    char work[1024];
    if (len >= sizeof(work)) len = sizeof(work)-1;
    memcpy(work, line, len);
    work[len] = '\0';

    // Allow remote control via serial too
    if (strcmp(work, "AP") == 0) {
        g_ap_mode = true; g_sta_mode = false;
        if (ap_request_sem) xSemaphoreGive(ap_request_sem);
        ESP_LOGI(TAG, "Serial command: go to AP mode");
        return;
    }
    if (strcmp(work, "STA") == 0) {
        g_ap_mode = false; g_sta_mode = true;
        connect_wifi_sta();
        ESP_LOGI(TAG, "Serial command: go to STA mode");
        return;
    }

    // existing parsing code (unchanged) - parse COxx payloads and send telemetry
    char payload[512];
    payload[0] = '{';
    payload[1] = '\0';

    char *p = work;
    while (1) {
        char *co = strstr(p, "CO");
        if (!co) break;
        char *num = co + 2;
        char sensor_s[4] = {0}; int si = 0;
        while (*num && isdigit((unsigned char)*num) && si < (int)sizeof(sensor_s)-1) {
            sensor_s[si++] = *num++;
        }
        if (si == 0) { p = num; continue; }
        int sensor = atoi(sensor_s);

        char *eq = strchr(num, '=');
        if (!eq) { p = num; continue; }
        char *co_start = eq + 1;
        char *co_end = strchr(co_start, ' ');
        if (!co_end) co_end = strchr(co_start, ';');
        if (!co_end) co_end = co_start + strlen(co_start);
        char co_buf[32]; size_t co_len = (size_t)(co_end - co_start); if (co_len >= sizeof(co_buf)) co_len = sizeof(co_buf)-1; memcpy(co_buf, co_start, co_len); co_buf[co_len] = '\0';
        float co_val = atof(co_buf);

        // temp
        char temp_key[16]; snprintf(temp_key, sizeof(temp_key), "Temp%d=", sensor);
        char *tpos = strstr(co, temp_key);
        float temp_val = 0.0f;
        if (tpos) {
            char *tstart = tpos + strlen(temp_key);
            char *tend = strchr(tstart, ' ');
            if (!tend) tend = strchr(tstart, ';');
            if (!tend) tend = tstart + strlen(tstart);
            char tbuf[32]; size_t tlen = (size_t)(tend - tstart); if (tlen >= sizeof(tbuf)) tlen = sizeof(tbuf)-1; memcpy(tbuf, tstart, tlen); tbuf[tlen] = '\0';
            temp_val = atof(tbuf) / 10.0f;
        }

        // hum
        char hum_key[16]; snprintf(hum_key, sizeof(hum_key), "Hum%d=", sensor);
        char *hpos = strstr(co, hum_key);
        float hum_val = 0.0f;
        if (hpos) {
            char *hstart = hpos + strlen(hum_key);
            char *hend = strchr(hstart, ';');
            if (!hend) hend = strchr(hstart, ' ');
            if (!hend) hend = hstart + strlen(hstart);
            char hbuf[32]; size_t hlen = (size_t)(hend - hstart); if (hlen >= sizeof(hbuf)) hlen = sizeof(hbuf)-1; memcpy(hbuf, hstart, hlen); hbuf[hlen] = '\0';
            hum_val = atof(hbuf);
        }

        // 
        
        //Relay_status
        char rel_key[16]; snprintf(rel_key, sizeof(rel_key), "Rel_St%d=", sensor);
        char *rpos = strstr(co, rel_key);
        float Rel_val = 0.0f;
        if (rpos) {
            char *rstart = rpos + strlen(rel_key);
            char *rend = strchr(rstart, ';');
            if (!rend) rend = strchr(rstart, ' ');
            if (!rend) rend = rstart + strlen(rstart);
            char rbuf[32]; size_t rlen = (size_t)(rend - rstart); if (rlen >= sizeof(rbuf)) rlen = sizeof(rbuf)-1; memcpy(rbuf, rstart, rlen); rbuf[rlen] = '\0';
            Rel_val = atof(rbuf);
        }

        // Analaog_output_val
        char Ana_out_key[16]; snprintf(Ana_out_key, sizeof(Ana_out_key), "Ana_out%d=", sensor);
        char *apos = strstr(co, Ana_out_key);
        float Ana_out_val = 0.0f;
        if (apos) {
            char *astart = apos + strlen(Ana_out_key);
            char *aend = strchr(astart, ';');
            if (!aend) aend = strchr(astart, ' ');
            if (!aend) aend = astart + strlen(astart);
            char abuf[32]; size_t alen = (size_t)(aend - astart); if (alen >= sizeof(abuf)) alen = sizeof(abuf)-1; memcpy(abuf, astart, alen); abuf[alen] = '\0';
            Ana_out_val = atof(abuf);
        }

        // CO2 MIN
        char min_key[16];
        snprintf(min_key, sizeof(min_key), "COMIN%d=", sensor);
        char *mpos = strstr(co, min_key);
        float min_val = 0.0f;

        if (mpos) {
            char *mstart = mpos + strlen(min_key);
            char *mend = strchr(mstart, ' ');
            if (!mend) mend = strchr(mstart, ';');
            if (!mend) mend = mstart + strlen(mstart);

            char mbuf[32];
            size_t mlen = (size_t)(mend - mstart);
            if (mlen >= sizeof(mbuf)) mlen = sizeof(mbuf) - 1;
            memcpy(mbuf, mstart, mlen);
            mbuf[mlen] = '\0';

            min_val = atof(mbuf);
        }

        // CO2 MAX
        char max_key[16];
        snprintf(max_key, sizeof(max_key), "COMAX%d=", sensor);
        char *xpos = strstr(co, max_key);
        float max_val = 0.0f;

        if (xpos) {
            char *xstart = xpos + strlen(max_key);
            char *xend = strchr(xstart, ' ');
            if (!xend) xend = strchr(xstart, ';');
            if (!xend) xend = xstart + strlen(xstart);

            char xbuf[32];
            size_t xlen = (size_t)(xend - xstart);
            if (xlen >= sizeof(xbuf)) xlen = sizeof(xbuf) - 1;
            memcpy(xbuf, xstart, xlen);
            xbuf[xlen] = '\0';

            max_val = atof(xbuf);
        }

        // 4-20mA enable mode (coming as mA_mode1=0/1)
        char ma_key[16];
        snprintf(ma_key, sizeof(ma_key), "mA_mode%d=", sensor);
        char *mapos = strstr(co, ma_key);
        float ma420_mode = 0.0f;

        if (mapos) {
            char *start = mapos + strlen(ma_key);
            char *end = strchr(start, ' ');
            if (!end) end = strchr(start, ';');
            if (!end) end = start + strlen(start);

            char buf[8];
            size_t l = end - start;
            if (l >= sizeof(buf)) l = sizeof(buf)-1;
            memcpy(buf, start, l);
            buf[l] = '\0';

            ma420_mode = atof(buf);
        }




        char part[128];
       snprintf(part, sizeof(part),
                "\"CO%d\":%.1f,"
                "\"Temp%d\":%.1f,"
                "\"Hum%d\":%.1f,"
                "\"Rel_St%d\":%.1f,"
                "\"Ana_out%d\":%.1f,"
                "\"COMIN%d\":%.0f,"
                "\"COMAX%d\":%.0f,"
                "\"mA_mode%d\":%.0f,",
                sensor, co_val,
                sensor, temp_val,
                sensor, hum_val,
                sensor, Rel_val,
                sensor, Ana_out_val,
                sensor, min_val,
                sensor, max_val,
                sensor, ma420_mode);

        strncat(payload, part, sizeof(payload)-strlen(payload)-1);

        char field[32];

        snprintf(field, sizeof(field), "CO%d", sensor);
        mqtt_publish_field(field, co_val);

        snprintf(field, sizeof(field), "Temp%d", sensor);
        mqtt_publish_field(field, temp_val);

        snprintf(field, sizeof(field), "Hum%d", sensor);
        mqtt_publish_field(field, hum_val);

        snprintf(field, sizeof(field), "Relay%d", sensor);
        mqtt_publish_field(field, Rel_val);

        snprintf(field, sizeof(field), "Analog%d", sensor);
        mqtt_publish_field(field, Ana_out_val);

        snprintf(field, sizeof(field), "COMIN%d", sensor);
        mqtt_publish_field(field, min_val);

        snprintf(field, sizeof(field), "COMAX%d", sensor);
        mqtt_publish_field(field, max_val);

        snprintf(field, sizeof(field), "mA_mode%d", sensor);
        mqtt_publish_field(field, ma420_mode);



        p = co_end;
    }

    size_t plen = strlen(payload);
    if (plen > 1) {
        if (payload[plen-1] == ',') payload[plen-1] = '\0';
        strncat(payload, "}", sizeof(payload)-strlen(payload)-1);

        // blink LED quickly to show send activity
        gpio_set_level(LED_PIN, 0);
        send_telemetry(payload);        // ThingsBoard HTTP
        //send_telemetry_mqtt(payload);  // MQTT (optional)
        gpio_set_level(LED_PIN, 1);
    }
}

// ================ UART TASK ====================
static void uart_task(void *arg) {
    (void)arg;
    const int rx_buf_size = UART_BUF_SIZE;
    uint8_t* data = (uint8_t*) malloc(rx_buf_size);
    char lineBuf[1024]; size_t lineLen = 0;

    uart_config_t uart_config = {
        .baud_rate = 19200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT, // ESP32-C6
    };
    uart_param_config(UART_NUM, &uart_config);
    // install driver with larger RX buffer and no TX buffer
    uart_driver_install(UART_NUM, 2048, 0, 0, NULL, 0);
    // set pins for UART1 (TX, RX)
    uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "UART%d initialized on TX=%d RX=%d", UART_NUM, UART_TX_PIN, UART_RX_PIN);

    while (true) {
        int len = uart_read_bytes(UART_NUM, data, 1, 50 / portTICK_PERIOD_MS);
        if (len > 0) {
            char c = (char)data[0];
            // optional: log received byte (comment out for high-throughput)
            // ESP_LOGI(TAG, "uart: got char '%c' (0x%02X)", isprint((unsigned char)c)?c:'.', (unsigned char)c);

            if (c == '\n') {
                if (lineLen > 0) {
                    lineBuf[lineLen] = '\0';
                    ESP_LOGI(TAG, "Serial line: %s", lineBuf);
                    process_serial_input(lineBuf);
                    lineLen = 0;
                }
            } else if (c == '\r') {
                // ignore
            } else {
                if (lineLen < sizeof(lineBuf)-1) lineBuf[lineLen++] = c;
                else { lineLen = 0; }
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    free(data);
}

// ================ AP MANAGER TASK =================
static void ap_manager_task(void *arg) {
    (void)arg;
    while (1) {
        if (ap_request_sem && xSemaphoreTake(ap_request_sem, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "AP requested (by button or wifi failure/serial)");
            provisioning_attempted = false;
            start_ap_mode();
        }
    }
}

// ================ BUTTON ISR & SETUP =============
static void IRAM_ATTR button_isr_handler(void* arg) {
    static uint32_t last = 0;
    uint32_t now = xTaskGetTickCountFromISR();
    if (now - last < pdMS_TO_TICKS(250)) return; // debounce 250ms
    last = now;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (ap_request_sem) xSemaphoreGiveFromISR(ap_request_sem, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

static void button_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE, // detect falling edge (active low)
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_PIN, button_isr_handler, NULL);
}

// ================ WIFI CHECK TASK =============
static void wifi_check_task(void *arg) {
    (void)arg;
    while (true) {
        if (!g_ap_mode) {
            wifi_mode_t mode;
            if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_STA && !g_sta_connected) {
                ESP_LOGI(TAG, "Not connected - trying to connect...");
                connect_wifi_sta();
            }
        }
        vTaskDelay(WIFI_CHECK_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}

// ================ LED TASK =====================
static void led_task(void *arg) {
    (void)arg;
    while (1) {
        if (g_ap_mode) {
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else if (g_sta_mode && g_sta_connected) {
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(900));
        } else if (g_sta_mode && !g_sta_connected) {
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(400));
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

static void ap_timeout_cb(void *arg)
{
    ESP_LOGI(TAG, "AP timeout expired, stopping AP");
    stop_webserver();
    esp_wifi_stop();
    g_ap_mode = false;
    g_sta_mode = true;
    connect_wifi_sta();
}

static void init_ap_timer(void)
{
    esp_timer_create_args_t t = {
        .callback = ap_timeout_cb,
        .name = "ap_timeout"
    };
    esp_timer_create(&t, &ap_timeout_timer);
}





// ================ APP MAIN ====================
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 1);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &wifi_event_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &ip_event_instance));

    ap_request_sem = xSemaphoreCreateBinary();
    payload_mutex = xSemaphoreCreateMutex();

    button_init();
    init_ap_timer();
    load_or_create_ap_password();

    load_user_email();

    load_mqtt_config();

    ESP_LOGI(TAG, "MQTT enabled = %d", g_mqtt.enabled);

    load_wifi_credentials();

    get_device_name(g_device_name, sizeof(g_device_name));


    if (!g_ap_mode) {
        ESP_LOGI(TAG, "Starting WiFi in STA mode");
        connect_wifi_sta();
    } else {
        ESP_LOGI(TAG, "Starting in AP mode (no credentials)");
        start_ap_mode();
        esp_timer_start_once(ap_timeout_timer, 10 * 60 * 1000000ULL); // 10 min
    }

    xTaskCreate(ap_manager_task, "ap_manager", 4096, NULL, 6, NULL);
    xTaskCreate(uart_task, "uart_task", 8192, NULL, 5, NULL);
    xTaskCreate(wifi_check_task, "wifi_check", 4096, NULL, 5, NULL);
    xTaskCreate(led_task, "led_task", 2048, NULL, 2, NULL);
    xTaskCreate(tb_control_task, "tb_control", 4096, NULL, 5, NULL);

    

        // =================== OTA UPDATE ===================
    /*
    else if (strstr(topic, "set/update")) {
        ESP_LOGW(TAG, "OTA update requested via MQTT payload='%s'", p);

        if (strcasecmp(p, "1") == 0 ||
            strcasecmp(p, "start") == 0 ||
            strcasecmp(p, "update") == 0) 
        {
            mqtt_publish_kv("set/update", "starting");

            // Create OTA task
            xTaskCreate(ota_update_task, "ota_update_task", 8192, NULL, 5, NULL);
        }
        else {
            ESP_LOGW(TAG, "Unknown OTA payload: '%s'", p);
        }
    }
    */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
