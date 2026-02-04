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
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_rom_sys.h"

#define TAG "OBC_PRO"
#define LED_PIN             5
#define BOOT_BUTTON_GPIO    0
#define DEFAULT_SSID   "OBC_Device"
#define WEB_PASSWORD   "admin"

/* Global State */
static bool s_wifi_master_on = false;
static bool s_in_ap_mode = false;
static bool s_web_authenticated = false;
static uint8_t s_bright = 50;
static httpd_handle_t s_http_server = NULL;
static char wifi_ssid[33] = {0};
static char wifi_pass[65] = {0};

/* --- PWM / LED --- */
void ledc_init(void) {
    ledc_timer_config_t t = { .speed_mode=LEDC_LOW_SPEED_MODE, .timer_num=LEDC_TIMER_0, .duty_resolution=LEDC_TIMER_13_BIT, .freq_hz=5000, .clk_cfg=LEDC_AUTO_CLK };
    ledc_timer_config(&t);
    ledc_channel_config_t c = { .speed_mode=LEDC_LOW_SPEED_MODE, .channel=LEDC_CHANNEL_0, .timer_sel=LEDC_TIMER_0, .intr_type=LEDC_INTR_DISABLE, .gpio_num=LED_PIN, .duty=0, .hpoint=0 };
    ledc_channel_config(&c);
}
void set_brightness(uint8_t percent) {
    if (percent > 100) percent = 100;
    s_bright = percent;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (percent*8191)/100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* --- BLE --- */
#define DEVICE_NAME "OBC_DIMMER"
static uint8_t adv_service_uuid128[32] = { 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00 };
static esp_ble_adv_data_t adv_data = { .set_scan_rsp=false, .include_name=true, .include_txpower=false, .min_interval=0x0006, .max_interval=0x0010, .appearance=0x00, .manufacturer_len=0, .p_manufacturer_data=NULL, .service_data_len=0, .p_service_data=NULL, .service_uuid_len=16, .p_service_uuid=adv_service_uuid128, .flag=(ESP_BLE_ADV_FLAG_GEN_DISC|ESP_BLE_ADV_FLAG_BREDR_NOT_SPT) };
static esp_ble_adv_params_t adv_params = { .adv_int_min=0x20, .adv_int_max=0x40, .adv_type=ADV_TYPE_IND, .own_addr_type=BLE_ADDR_TYPE_PUBLIC, .channel_map=ADV_CHNL_ALL, .adv_filter_policy=ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY };
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) { if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT) esp_ble_gap_start_advertising(&adv_params); }
static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    if (event == ESP_GATTS_REG_EVT) {
        esp_ble_gap_set_device_name(DEVICE_NAME); esp_ble_gap_config_adv_data(&adv_data);
        esp_gatt_srvc_id_t service_id = {.is_primary=true, .id.inst_id=0x00, .id.uuid.len=ESP_UUID_LEN_16, .id.uuid.uuid.uuid16=0x00FF};
        esp_ble_gatts_create_service(gatts_if, &service_id, 4);
    } else if (event == ESP_GATTS_CREATE_EVT) {
        esp_ble_gatts_start_service(param->create.service_handle);
        esp_bt_uuid_t char_uuid = { .len=ESP_UUID_LEN_16, .uuid={.uuid16=0xFF01} };
        esp_ble_gatts_add_char(param->create.service_handle, &char_uuid, ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE, ESP_GATT_CHAR_PROP_BIT_READ|ESP_GATT_CHAR_PROP_BIT_WRITE, NULL, NULL);
    } else if (event == ESP_GATTS_WRITE_EVT) { if (param->write.len == 1) set_brightness(param->write.value[0]); }
}

