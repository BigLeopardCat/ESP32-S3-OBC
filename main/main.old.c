#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "esp_rom_sys.h"

// LCD / LVGL Includes
#include "driver/spi_master.h"
#include "lvgl.h"
#include "lvgl.h"
#include "esp_netif.h"


#define TAG "OBC_PRO"
#define BOOT_BUTTON_GPIO    0
#define DEFAULT_SSID   "OBC_Device"
#define WEB_PASSWORD   "admin"

// OLED SPI Pins
#define OLED_PIN_CS    36
#define OLED_PIN_DC    37
#define OLED_PIN_RST   38
#define OLED_PIN_MOSI  39
#define OLED_PIN_CLK   40
#define OLED_H_RES     128
#define OLED_V_RES     64

/* Global State */
static bool s_wifi_master_on = false;
static bool s_in_ap_mode = false;
static bool s_web_authenticated = false;
static httpd_handle_t s_http_server = NULL;
static char wifi_ssid[33] = {0};
static char wifi_pass[65] = {0};

/* UI State */
static lv_obj_t *ui_status_label = NULL;
static SemaphoreHandle_t s_gui_sem = NULL;

/* --- UI Functions --- */
static void ui_update_text(const char *text) {
    if (xSemaphoreTake(s_gui_sem, pdMS_TO_TICKS(100))) {
    if (ui_status_label && s_gui_sem) {
            lv_label_set_text(ui_status_label, text);
            xSemaphoreGive(s_gui_sem);
        }
    }
}

/* --- NVS --- */
void save_config(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", wifi_ssid); nvs_set_str(h, "pass", wifi_pass);
        nvs_commit(h); nvs_close(h);
    }
}
void load_config(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
        size_t len=sizeof(wifi_ssid); if(nvs_get_str(h, "ssid", wifi_ssid, &len)!=ESP_OK) wifi_ssid[0]=0;
        len=sizeof(wifi_pass); if(nvs_get_str(h, "pass", wifi_pass, &len)!=ESP_OK) wifi_pass[0]=0;
        nvs_close(h);
    }
}

/* --- OTA & Web Util --- */
void remote_ota_task(void *pvParameter) {
    char *url = (char *)pvParameter;
    esp_http_client_config_t cc = { .url = url, .keep_alive_enable = true, .skip_cert_common_name_check = true };
    esp_https_ota_config_t oc = { .http_config = &cc };
    if (esp_https_ota(&oc) == ESP_OK) esp_restart();
    free(url); vTaskDelete(NULL);
}
void parse_param(char *buf, const char *key, char *dest, int max_len) {
    char *p = strstr(buf, key); if (!p) return;
    p += strlen(key); if (*p == '=') p++;
    int i = 0; while (*p && *p != '&' && i < max_len-1) dest[i++] = *p++; dest[i] = 0;
}

/* --- Web UI --- */
const char* HTML_HEAD = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=0'><style>\
:root{--bg:#f2f2f7;--card:#fff;--txt:#000;--hl:#007aff} @media(prefers-color-scheme:dark){:root{--bg:#1c1c1e;--card:#2c2c2e;--txt:#fff;--hl:#0a84ff}}\
body{font-family:-apple-system,system-ui,sans-serif;background:var(--bg);color:var(--txt);margin:0;padding:20px;}\
.card{background:var(--card);border-radius:12px;padding:16px;margin-bottom:16px;box-shadow:0 2px 8px rgba(0,0,0,0.05)}\
h2{margin:0 0 10px;font-size:22px} input,button{width:100%;box-sizing:border-box;margin:5px 0;padding:12px;border-radius:8px;border:1px solid #ddd;font-size:16px}\
button{background:var(--hl);color:#fff;border:none;font-weight:600} input[type=range]{height:6px;padding:0}\
input[type=color]{height:50px;padding:0;overflow:hidden;border:none}\
</style><script>function p(u,d){fetch(u,{method:'POST',body:d})} function g(id){return document.getElementById(id)}\
function D(v){p('/api/d?v='+v)} function C(v){p('/api/c?v='+v.replace('#',''))} function W(){g('wf').submit()}\
function UF(){var f=g('f').files[0];if(!f)return;var d=new FormData();d.append('f',f);g('ub').innerText='Uploading...';fetch('/ota',{method:'POST',body:d}).then(r=>{r.ok?location.reload():alert('Error')})}\
function UR(){var u=g('ur').value;if(u){var d=new FormData();d.append('u',u);p('/ota/remote',d);alert('Starting Update...')}}\
</script></head><body>";

