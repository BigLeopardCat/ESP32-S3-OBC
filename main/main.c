/* ==========================================================
 * OBC OLED IoT 显示固件 (ESP32-S3)
 *
 * 功能：
 *   - 连接 WiFi (STA 模式)
 *   - 通过 MQTT over TLS (mqtts://) 连接 IoT 平台
 *   - 订阅指定 topic，收到消息后在 128x64 SPI OLED 上显示
 *
 * 硬件接线（与旧固件一致）：
 *   CS=36  DC=37  RST=38  MOSI=39  CLK=40
 *
 * 需要修改的配置都在下方「配置区」，改完直接编译即可。
 * 旧固件（网页配置/OTA/BLE）已备份在 main.c.bak。
 * ========================================================== */
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "u8g2.h"
#include "esp_rom_sys.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_timer.h"
#include <stdlib.h>

/* 服务器证书链，由 EMBED_TXTFILES 嵌入固件（见 main/CMakeLists.txt） */
extern const uint8_t mqtt_ca_pem_start[] asm("_binary_saudade_site_ca_pem_start");

/* ===================== 配置区（修改这里） ===================== */
/* 你的 WiFi */
#define WIFI_SSID       "别找我喵"
#define WIFI_PASS       "meowmeow"

/* IoT 平台接入：
 *   1. 浏览器打开 https://saudade.site/device-console/（需登录博客）
 *   2. 注册设备，把返回的 device_id / device_key 填到下面（key 仅显示一次） */
#define MQTT_URI        "mqtts://saudade.site:8883"
#define DEVICE_ID       "dev-52ff0c8906344627bdf01a480bfd3952"   /* ← 控制台注册返回的 device_id */
#define DEVICE_KEY      "dk-caf399a4633442a5a28ccdc733413cd9"    /* ← 控制台注册返回的 device_key */
#define MQTT_USER       DEVICE_ID                /* 设备认证用户名 = device_id */
#define MQTT_PASS       DEVICE_KEY               /* 设备认证密码 = device_key */
#define MQTT_KEEPALIVE  60

/* 固件版本（OTA 比对用）：每次发版手动 +1，如 1.1.0 */
#define APP_VERSION     "1.2.0"

/* OTA：平台固件仓库（需与设备服务版本一致），上传新固件到控制台后自动升级 */
#define OTA_INFO_URL    "https://saudade.site/device-api/api/ota/info"
#define OTA_FW_URL      "https://saudade.site/device-api/api/ota/fw/current.bin"  /* current 可被平台回滚切换 */
#define OTA_CHECK_MS    (6 * 60 * 60 * 1000)     /* 每 6 小时检查一次新固件 */

/* 遥测上报周期 */
#define TELEMETRY_MS    5000
/* ============================================================= */

#define TAG "OLED_IOT"

/* OLED SPI 引脚 */
#define OLED_PIN_CS    36
#define OLED_PIN_DC    37
#define OLED_PIN_RST   38
#define OLED_PIN_MOSI  39
#define OLED_PIN_CLK   40
#define OLED_H_RES     128
#define OLED_V_RES     64

/* u8g2 显示（文泉驿12px 全量 GB2312 字库，6763 汉字）：CJK 字宽 12px，ASCII 半宽 6px */
#define FONT_LINE_H     13                  /* 行高：12px 字高 + 1px 间距 */
#define MAX_TEXT_LINES  4                   /* 最多显示行数 */
#define LINE_MAX_W      (OLED_H_RES - 2)    /* 单行最大像素宽度 */

static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_disp_mutex;
static volatile bool s_wifi_connected = false;
static esp_mqtt_client_handle_t s_mqtt = NULL;

/* ---- 设备参数（网页控制台下发的 config，NVS 持久化） ---- */
static int32_t  g_cfg_version  = 0;                 /* 当前生效配置版本 */
static uint8_t  g_brightness   = 0xFF;              /* OLED 对比度 0-255 */
static char     g_default_text[64] = "Hello OBC";   /* 空闲显示内容 */