/* --- NVS --- */
void save_config(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", wifi_ssid); nvs_set_str(h, "pass", wifi_pass);
        nvs_set_u8(h, "bright", s_bright);
        nvs_commit(h); nvs_close(h);
    }
}
void load_config(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
        size_t len=sizeof(wifi_ssid); if(nvs_get_str(h, "ssid", wifi_ssid, &len)!=ESP_OK) wifi_ssid[0]=0;
        len=sizeof(wifi_pass); if(nvs_get_str(h, "pass", wifi_pass, &len)!=ESP_OK) wifi_pass[0]=0;
        nvs_get_u8(h, "bright", &s_bright);
        nvs_close(h);
    }
    set_brightness(s_bright);
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
    
    /* Section 1: Light */
    snprintf(buf, sizeof(buf), "<h2>Control Center</h2><div class='card'><h3>Light</h3><input type='range' oninput='D(this.value)' value='%d'></div>", 
             s_bright);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    
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
    httpd_resp_set_status(req, "302 Found"); httpd_resp_set_hdr(req, "Location", "/"); httpd_resp_send(req, NULL, 0); return ESP_OK;
}

static esp_err_t api_handler(httpd_req_t *req) {
    char buf[256]; char type=req->uri[5];
    if(type=='w') { /* WiFi */
        int ret=httpd_req_recv(req, buf, sizeof(buf)-1); if(ret>0){ buf[ret]=0; parse_param(buf,"s",wifi_ssid,sizeof(wifi_ssid)); parse_param(buf,"p",wifi_pass,sizeof(wifi_pass)); save_config(); }
        httpd_resp_send(req,"Saved.",-1);
    } else if(httpd_req_get_url_query_str(req,buf,sizeof(buf))==ESP_OK) {
        char v[10]={0}; parse_param(buf,"v",v,sizeof(v));
        if(type=='d') set_brightness(atoi(v)); /* Dim */
        httpd_resp_send(req,"OK",2);
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
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if(s_wifi_master_on && !s_in_ap_mode) { ESP_LOGI(TAG, "STA Disconnected, retrying..."); esp_wifi_connect(); }
    }
}
void connect_sta(void) {
    ESP_LOGI(TAG, "Switching to STA Mode...");
    wifi_config_t c={0}; strncpy((char*)c.sta.ssid,wifi_ssid,32); strncpy((char*)c.sta.password,wifi_pass,64);
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA,&c); 
    esp_wifi_start(); esp_wifi_connect();
}
void start_ap(void) {
    if(s_in_ap_mode) return;
    ESP_LOGI(TAG, "Switching to AP Mode...");
    esp_wifi_disconnect(); esp_wifi_stop();
    wifi_config_t c = { .ap = { .ssid=DEFAULT_SSID, .ssid_len=strlen(DEFAULT_SSID), .max_connection=4, .authmode=WIFI_AUTH_OPEN } };
    esp_wifi_set_mode(WIFI_MODE_AP); esp_wifi_set_config(WIFI_IF_AP, &c); esp_wifi_start();
    
    httpd_config_t h = HTTPD_DEFAULT_CONFIG(); h.max_uri_handlers=12; h.stack_size=8192; h.uri_match_fn = httpd_uri_match_wildcard;
    httpd_start(&s_http_server, &h);
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/",.method=HTTP_GET,.handler=root_handler});
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/login",.method=HTTP_POST,.handler=login_handler});
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/api/w",.method=HTTP_POST,.handler=api_handler});
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/api/d",.method=HTTP_POST,.handler=api_handler}); // POST for slider
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/api/c",.method=HTTP_POST,.handler=api_handler});
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/ota",.method=HTTP_POST,.handler=ota_handler});
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/ota/remote",.method=HTTP_POST,.handler=ota_remote_trigger_handler});
    httpd_register_uri_handler(s_http_server, &(httpd_uri_t){.uri="/*",.method=HTTP_GET,.handler=captive_portal_handler});

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

void app_main(void) {
    nvs_flash_init(); ledc_init();
    
    load_config();
    esp_netif_init(); esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_netif_create_default_wifi_sta(); esp_netif_create_default_wifi_ap();
    wifi_init_config_t c=WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&c);

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t bc=BT_CONTROLLER_INIT_CONFIG_DEFAULT(); esp_bt_controller_init(&bc); esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_config_t bdc=BT_BLUEDROID_INIT_CONFIG_DEFAULT(); esp_bluedroid_init_with_cfg(&bdc); esp_bluedroid_enable();
    esp_ble_gatts_register_callback(gatts_profile_event_handler); esp_ble_gap_register_callback(gap_event_handler); esp_ble_gatts_app_register(0);

    xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL);
    xTaskCreate(btn_task, "btn", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Device Ready.");
}
