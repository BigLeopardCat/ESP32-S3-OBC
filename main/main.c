#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/ledc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_image_format.h" // 必须包含此头文件以验证固件

// 蓝牙相关头文件
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"

#define TAG "OBC_SYSTEM"

// --- 硬件定义 ---
#define LED_PIN 4  // PWM 输出 IO

// --- WiFi 配置 ---
#define WIFI_SSID      "CU_BNkT" // 请在此处修改您的WiFi名称
#define WIFI_PASS      "mp2ca2rz" // 请在此处修改您的WiFi密码
#define OTA_URL        "http://66.103.206.198/ota/OBC.bin" // 请填写您的公网 HTTPS 地址
//#define OTA_URL        "http://192.168.1.7:8070/OBC.bin" // 旧的本地测试地址

// --- BLE 定义 ---
#define DEVICE_NAME    "ESP32S3_DIMMER"
// 自定义 UUID
#define SERVICE_UUID           0x00FF 
#define CHAR_DIMMER_UUID       0xFF01
#define CHAR_OTA_CTRL_UUID     0xFF02
#define CHAR_OTA_DATA_UUID     0xFF03

static uint8_t char_prop_write = ESP_GATT_CHAR_PROP_BIT_WRITE;
static uint8_t char_prop_write_nr = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;

// OTA 句柄
static esp_ota_handle_t update_handle = 0;
static const esp_partition_t *update_partition = NULL;

// --- PWM 功能 ---
void ledc_init(void) {
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_PIN,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel);
}

void set_brightness(uint8_t percent) {
    if (percent > 100) percent = 100;
    uint32_t duty = (percent * 8191) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "Brightness set to %d%%", percent);
}

// --- WiFi OTA 任务 ---
void wifi_ota_task(void *pvParameter) {
    ESP_LOGI(TAG, "Starting WiFi OTA...");
    esp_http_client_config_t config = {
        .url = OTA_URL,
        .timeout_ms = 60000,            // 60s 超时
        .keep_alive_enable = false,     // 关闭长连接，减少维护开销
        .buffer_size = 1024,            // 减小缓冲区，避免大包重传阻塞
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach, 
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    if (esp_https_ota(&ota_config) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi OTA Success! Restarting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "WiFi OTA Failed");
    }
    vTaskDelete(NULL);
}

// --- BLE OTA 逻辑 ---
// 指令：0=Start, 1=End, 2=Request WiFi OTA
void handle_ota_control(uint8_t cmd) {
    esp_err_t err;
    switch (cmd) {
        case 0: // Start BLE OTA
            ESP_LOGI(TAG, "BLE OTA Start");
            update_partition = esp_ota_get_next_update_partition(NULL);
            if (update_partition == NULL) {
                ESP_LOGE(TAG, "No OTA partition found");
                return;
            }
            err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
            if (err != ESP_OK) ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
            break;

        case 1: // End BLE OTA
            ESP_LOGI(TAG, "BLE OTA End");
            if (esp_ota_end(update_handle) == ESP_OK) {
                err = esp_ota_set_boot_partition(update_partition);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Restarting...");
                    esp_restart();
                }
            } else {
                ESP_LOGE(TAG, "OTA End failed");
            }
            break;

        case 2: // Trigger WiFi OTA
            // 启动 WiFi OTA 任务
            // 延迟一点点启动，确保蓝牙有时间回复 Write Response 给手机
            xTaskCreate(wifi_ota_task, "wifi_ota", 8192, NULL, 5, NULL);
            break;

        case 3: // Active Rollback (手动回滚到上一个版本)
            ESP_LOGI(TAG, "CMD: Active Rollback requested");
            // 1. 获取当前正在运行的分区
            const esp_partition_t *running = esp_ota_get_running_partition();
            // 2. 获取下一个更新分区 (在双分区模式下，这就是"另一个"分区，也就是上个版本所在的分区)
            const esp_partition_t *prev = esp_ota_get_next_update_partition(NULL);

            if (running && prev) {
                ESP_LOGI(TAG, "Current: %s, Target Rollback: %s", running->label, prev->label);
                
                // 3. 验证目标分区里是否有合法的固件
                esp_image_metadata_t data;
                const esp_partition_pos_t part_pos = {
                    .offset = prev->address,
                    .size = prev->size,
                };
                
                if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &part_pos, &data) == ESP_OK) {
                    ESP_LOGI(TAG, "Rollback partition verified! Switching...");
                    // 4. 设置启动分区 (这会更新 otadata 中的序列号)
                    if (esp_ota_set_boot_partition(prev) == ESP_OK) {
                        ESP_LOGI(TAG, "Restarting to execute rollback...");
                        esp_restart();
                    } else {
                        ESP_LOGE(TAG, "Failed to set boot partition");
                    }
                } else {
                    ESP_LOGE(TAG, "Rollback target is invalid (no valid app found)");
                }
            }
            break;
    }
}