/* ---------------- SSD1306 底层 (SPI) ---------------- */
static void oled_cmd(uint8_t cmd)
{
    gpio_set_level((gpio_num_t)OLED_PIN_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(s_spi, &t);
}

static void oled_data(const uint8_t *data, int len)
{
    if (len <= 0) return;
    gpio_set_level((gpio_num_t)OLED_PIN_DC, 1);
    spi_transaction_t t = { .length = (size_t)len * 8, .tx_buffer = data };
    spi_device_polling_transmit(s_spi, &t);
}

static u8g2_t s_u8g2;

/* u8g2 字节回调：命令/数据转发到自有 SPI 驱动 */
static uint8_t u8x8_byte_obc(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    static uint8_t dc = 0;
    switch (msg) {
    case U8X8_MSG_BYTE_SET_DC:
        dc = arg_int;
        break;
    case U8X8_MSG_BYTE_SEND:   /* 注：== U8X8_MSG_CAD_SEND_DATA（u8x8.h 中的别名） */
        if (dc) {
            oled_data(arg_ptr, arg_int);
        } else {
            for (int i = 0; i < arg_int; i++) {
                oled_cmd(((const uint8_t *)arg_ptr)[i]);
            }
        }
        break;
    default:
        break;
    }
    return 1;
}

/* u8g2 GPIO/延时回调：只处理复位和延时（CS/DC/SCLK/MOSI 由 SPI 驱动和字节回调处理） */
static uint8_t u8x8_gpio_obc(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    switch (msg) {
    case U8X8_MSG_GPIO_RESET:
        gpio_set_level((gpio_num_t)OLED_PIN_RST, arg_int);
        break;
    case U8X8_MSG_DELAY_100NANO:
        esp_rom_delay_us(1);
        break;
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        break;
    default:
        break;
    }
    return 1;
}

/* ---------------- 文字绘制（u8g2 + 文泉驿12px 中文字体） ---------------- */

/* UTF-8 字符像素宽度：ASCII 6px，多字节（CJK）12px */
static int utf8_char_width(const char *c)
{
    return ((uint8_t)*c < 0x80) ? 6 : 12;
}

/* 居中画一行 UTF-8 文本 */
static void draw_utf8_centered(const char *text, int y, int ascent)
{
    int w = 0;
    for (const char *p = text; *p; ) {
        int len = ((uint8_t)*p < 0x80) ? 1 : (((uint8_t)*p < 0xE0) ? 2 : (((uint8_t)*p < 0xF0) ? 3 : 4));
        w += utf8_char_width(p);
        p += len;
    }
    /* 20260831：clamp 口径与行截断一致用 LINE_MAX_W（128→126），超宽行不再按 128 居中导致左贴边 */
    if (w > LINE_MAX_W) w = LINE_MAX_W;
    int x = (OLED_H_RES - w) / 2;
    if (x < 0) x = 0;
    u8g2_DrawUTF8(&s_u8g2, x, y, text);
}

/* 通用文本显示：UTF-8 消息自动换行（不切汉字），每行居中 */
static void oled_show_text(const char *utf8_text)
{
    if (s_disp_mutex) xSemaphoreTake(s_disp_mutex, portMAX_DELAY);
    u8g2_ClearBuffer(&s_u8g2);
    /* 全量 GB2312（6763 汉字 + ASCII），按 Unicode 索引，DrawUTF8 直接可用 */
    u8g2_SetFont(&s_u8g2, u8g2_font_wqy12_t_gb2312);

    int ascent = u8g2_GetFontAscent(&s_u8g2);
    if (ascent <= 0 || ascent > FONT_LINE_H) ascent = FONT_LINE_H - 1;
    int y = (OLED_V_RES - FONT_LINE_H * MAX_TEXT_LINES) / 2 + ascent;  /* 垂直居中 */

    const char *p = utf8_text;
    int line = 0;
    while (*p && line < MAX_TEXT_LINES) {
        /* 截取一行：宽度 ≤ LINE_MAX_W，遇 \n 换行。
           20260831 修复"先加后查"：旧逻辑把超宽字符先加入本行再退出循环，整行
           超宽（如 11 个全角 = 132px > 126）居中时 x 被 clamp 到 0，最右侧字符
           被屏幕右缘裁切（"中午吃碗热汤面，加个蛋"的"蛋"右 4px 被切掉）；
           现改为"先查后加"，超宽字符留给下一行。旧的回退逻辑（字节中间截断）
           是死代码——end 始终按完整字符长度前进，不会落在字节中间，已删除。 */
        const char *end = p;
        int w = 0;
        while (*end && *end != '\n') {
            int len = ((uint8_t)*end < 0x80) ? 1 : (((uint8_t)*end < 0xE0) ? 2 : (((uint8_t)*end < 0xF0) ? 3 : 4));
            if (w + utf8_char_width(end) > LINE_MAX_W) break;
            w += utf8_char_width(end);
            end += len;
        }
        char buf[32];
        int n = (int)(end - p);
        if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
        memcpy(buf, p, n);
        buf[n] = 0;
        draw_utf8_centered(buf, y, ascent);
        y += FONT_LINE_H;
        p = end;
        if (*p == '\n') p++;
        line++;
    }
    u8g2_SendBuffer(&s_u8g2);
    if (s_disp_mutex) xSemaphoreGive(s_disp_mutex);
}

/* 兼容旧接口：状态/消息都走统一渲染 */
static void oled_show_lines(const char *lines) { oled_show_text(lines); }
static void oled_show_message(const char *msg)  { oled_show_text(msg); }

/* ---------------- 配置持久化（NVS） ---------------- */
static void config_save(void)
{
    nvs_handle_t h;
    if (nvs_open("obc_cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "version", g_cfg_version);
    nvs_set_u8(h, "bright", g_brightness);
    nvs_set_str(h, "dtext", g_default_text);
    nvs_commit(h);
    nvs_close(h);
}

static void config_load(void)
{
    nvs_handle_t h;
    if (nvs_open("obc_cfg", NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_i32(h, "version", &g_cfg_version);
    nvs_get_u8(h, "bright", &g_brightness);
    size_t len = sizeof(g_default_text);
    if (nvs_get_str(h, "dtext", g_default_text, &len) != ESP_OK) {
        strcpy(g_default_text, "Hello OBC");
    }
    nvs_close(h);
}

/* 应用配置（网页下发）：brightness=OLED 对比度；default_text=空闲显示文本 */
static void apply_config(const cJSON *cfg)
{
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(cfg, "brightness");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(cfg, "default_text");
    bool changed = false;
    if (cJSON_IsNumber(b)) {
        uint8_t v = (uint8_t)(b->valueint & 0xFF);
        if (v != g_brightness) { g_brightness = v; changed = true; }
    }
    if (cJSON_IsString(t) && t->valuestring[0]) {
        if (strcmp(g_default_text, t->valuestring) != 0) {
            snprintf(g_default_text, sizeof(g_default_text), "%s", t->valuestring);
            changed = true;
        }
    }
    /* 应用亮度（SSD1306 对比度指令） */
    if (s_disp_mutex) xSemaphoreTake(s_disp_mutex, portMAX_DELAY);
    oled_cmd(0x81);
    oled_cmd(g_brightness);
    if (s_disp_mutex) xSemaphoreGive(s_disp_mutex);
    if (changed) config_save();
    ESP_LOGI(TAG, "配置应用: brightness=%d default_text=%s",
             g_brightness, g_default_text);
}

/* ---------------- 遥测 ---------------- */
static void publish_telemetry(void)
{
    /* 模拟温湿度（OBC 演示用）；接真实传感器时替换 */
    static float temp = 25.0f;
    temp += ((rand() % 100) - 50) / 100.0f;

    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    char topic[96], payload[256];
    snprintf(topic, sizeof(topic), "devices/%s/telemetry", DEVICE_ID);
    snprintf(payload, sizeof(payload),
             "{\"cfg_version\":%d,\"firmware\":\"%s\",\"temperature\":%.1f,"
             "\"rssi\":%d,\"uptime\":%u,\"brightness\":%d}",
             (int)g_cfg_version, APP_VERSION, temp, rssi,
             (unsigned)(esp_timer_get_time() / 1000000), g_brightness);
    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
}

static void telemetry_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_MS));
        if (s_mqtt && s_wifi_connected) publish_telemetry();
    }
}

/* ---------------- OTA（平台固件仓库轮询升级/回滚） ---------------- */
/* 拉取平台固件版本信息（HTTP Basic 设备认证） */
static char *ota_fetch_version(void)
{
    esp_http_client_config_t cc = {
        .url = OTA_INFO_URL,
        .username = DEVICE_ID,
        .password = DEVICE_KEY,
        .timeout_ms = 8000,
        .cert_pem = (const char *)mqtt_ca_pem_start,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cc);
    if (!c) return NULL;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(c); return NULL; }
    int len = esp_http_client_fetch_headers(c);
    char *buf = NULL;
    if (len > 0 && len < 512) {
        buf = malloc(len + 1);
        if (buf) {
            int got = esp_http_client_read(c, buf, len);
            if (got < 0) { free(buf); buf = NULL; }
            else { buf[got] = 0; }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return buf;
}

static void ota_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(15000));   /* 启动 15s 后首次检查 */
    for (;;) {
        if (s_wifi_connected) {
            char *info = ota_fetch_version();
            if (info) {
                cJSON *j = cJSON_Parse(info);
                free(info);
                if (j) {
                    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "version");
                    if (cJSON_IsString(v) && v->valuestring[0]) {
                        /* 服务器 current 与本地版本不一致即刷（支持升级与回滚降级）；
                           版本一致则跳过，避免重复刷写 */
                        if (strcmp(v->valuestring, APP_VERSION) != 0) {
                            ESP_LOGI(TAG, "平台固件 %s（本地 %s），开始升级/降级…",
                                     v->valuestring, APP_VERSION);
                            /* 带 Basic 认证下载固件（esp_https_ota 校验服务器证书） */
                            esp_http_client_config_t cc = {
                                .url = OTA_FW_URL,
                                .username = DEVICE_ID,
                                .password = DEVICE_KEY,
                                .timeout_ms = 60000,
                                .keep_alive_enable = true,
                                .cert_pem = (const char *)mqtt_ca_pem_start,
                            };
                            esp_https_ota_config_t oc = { .http_config = &cc };
                            if (esp_https_ota(&oc) == ESP_OK) {
                                oled_show_lines("OTA OK\nRestarting...");
                                vTaskDelay(pdMS_TO_TICKS(1000));
                                esp_restart();
                            } else {
                                ESP_LOGE(TAG, "OTA 下载/校验失败");
                                oled_show_lines("OTA Failed\nRetry later");
                            }
                        }
                    }
                    cJSON_Delete(j);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_MS));
    }
}

