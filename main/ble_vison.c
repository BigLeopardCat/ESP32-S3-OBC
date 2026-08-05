#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"

#define GATTS_TAG "VISON_BLE"

static uint8_t vison_service_uuid128[16] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12,
};

static uint8_t vison_char_uuid128[16] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    // 00 00 21 43 65 87 21 43 65 87 21 43 21 43 65 87
    0x00, 0x00, 0x21, 0x43, 0x65, 0x87, 0x21, 0x43, 0x65, 0x87, 0x21, 0x43, 0x21, 0x43, 0x65, 0x87
};

static uint16_t vison_handle_table[3]; // SVC, CHAR, VAL
static uint16_t vison_cccd_handle = 0;

static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data =  NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(vison_service_uuid128),
    .p_service_uuid = vison_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = false,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data =  NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(vison_service_uuid128), // UUID goes in Scan Response
    .p_service_uuid = vison_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static uint16_t gatts_if_curr = ESP_GATT_IF_NONE;
static uint16_t conn_id_curr = 0;
static volatile bool is_connected = false;
static volatile bool is_gatt_ready = false;
static volatile bool is_notify_enabled = false;

static void start_ble_advertising_if_ready(void) {
    if (is_gatt_ready) {
        esp_ble_gap_start_advertising(&adv_params);
    }
}

static void vison_notify_task(void *arg) {
    const char *orientations[] = {"0 100", "1 400", "2 880", "3 270"};
    int idx = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (is_connected && gatts_if_curr != ESP_GATT_IF_NONE) {
            const char *payload = orientations[idx];
            esp_err_t ret = esp_ble_gatts_send_indicate(gatts_if_curr, conn_id_curr, vison_handle_table[2],
                                                         strlen(payload), (uint8_t *)payload, false);
            if (ret == ESP_OK) {
                ESP_LOGI(GATTS_TAG, "Sent %s", payload);
                idx = (idx + 1) % 4;
            } else {
                ESP_LOGE(GATTS_TAG, "send_indicate failed, ret=%d", ret);
            }
        }
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        start_ble_advertising_if_ready();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(GATTS_TAG, "Advertising start failed");
        }
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        /* send the positive (true) security response to the peer device to accept the security request */
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(GATTS_TAG, "Pairing success!");
        } else {
            ESP_LOGE(GATTS_TAG, "Pairing failed! reason: 0x%x", param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    default:
        break;
    }
}

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    switch (event) {
    case ESP_GATTS_REG_EVT:
        esp_ble_gap_set_device_name("VISON_SENSOR");
        esp_ble_gap_config_adv_data(&adv_data);
        esp_ble_gap_config_adv_data(&scan_rsp_data);
        
        esp_gatt_srvc_id_t service_id;
        service_id.is_primary = true;
        service_id.id.inst_id = 0x00;
        service_id.id.uuid.len = ESP_UUID_LEN_128;
        memcpy(service_id.id.uuid.uuid.uuid128, vison_service_uuid128, ESP_UUID_LEN_128);
        
        esp_ble_gatts_create_service(gatts_if, &service_id, 4);
        break;
    case ESP_GATTS_CREATE_EVT:
        vison_handle_table[0] = param->create.service_handle;
        ESP_LOGI(GATTS_TAG, "Service created, handle=%u", vison_handle_table[0]);
        
        esp_bt_uuid_t char_uuid;
        char_uuid.len = ESP_UUID_LEN_128;
        memcpy(char_uuid.uuid.uuid128, vison_char_uuid128, ESP_UUID_LEN_128);
        
        esp_err_t add_char_ret = esp_ble_gatts_add_char(vison_handle_table[0], &char_uuid,
                               ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                               ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_READ,
                               NULL, NULL);
        ESP_LOGI(GATTS_TAG, "Add char ret=%d", add_char_ret);
        break;
    case ESP_GATTS_ADD_CHAR_EVT:
        vison_handle_table[1] = param->add_char.attr_handle;
        vison_handle_table[2] = param->add_char.attr_handle; // Store char val handle
        ESP_LOGI(GATTS_TAG, "Char added, value handle=%u", vison_handle_table[2]);

        // 先把服务启动起来，让客户端尽快能枚举到这个特征。
        esp_ble_gatts_start_service(vison_handle_table[0]);
        
        // Characteristic Descriptor (Client Characteristic Configuration) for Notify
        esp_bt_uuid_t desc_uuid;
        desc_uuid.len = ESP_UUID_LEN_16;
        desc_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
        esp_err_t add_descr_ret = esp_ble_gatts_add_char_descr(vison_handle_table[0], &desc_uuid,
                                     ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                     NULL, NULL);
        ESP_LOGI(GATTS_TAG, "Add char descr ret=%d", add_descr_ret);
        break;
    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        ESP_LOGI(GATTS_TAG, "Char descr added, starting service");
        vison_cccd_handle = param->add_char_descr.attr_handle;
        ESP_LOGI(GATTS_TAG, "CCCD handle=%u", vison_cccd_handle);
        gatts_if_curr = gatts_if;
        is_gatt_ready = true;
        esp_ble_gap_start_advertising(&adv_params);
        break;
    case ESP_GATTS_WRITE_EVT:
        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
        }
        if (param->write.handle == vison_cccd_handle && param->write.len >= 2) {
            uint16_t cccd = param->write.value[1] << 8 | param->write.value[0];
            is_notify_enabled = (cccd & 0x0001) != 0;
            ESP_LOGI(GATTS_TAG, "CCCD write, notify=%d, raw=0x%04x", is_notify_enabled, cccd);
        }
        break;
    case ESP_GATTS_CONNECT_EVT:
        ESP_LOGI(GATTS_TAG, "ESP_GATTS_CONNECT_EVT, conn_id %d", param->connect.conn_id);
        conn_id_curr = param->connect.conn_id;
        gatts_if_curr = gatts_if;
        is_connected = true;
        is_notify_enabled = false;
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(GATTS_TAG, "ESP_GATTS_DISCONNECT_EVT, reason 0x%x", param->disconnect.reason);
        is_connected = false;
        is_notify_enabled = false;
        start_ble_advertising_if_ready();
us %d", param->reg.app_id, param->reg.status);
            return;
        }
    }
    gatts_profile_event_handler(event, gatts_if, param);
}

void ble_vison_init(void) {
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_init();
    esp_bluedroid_enable();
    
    esp_ble_gatts_register_callback(gatts_event_handler);
    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gatts_app_register(0);
    
    // 修复SMP配对断开问题：我们直接使用Just Works模式跳过密钥验证
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req,     // 不要求绑定，避免 Windows 端旧的配对记录影响后续连接/发现
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_NO_BOND;
   esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    xTaskCreate(vison_notify_task, "vison_notify", 4096, NULL, 5, NULL);
}