static esp_err_t login_handler(httpd_req_t *req) {
    char buf[128]; int ret=httpd_req_recv(req,buf,sizeof(buf)-1); if(ret<=0)return ESP_FAIL; buf[ret]=0;
    char pwd[64]={0}; parse_param(buf,"password",pwd,sizeof(pwd));
    if(strcmp(pwd,WEB_PASSWORD)==0) {
        s_web_authenticated=true; httpd_resp_set_status(req,"302 Found"); httpd_resp_set_hdr(req,"Location","/"); httpd_resp_send(req,NULL,0);
    } else {
        httpd_resp_send_chunk(req, HTML_HEAD, HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, "<div class='card'><h2>Error</h2><p>Wrong Password</p><a href='/'>Retry</a></div>", HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return ESP_OK;
}
static esp_err_t root_handler(httpd_req_t *req) {
    if(!s_web_authenticated) {
        httpd_resp_send_chunk(req, HTML_HEAD, HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, "<div class='card' style='margin-top:20vh'><h2>Login</h2><form action='/login' method='post'><input name='password' type='password' placeholder='Password'><button>Enter</button></form></div>", HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, NULL, 0); 
        return ESP_OK;
    }
    
    char buf[512]; 
    httpd_resp_send_chunk(req, HTML_HEAD, HTTPD_RESP_USE_STRLEN);

    /* Section 2: WiFi */
    snprintf(buf, sizeof(buf), "<div class='card'><h3>WiFi</h3><form id='wf' action='/api/w' method='post'><input name='s' value='%s' placeholder='SSID'><input name='p' value='%s' placeholder='Pass'></form><button onclick='W()'>Save & Connect</button></div>", 
             wifi_ssid, wifi_pass);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    
    /* Section 3: Update + End */
    httpd_resp_send_chunk(req, "<div class='card'><h3>Update</h3><input type='file' id='f'><button id='ub' onclick='UF()'>Local Flash</button><hr><input id='ur' placeholder='http://url/fw.bin'><button onclick='UR()'>Remote Flash</button></div></body></html>", HTTPD_RESP_USE_STRLEN);
    
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
static esp_err_t captive_portal_handler(httpd_req_t *req) {
    if (req->method == HTTP_POST) {
        // 对于后台应用的 POST 请求 (如 /mmtls)，直接回复 204 No Content。
        // 不发送任何 Body 数据，快速结束连接，节省资源。
        // 同时告诉 App "请求成功"，防止它误以为网络不通而疯狂重试。
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    // 对于浏览器的 GET 请求，重定向到主页
    httpd_resp_set_status(req, "302 Found"); 
    httpd_resp_set_hdr(req, "Location", "/"); 
    httpd_resp_send(req, NULL, 0); 
    return ESP_OK;
}

static esp_err_t api_handler(httpd_req_t *req) {
    char buf[256]; char type=req->uri[5];
    if(type=='w') { /* WiFi */
        int ret=httpd_req_recv(req, buf, sizeof(buf)-1); if(ret>0){ buf[ret]=0; parse_param(buf,"s",wifi_ssid,sizeof(wifi_ssid)); parse_param(buf,"p",wifi_pass,sizeof(wifi_pass)); save_config(); }
        httpd_resp_send(req,"Saved.",-1);
    } return ESP_OK;
}
static esp_err_t ota_handler(httpd_req_t *req) {
    esp_ota_handle_t h; const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (!p || esp_ota_begin(p, OTA_SIZE_UNKNOWN, &h)!=ESP_OK) { httpd_resp_send_500(req); return ESP_FAIL; }
    char *buf = malloc(1024); int r, rem=req->content_len;
    while(rem>0 && (r=httpd_req_recv(req, buf, MIN(rem,1024)))>0) { esp_ota_write(h, buf, r); rem-=r; }
    free(buf); esp_ota_end(h); esp_ota_set_boot_partition(p); httpd_resp_send(req,"OK",2); vTaskDelay(100); esp_restart(); return ESP_OK;
}
static esp_err_t ota_remote_trigger_handler(httpd_req_t *req) {
    char buf[256]; int ret=httpd_req_recv(req, buf, sizeof(buf)-1); if(ret>0){ buf[ret]=0; char url[128]={0}; parse_param(buf,"u",url,sizeof(url)); xTaskCreate(remote_ota_task, "rota", 8192, strdup(url), 5, NULL); }
    httpd_resp_send(req,"OK",2); return ESP_OK;
}

/* --- Core Logic --- */
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Station connected to AP");
        ui_update_text(LV_SYMBOL_WIFI " Phone Connected!");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ui_update_text(LV_SYMBOL_WARNING " WiFi Disconnected\nRetrying...");
        if(s_wifi_master_on && !s_in_ap_mode) { ESP_LOGI(TAG, "STA Disconnected, retrying..."); esp_wifi_connect(); }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        char buf[128];
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s\nIP: " IPSTR, wifi_ssid, IP2STR(&event->ip_info.ip));
        ui_update_text(buf);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}
void connect_sta(void) {
    ESP_LOGI(TAG, "Switching to STA Mode...");
    char buf[128];
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " Connecting...\nS:%s\nP:%s", wifi_ssid, wifi_pass);
    ui_update_text(buf);
    wifi_config_t c={0}; strncpy((char*)c.sta.ssid,wifi_ssid,32); strncpy((char*)c.sta.password,wifi_pass,64);
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA,&c); 
    esp_wifi_start(); esp_wifi_connect();
}
void start_ap(void) {
    if(s_in_ap_mode) return;
    ESP_LOGI(TAG, "Switching to AP Mode...");
    char buf[128];
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " AP Mode\nS:%s\nWeb:%s", DEFAULT_SSID, WEB_PASSWORD);
    ui_update_text(buf);
    esp_wifi_disconnect(); esp_wifi_stop();
    wifi_config_t c = { .ap = { .ssid=DEFAULT_SSID, .ssid_len=strlen(DEFAULT_SSID), .max_connection=4, .authmode=WIFI_AUTH_OPEN } };
    esp_wifi_set_mode(WIFI_MODE_AP); esp_wifi_set_config(WIFI_IF_AP, &c); esp_wifi_start();
    
    httpd_config_t h = HTTPD_DEFAULT_CONFIG(); 
    h.max_uri_handlers = 13; 
    h.stack_size = 8192; 
    h.uri_match_fn = httpd_uri_match_wildcard;
    h.lru_purge_enable = true; // Clean up old connections
    
    httpd_start(&s_http_server, &h);
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/",.method=HTTP_GET,.handler=root_handler});
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/login",.method=HTTP_POST,.handler=login_handler});
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/api/w",.method=HTTP_POST,.handler=api_handler});
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/ota",.method=HTTP_POST,.handler=ota_handler});
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/ota/remote",.method=HTTP_POST,.handler=ota_remote_trigger_handler});
    
    // Catch-all handlers for Captive Portal (MUST be last)
    // Handle both GET and POST to avoid "Method not allowed" spam from background apps
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/*",.method=HTTP_GET,.handler=captive_portal_handler});
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/*",.method=HTTP_POST,.handler=captive_portal_handler}); 

    s_in_ap_mode=true; s_web_authenticated=false;
}
void stop_ap(void) {
    if(!s_in_ap_mode) return;
    ESP_LOGI(TAG, "Stopping AI Mode...");
    save_config(); if(s_http_server) { httpd_stop(s_http_server); s_http_server=NULL; }
    s_in_ap_mode=false;
    if(s_wifi_master_on) connect_sta(); else esp_wifi_stop();
}
void master_toggle(void) {
    s_wifi_master_on = !s_wifi_master_on;
    ESP_LOGI(TAG, "Master Switch: %d", s_wifi_master_on);
    if(s_wifi_master_on) { if(!s_in_ap_mode) connect_sta(); } else { if(s_in_ap_mode) stop_ap(); else esp_wifi_stop(); }
}

/* --- Tasks --- */
void dns_task(void *pvParameters) {
    uint8_t d[512]; struct sockaddr_in s_addr, c_addr; socklen_t l=sizeof(c_addr); 
    int s = socket(AF_INET, SOCK_DGRAM, 0); struct timeval tv={.tv_sec=1}; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    memset(&s_addr,0,sizeof(s_addr)); s_addr.sin_family=AF_INET; s_addr.sin_addr.s_addr=htonl(INADDR_ANY); s_addr.sin_port=htons(53);
    bind(s, (struct sockaddr *)&s_addr, sizeof(s_addr));
    while(1) {
        if(s_in_ap_mode) {
            int len = recvfrom(s, d, sizeof(d), 0, (struct sockaddr *)&c_addr, &l);
            if(len>12) {
                d[2]|=0x80; d[3]|=0x80; d[7]=1; 
                int idx=12; 
                while(idx < len && d[idx]!=0) { 
                    int label_len = d[idx];
                    if(idx + label_len + 1 >= len) break;
                    idx += label_len + 1; 
                } 
                idx+=5; 
                if(idx+16<sizeof(d)) {
                     d[idx++]=0xC0; d[idx++]=0x0C; d[idx++]=0x00; d[idx++]=0x01; d[idx++]=0x00; d[idx++]=0x01; 
                     d[idx++]=0x00; d[idx++]=0x00; d[idx++]=0x00; d[idx++]=0x3C; d[idx++]=0x00; d[idx++]=0x04;
                     d[idx++]=192; d[idx++]=168; d[idx++]=4; d[idx++]=1;
                     sendto(s, d, idx, 0, (struct sockaddr *)&c_addr, sizeof(c_addr));
                }
            }
        } else { vTaskDelay(pdMS_TO_TICKS(500)); }
    }
}
void btn_task(void*z) {
    gpio_config_t c={.pin_bit_mask=(1ULL<<BOOT_BUTTON_GPIO), .mode=GPIO_MODE_INPUT, .pull_up_en=1}; gpio_config(&c);
    int cnt=0; bool p=0;
    while(1) {
        if(gpio_get_level(BOOT_BUTTON_GPIO)==0) {
            if(!p) { p=1; cnt=0; ESP_LOGI(TAG,"Button Press Detected"); }
            if(p) { if(++cnt > 150) { master_toggle(); cnt=-1000; } } 
        } else {
            if(p && cnt>0 && cnt!=-1000) { 
                if(s_wifi_master_on) { 
                    if(s_in_ap_mode) { ESP_LOGI(TAG, "Command: Switch to STA"); stop_ap(); }
                    else { ESP_LOGI(TAG, "Command: Switch to AP"); start_ap(); }
                } else {
                     ESP_LOGW(TAG, "WiFi Master is OFF. Long press to enable.");
                }
            }
            p=0; cnt=0;
        }
        vTaskDelay(20/portTICK_PERIOD_MS);
    }
}

void ui_tick_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (s_gui_sem && xSemaphoreTake(s_gui_sem, pdMS_TO_TICKS(100))) {
            lv_tick_inc(10); // Ensure LVGL time progresses
            lv_timer_handler();
            xSemaphoreGive(s_gui_sem);
        }
    }
}