/* ---------------- MQTT ---------------- */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data);

/* 打印 JSON 里的每个字段（变量名 = 数值），用于调试服务器下发的新参数 */
static void log_json_fields(const cJSON *obj)
{
    for (const cJSON *it = obj->child; it; it = it->next) {
        switch (it->type & 0xFF) {
        case cJSON_String:  ESP_LOGI(TAG, "  \"%s\" = \"%s\"", it->string, it->valuestring); break;
        case cJSON_Number:  ESP_LOGI(TAG, "  \"%s\" = %g", it->string, it->valuedouble); break;
        case cJSON_True:    ESP_LOGI(TAG, "  \"%s\" = true", it->string); break;
        case cJSON_False:   ESP_LOGI(TAG, "  \"%s\" = false", it->string); break;
        case cJSON_NULL:    ESP_LOGI(TAG, "  \"%s\" = null", it->string); break;
        case cJSON_Array:   ESP_LOGI(TAG, "  \"%s\" = [array x%d]", it->string, cJSON_GetArraySize(it)); break;
        case cJSON_Object:  ESP_LOGI(TAG, "  \"%s\" = {object}", it->string); break;
        default:            ESP_LOGI(TAG, "  \"%s\" = ?", it->string); break;
        }
    }
}

static void mqtt_start(void)
{
    if (s_mqtt) return;
    char will_topic[96];
    snprintf(will_topic, sizeof(will_topic), "devices/%s/status", DEVICE_ID);
    esp_mqtt_client_config_t cfg = {
        .broker = {
            .address = {
                .uri = MQTT_URI,
            },
            .verification = {
                .certificate = (const char *)mqtt_ca_pem_start,  /* 校验证书链 */
            },
        },
        .credentials = {
            .username = MQTT_USER,
            .authentication = {
                .password = MQTT_PASS,
            },
        },
        .session = {
            .keepalive = MQTT_KEEPALIVE,
            .last_will = {
                .topic = will_topic,        /* 异常掉线时自动上报 offline */
                .msg = "offline",
                .msg_len = 7,
                .qos = 1,
                .retain = 1,
            },
        },
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    char buf[128];

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "MQTT connected");
        /* 上线标记（offline 由遗嘱消息自动上报） */
        char st[96];
        snprintf(st, sizeof(st), "devices/%s/status", DEVICE_ID);
        esp_mqtt_client_publish(s_mqtt, st, "online", 0, 1, 1);
        /* 订阅配置通道（retain：上线立即收到最新配置）与指令通道 */
        char cfg_t[96], cmd_t[96];
        snprintf(cfg_t, sizeof(cfg_t), "devices/%s/config", DEVICE_ID);
        snprintf(cmd_t, sizeof(cmd_t), "devices/%s/cmd", DEVICE_ID);
        esp_mqtt_client_subscribe(s_mqtt, cfg_t, 1);
        esp_mqtt_client_subscribe(s_mqtt, cmd_t, 1);
        snprintf(buf, sizeof(buf), "MQTT OK\ncfg: %s", cfg_t);
        oled_show_lines(buf);
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected, reconnecting...");
        oled_show_lines("MQTT Lost\nReconnecting...");
        break;

    case MQTT_EVENT_DATA: {
        /* 收到的每条消息都完整打印，方便调试 */
        ESP_LOGI(TAG, "rx [%.*s] %.*s",
                 event->topic_len, event->topic,
                 event->data_len, event->data);

        static char buf[512];
        size_t len = event->data_len;
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, event->data, len);
        buf[len] = 0;

        bool is_cmd = strstr(event->topic, "/cmd") != NULL;
        bool is_cfg = strstr(event->topic, "/config") != NULL;

        cJSON *root = cJSON_Parse(buf);
        if (!root) { ESP_LOGW(TAG, "消息 JSON 解析失败: %s", buf); break; }
        log_json_fields(root);

        if (is_cfg) {
            /* 配置协议：{"cfg_version":N,"config":{...}} */
            const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "cfg_version");
            const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(root, "config");
            if (cJSON_IsObject(cfg)) {
                if (cJSON_IsNumber(ver)) g_cfg_version = ver->valueint;
                apply_config(cfg);
                /* 回执（带版本号）：控制台据此显示"已同步 vN" */
                char ack_topic[96], ack[96];
                snprintf(ack_topic, sizeof(ack_topic), "devices/%s/config/ack", DEVICE_ID);
                snprintf(ack, sizeof(ack), "{\"ack\":true,\"cfg_version\":%d}", (int)g_cfg_version);
                esp_mqtt_client_publish(s_mqtt, ack_topic, ack, 0, 1, 0);
            }
        } else if (is_cmd) {
            /* 指令：{"type":"display","text":"..."}，text 含 \n 可换行 */
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
            if (cJSON_IsString(type) && strcmp(type->valuestring, "display") == 0
                && cJSON_IsString(text)) {
                ESP_LOGI(TAG, "display: %s", text->valuestring);
                oled_show_message(text->valuestring);
                /* 回执：原样带回下发链路的 req_id（device-service 端到端对账用），
                   无 req_id（旧版服务/手动下发）时回基础回执 */
                char ack_topic[96], ack[192];
                const cJSON *req = cJSON_GetObjectItemCaseSensitive(root, "req_id");
                snprintf(ack_topic, sizeof(ack_topic), "devices/%s/cmd/ack", DEVICE_ID);
                if (cJSON_IsString(req)) {
                    snprintf(ack, sizeof(ack),
                             "{\"ack\":true,\"type\":\"display\",\"req_id\":\"%.120s\"}",
                             req->valuestring);
                } else {
                    snprintf(ack, sizeof(ack), "{\"ack\":true,\"type\":\"display\"}");
                }
                esp_mqtt_client_publish(s_mqtt, ack_topic, ack, 0, 1, 0);
            }
        }
        cJSON_Delete(root);
        break;
    }

    case MQTT_EVENT_ERROR:
        if (event->error_handle) {
            ESP_LOGE(TAG, "MQTT error, type=%d", event->error_handle->error_type);
        }
        break;

    default:
        break;
    }
}

