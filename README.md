# ESP32-S3 OBC — OLED IoT 显示设备

128x64 SPI OLED 车载显示终端，接入 saudade.site IoT 平台（EMQX + 设备服务）。

> 📖 **接入协议详解、拓展指南、控制台搭配**：[docs/device-integration.md](docs/device-integration.md)
> （MQTT topic 全表、payload 字段、req_id 回执对账、OTA 流程、如何新增指令/配置项/接真实传感器）

## 功能

- **MQTT over TLS**（mqtts://saudade.site:8883，device_id + device_key 认证，证书链校验）
- **指令显示**：控制台/网页发送 `{"type":"display","text":"..."}` → OLED 实时显示（含 \n 换行、自动折行、居中）
- **指令回执（req_id 对账）**：执行后回 `cmd/ack`，`req_id` 原样带回——控制台/agent 可端到端确认指令已执行（cmd_history 全链路可查）
- **参数配置（版本化）**：控制台下发 `{"cfg_version":N,"config":{...}}` →
  应用 OLED 亮度/默认文本 → NVS 持久化（重启不丢）→ 回执 `config/ack`（带版本号）
- **遥测上报**：每 5s 上报 生效配置版本 / 固件版本 / 温度 / RSSI / 运行时长
- **在线状态**：连接即上报 online，异常掉线由遗嘱消息自动标记 offline
- **OTA 远程升级**：每 6 小时轮询平台固件仓库，发现新版本自动下载升级（A/B 分区 + esp_https_ota，current 指针支持版本切换/回滚）
- **断线自愈**：WiFi/MQTT 自动重连；上线后 retain 配置自动补发

## 硬件

| 引脚 | 功能 |
|---|---|
| GPIO36 | OLED CS |
| GPIO37 | OLED DC |
| GPIO38 | OLED RST |
| GPIO39 | OLED MOSI |
| GPIO40 | OLED CLK |

SSD1306 128x64，SPI 10MHz，Page 寻址模式。

## 配置（main/main.c 顶部「配置区」）

| 宏 | 说明 |
|---|---|
| `WIFI_SSID` / `WIFI_PASS` | WiFi |
| `DEVICE_ID` / `DEVICE_KEY` | 控制台（https://saudade.site/device-console/）注册设备后填入 |
| `APP_VERSION` | 固件版本（OTA 比对，发版时 +1，如 1.2.0） |

## 编译烧录

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## 参数配置（网页控制台下发）

```json
{
  "brightness": 200,        // OLED 亮度 0-255（SSD1306 对比度）
  "default_text": "Hello"   // 空闲显示文本（开机/无指令时显示）
}
```

设备收到后：应用 → 写入 NVS → 回执 `{"ack":true,"cfg_version":N}`。
控制台显示"已同步 vN"；设备离线时下发，上线后 retain 自动补发（配置自愈）。

## OTA 升级

1. 控制台/接口上传新固件：`PUT /api/ota/firmware?version=1.2.0`（body=固件 bin）
2. 设备每 6 小时轮询 `GET /api/ota/info`，版本高于本地 → 自动下载升级并重启
3. 升级后遥测上报新固件版本，控制台可见

## 目录

```
main/
├── main.c            # 全部逻辑（OLED / MQTT / 配置 / 遥测 / OTA）
├── saudade.site.ca.pem   # 服务器证书链（嵌入固件校验 TLS）
└── CMakeLists.txt
partitions.csv        # 4M factory + 4M ota_0 + 4M ota_1（A/B OTA）
ota_server.py         # 旧版本地 OTA 服务器（已由平台 OTA 取代，可删除）
ble_vison.c           # BLE 视觉模块（旧功能，未启用）
main.old.c            # 旧版固件备份（网页配置/本地OTA/BLE）
```

依赖：ESP-IDF v5.x（esp-mqtt、esp_http_client、esp_https_ota 均为内置组件）。