static spi_device_handle_t spi;

static void oled_cmd(uint8_t cmd) {
    gpio_set_level((gpio_num_t)OLED_PIN_DC, 0);
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    spi_device_polling_transmit(spi, &t);
}

static void oled_data(const uint8_t *data, int len) {
    if (len == 0) return;
    gpio_set_level((gpio_num_t)OLED_PIN_DC, 1);
    spi_transaction_t t = {.length = (size_t)len * 8, .tx_buffer = data};
    spi_device_polling_transmit(spi, &t);
}

static void disp_rounder_event_cb(lv_event_t * e) {
    lv_area_t * area = lv_event_get_invalidated_area(e);
    area->y1 = area->y1 & (~0x7);
    area->y2 = (area->y2 | 0x7);
}

static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    int out_len = w * ((h + 7) / 8);
    
    // Allocate a DMA capable buffer for the translated 1bpp OLED data
    uint8_t *buf = heap_caps_malloc(out_len, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        lv_display_flush_ready(disp);
        return;
    }
    memset(buf, 0, out_len);

    // Convert LVGL RGB565 arrays into 1bpp Format
    // Invert mapping for OLED mode: dark LVGL colors -> pixel ON (white), light colors -> pixel OFF (black)
    uint16_t *color_p = (uint16_t *)px_map;
    for(int y = 0; y < h; y++) {
        for(int x = 0; x < w; x++) {
            uint16_t c = color_p[y * w + x];
            uint32_t r = (c >> 11) & 0x1F;
            uint32_t g = (c >> 5) & 0x3F;
            uint32_t b = c & 0x1F;
            // 扩展到 0-255
            r = (r * 255) / 31;
            g = (g * 255) / 63;
            b = (b * 255) / 31;
            
            uint32_t bright = (r * 299 + g * 587 + b * 114) / 1000;
            // 原代码这里判定为 bright < 128 (中心亮度)
            if (bright < 128) {
                buf[(y / 8) * w + x] |= (1 << (y % 8)); 
            }
        }
    }

    // Standard Page Addressing Write Sequence for 128x64 OLED
    for (int p = 0; p < ((h + 7) / 8); p++) {
        int page = (area->y1 / 8) + p;
        oled_cmd(0xB0 | page);          // Set Page Address
        oled_cmd(0x00 | (area->x1 & 0x0F));       // Set Lower Column Address
        oled_cmd(0x10 | ((area->x1 >> 4) & 0x0F)); // Set Higher Column Address
        oled_data(&buf[p * w], w);
    }

    free(buf);

    lv_display_flush_ready(disp);
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 1. SPI Bus Init
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = OLED_PIN_CLK,
        .mosi_io_num = OLED_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = OLED_H_RES * OLED_V_RES,
    };
    spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = OLED_PIN_CS,
        .queue_size = 7,
    };
    spi_bus_add_device(SPI2_HOST, &devcfg, &spi);

    // OLED Hardware Init
    gpio_set_direction((gpio_num_t)OLED_PIN_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)OLED_PIN_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)OLED_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level((gpio_num_t)OLED_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    oled_cmd(0xAE); // display off
    oled_cmd(0x20); // Set Memory Addressing Mode
    oled_cmd(0x02); // 02=Page Addressing Mode
    oled_cmd(0xB0); // Set Page Start Address for Page Addressing Mode,0-7
    oled_cmd(0xC8); // Set COM Output Scan Direction
    oled_cmd(0x00); // set low column address
    oled_cmd(0x10); // set high column address
    oled_cmd(0x40); // set start line address
    oled_cmd(0x81); // set contrast control register
    oled_cmd(0xFF);
    oled_cmd(0xA1); // set segment re-map 0 to 127
    oled_cmd(0xA6); // set normal display
    oled_cmd(0xA8); // set multiplex ratio(1 to 64)
    oled_cmd(0x3F); //
    oled_cmd(0xA4); // 0xa4,Output follows RAM content;0xa5,Output ignores RAM content
    oled_cmd(0xD3); // set display offset
    oled_cmd(0x00); // no offset
    oled_cmd(0xD5); // set display clock divide ratio/oscillator frequency
    oled_cmd(0xF0); // set divide ratio
    oled_cmd(0xD9); // set pre-charge period
    oled_cmd(0x22); 
    oled_cmd(0xDA); // set com pins hardware configuration
    oled_cmd(0x12);
    oled_cmd(0xDB); // set vcomh
    oled_cmd(0x20); // 0x20,0.77xVcc
    oled_cmd(0x8D); // set DC-DC enable
    oled_cmd(0x14);
    oled_cmd(0xAF); // display on

    // Clear OLED RAM
    oled_cmd(0x22); oled_cmd(0x00); oled_cmd(0x07);
    oled_cmd(0x21); oled_cmd(0x00); oled_cmd(0x7F);
    uint8_t *clr = heap_caps_malloc(1024, MALLOC_CAP_DMA);
    if(clr) {
        memset(clr, 0, 1024);
        oled_data(clr, 1024);
        free(clr);
    }

    s_gui_sem = xSemaphoreCreateMutex();

    // 4. LVGL Port Init
    lv_init();

    // Declaration of external animation frames
    extern const lv_image_dsc_t * anim_frames[];
    extern const int anim_frames_count;

    lv_display_t *disp = lv_display_create(OLED_H_RES, OLED_V_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    
    // allocate draw buffer - V9 requires raw arrays instead of struct wrappers, RGB565 is 2 bytes per pixel
    static uint8_t draw_buf[OLED_H_RES * 16 * 2]; 
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, disp_flush);
    lv_display_add_event_cb(disp, disp_rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    // 5. Create UI Label
    lv_obj_t *bg = lv_screen_active();

    // Create and display the boot logo animation
    lv_obj_t *boot_logo = lv_animimg_create(bg);
    lv_animimg_set_src(boot_logo, (const void **)anim_frames, anim_frames_count);
    
    // Set duration: play at ~30 FPS (33ms per frame)
    uint32_t anim_duration = anim_frames_count * 33;
    lv_animimg_set_duration(boot_logo, anim_duration);
    lv_animimg_set_repeat_count(boot_logo, 1); // 1 means it runs once
    lv_obj_align(boot_logo, LV_ALIGN_CENTER, 0, 0); // Center the image
    lv_animimg_start(boot_logo);

    // Allow the logo to display (Blocking wait during boot)
    uint32_t start_time = lv_tick_get();
    while (lv_tick_get() - start_time < anim_duration) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_tick_inc(10);
    }

    // Remove the logo and create the status label
    lv_obj_del(boot_logo);

    lv_obj_t *ui_status_label_local = lv_label_create(bg);
    
    // 限制标签的宽度为屏幕宽度（128）
    lv_obj_set_width(ui_status_label_local, 128);
    
    // 开启长文本自动换行模式
    lv_label_set_long_mode(ui_status_label_local, LV_LABEL_LONG_WRAP);
    
    // 让多行文本始终保持内部居中对齐
    lv_obj_set_style_text_align(ui_status_label_local, LV_TEXT_ALIGN_CENTER, 0);

    lv_label_set_text(ui_status_label_local, "System Booting...");
    lv_obj_align(ui_status_label_local, LV_ALIGN_CENTER, 0, 0);

    // Provide a small animation / wait before assigning globally
    lv_timer_handler(); 
    
    ui_status_label = ui_status_label_local;
    
    // Create LVGL tick task
    xTaskCreate(ui_tick_task, "lv_tick", 4096, NULL, 5, NULL);

    load_config();
    esp_netif_init(); esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    esp_netif_create_default_wifi_sta(); esp_netif_create_default_wifi_ap();
    wifi_init_config_t c=WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&c);

    xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL);
    xTaskCreate(btn_task, "btn", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Device Ready.");
    
    ui_update_text(LV_SYMBOL_WIFI " Ready.\nPress Btn for AP");
}