/* ---------------- WiFi ---------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = event_data;
            ESP_LOGW(TAG, "WiFi disconnected, reason=%d", e->reason);
            s_wifi_connected = false;
            oled_show_lines("WiFi Lost\nRetrying...");
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = event_data;
        s_wifi_connected = true;
        char buf[64];
        snprintf(buf, sizeof(buf), "WiFi OK\nIP: " IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&e->ip_info.ip));
        oled_show_lines(buf);
        mqtt_start();   /* 拿到 IP 后启动 MQTT */
    }
}

/* 断线自动重连（每 5 秒尝试一次） */
static void wifi_retry_task(void *arg)
{
    while (1) {
        if (!s_wifi_connected) {
            esp_wifi_connect();   /* 失败返回错误码也无妨，下轮再试 */
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ---------------- 主入口 ---------------- */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ---- OLED SPI 初始化（同旧固件） ---- */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = OLED_PIN_CLK,
        .mosi_io_num = OLED_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = OLED_H_RES * OLED_V_RES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = OLED_PIN_CS,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi));

    gpio_set_direction((gpio_num_t)OLED_PIN_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)OLED_PIN_RST, GPIO_MODE_OUTPUT);

    s_disp_mutex = xSemaphoreCreateMutex();

    /* u8g2 初始化（内部完成 SSD1306 复位+配置，走上面两个回调）
     * 注意：必须用 _f（全帧 1024 字节缓冲），_1 只有 128 字节（8 行），
     *       其余区域会显示上电随机内容（乱码） */
    u8g2_Setup_ssd1306_128x64_noname_f(&s_u8g2, U8G2_R0, u8x8_byte_obc, u8x8_gpio_obc);
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);

    /* 从 NVS 恢复上次配置（重启/断电不丢）并应用亮度 */
    config_load();
    oled_cmd(0x81);
    oled_cmd(g_brightness);
    oled_show_lines(g_default_text);            /* 开机画面 = 默认显示文本 */

    /* ---- WiFi STA ---- */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wc = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreate(wifi_retry_task, "wifi_retry", 4096, NULL, 4, NULL);
    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 5, NULL);
    xTaskCreate(ota_task, "ota", 6144, NULL, 3, NULL);

    char boot[96];
    snprintf(boot, sizeof(boot), "WiFi Connecting\n%s", WIFI_SSID);
    oled_show_lines(boot);

    ESP_LOGI(TAG, "Ready. SSID=%s fw=%s cfg_v=%d",
             WIFI_SSID, APP_VERSION, g_cfg_version);
}