void handle_ota_data(const uint8_t *data, uint16_t len) {
    if (update_handle) {
        esp_ota_write(update_handle, data, len);
    }
}

// --- BLE GATT 回调 ---
enum {
    IDX_SVC,
    IDX_CHAR_DIMMER,
    IDX_CHAR_DIMMER_VAL,
    IDX_CHAR_OTA_CTRL,
    IDX_CHAR_OTA_CTRL_VAL,
    IDX_CHAR_OTA_DATA,
    IDX_CHAR_OTA_DATA_VAL,
    HRS_IDX_NB,
};

uint16_t handle_table[HRS_IDX_NB];


// --- BLE 5.0 Extended Advertising Data ---
static const uint8_t raw_adv_data[] = {
    0x02, 0x01, 0x06, // Flags: General Discoverable, BR/EDR Not Supported
    0x03, 0x03, 0xFF, 0x00, // Complete List of 16-bit Service Class UUIDs (0x00FF)
    0x0F, 0x09, 'E', 'S', 'P', '3', '2', 'S', '3', '_', 'D', 'I', 'M', 'M', 'E', 'R' // Complete Local Name
};

static esp_ble_gap_ext_adv_params_t ext_adv_params = {
    .type = ESP_BLE_GAP_SET_EXT_ADV_PROP_CONNECTABLE,
    .interval_min = 0x20,
    .interval_max = 0x40,
    .channel_map = ADV_CHNL_ALL,
    .filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    .primary_phy = ESP_BLE_GAP_PHY_1M,
    .max_skip = 0,
    .secondary_phy = ESP_BLE_GAP_PHY_1M,
    .sid = 0,
    .scan_req_notif = false,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
};

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_EXT_ADV_SET_PARAMS_COMPLETE_EVT:
             if (param->ext_adv_set_params.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Set ext adv params failed: %d", param->ext_adv_set_params.status);
             } else {
                ESP_LOGI(TAG, "Set ext adv params success");
                esp_ble_gap_config_ext_adv_data_raw(0, sizeof(raw_adv_data), raw_adv_data);
             }
            break;
        case ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT:
             if (param->ext_adv_data_set.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Set ext adv data failed: %d", param->ext_adv_data_set.status);
             } else {
                ESP_LOGI(TAG, "Set ext adv data success");
                esp_ble_gap_ext_adv_start(1, &(esp_ble_gap_ext_adv_t){.instance = 0, .duration = 0, .max_events = 0});
             }
            break;
        case ESP_GAP_BLE_EXT_ADV_START_COMPLETE_EVT:
             if (param->ext_adv_start.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Ext adv start failed: %d", param->ext_adv_start.status);
             } else {
                ESP_LOGI(TAG, "Ext adv start success");
             }
            break;
        default:
            break;
    }
}

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    switch (event) {
    case ESP_GATTS_REG_EVT: {
        esp_ble_gap_set_device_name(DEVICE_NAME);
        
        // Use Extended Advertising Setup
        esp_ble_gap_ext_adv_set_params(0, &ext_adv_params);

        // 简化的属性表创建...
        esp_gatt_srvc_id_t service_id = {
            .is_primary = true, 
            .id.inst_id = 0x00, 
            .id.uuid.len = ESP_UUID_LEN_16, 
            .id.uuid.uuid.uuid16 = SERVICE_UUID
        };
        esp_ble_gatts_create_service(gatts_if, &service_id, HRS_IDX_NB*2); 
        break;
    }
    case ESP_GATTS_CREATE_EVT: // 服务创建后，逐个添加特征
        handle_table[IDX_SVC] = param->create.service_handle;
        
        esp_bt_uuid_t uuid;
        uuid.len = ESP_UUID_LEN_16;
        
        // 1. 调光特征
        uuid.uuid.uuid16 = CHAR_DIMMER_UUID;
        esp_ble_gatts_add_char(handle_table[IDX_SVC], &uuid, ESP_GATT_PERM_WRITE, char_prop_write, NULL, NULL);
        break;
        
    case ESP_GATTS_ADD_CHAR_EVT:
        // 通过 UUID 判断添加的是哪个特征，并保存 Handle
        if (param->add_char.char_uuid.uuid.uuid16 == CHAR_DIMMER_UUID) {
            handle_table[IDX_CHAR_DIMMER] = param->add_char.attr_handle;
            
            // 添加下一个：OTA 控制
            esp_bt_uuid_t uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = CHAR_OTA_CTRL_UUID };
            esp_ble_gatts_add_char(handle_table[IDX_SVC], &uuid, ESP_GATT_PERM_WRITE, char_prop_write, NULL, NULL);
        }
        else if (param->add_char.char_uuid.uuid.uuid16 == CHAR_OTA_CTRL_UUID) {
            handle_table[IDX_CHAR_OTA_CTRL] = param->add_char.attr_handle;
            
            // 添加下一个：OTA 数据
            esp_bt_uuid_t uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = CHAR_OTA_DATA_UUID };
            esp_ble_gatts_add_char(handle_table[IDX_SVC], &uuid, ESP_GATT_PERM_WRITE, char_prop_write_nr, NULL, NULL);
        }
        else if (param->add_char.char_uuid.uuid.uuid16 == CHAR_OTA_DATA_UUID) {
             handle_table[IDX_CHAR_OTA_DATA] = param->add_char.attr_handle;
             // 全部添加完，启动服务
             esp_ble_gatts_start_service(handle_table[IDX_SVC]);
        }
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == handle_table[IDX_CHAR_DIMMER]) {
            set_brightness(param->write.value[0]);
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
            }
        }
        else if (param->write.handle == handle_table[IDX_CHAR_OTA_CTRL]) {
            // 注意：handle_ota_control 内需要处理 Response，我们稍微改动一下调用方式
            // 或者简单地在这里发 Response 也可以，但最好传递 gatts_if 等参数进去
             if (param->write.need_rsp) {
                // 对于 OTA，我们特殊处理，在 handle_ota_control 内部或者这里处理
                 // 为了简单，我们先在这里发回响应：
                 esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
             }
            handle_ota_control(param->write.value[0]);
        }
        else if (param->write.handle == handle_table[IDX_CHAR_OTA_DATA]) {
            handle_ota_data(param->write.value, param->write.len);
            if (param->write.need_rsp) {
                 esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
            }
        }
        break;
        
     case ESP_GATTS_START_EVT:
          // 开始广播
        break;
    
    // --- 新增：连接建立事件 ---
    case ESP_GATTS_CONNECT_EVT:
        ESP_LOGI(TAG, "BLE Connected, conn_id=%d", param->connect.conn_id);
        break;

    // --- 新增：断开连接事件 ---
    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "BLE Disconnected, reason=0x%x", param->disconnect.reason);
        // 关键修复：断开后必须重新开启广播，否则设备即不可见
        esp_ble_gap_ext_adv_start(1, &(esp_ble_gap_ext_adv_t){.instance = 0, .duration = 0, .max_events = 0});
        break;

    default:
        break;
    }
}



// --- WiFi 连接相关 ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi Disconnected. Reconnecting...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// --- 主程序 ---
void app_main(void) {
    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    // 2. 初始化 PWM
    ledc_init();
    set_brightness(10); // 默认 10% 亮度

    // 3. 初始化 WiFi (简单 STA 模式)
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t wifi_config = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, },
    };
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    // 4. 初始化蓝牙
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    // 注意：ESP32-S3 只有 BLE
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_init();
    esp_bluedroid_enable();

    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gatts_register_callback(gatts_profile_event_handler);
    esp_ble_gatts_app_register(0);
}
