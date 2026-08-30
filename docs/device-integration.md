# ESP32-S3-OBC 设备接入指南

> 本固件是 saudade.site IoT 平台的参考设备实现：128x64 OLED 显示终端，
> 通过 MQTT over TLS 接入平台，接收指令显示、上报遥测、接收参数配置与 OTA 升级。
> 配套文档：[平台接入指南（主仓库 docs/iot-device-integration.md）](../../memory_blog_rust/docs/iot-device-integration.md)
> ——协议的全量定义（MQTT topic、REST API、JWT 鉴权）以平台侧文档为准，本文讲固件侧怎么接。

---

## 1. 接入方式总览

```
ESP32-S3 (main.c)
   │  WiFi STA
   ▼
mqtts://saudade.site:8883 (EMQX, TLS 证书链校验)
   │  user = DEVICE_ID / pass = DEVICE_KEY
   ▼
device-service (:3100) ── 控制台下发指令/配置/OTA
```

三条通道，职责分离：

| 通道 | 协议 | 用途 |
|---|---|---|
| MQTT | `mqtts://saudade.site:8883` | 指令（cmd）、参数配置（config）、遥测上报、在线状态 |
| HTTPS REST | `https://saudade.site/device-api/api/...` | OTA 固件拉取（HTTP Basic 认证） |
| 网页控制台 | `https://saudade.site/device-console/` | 人机交互入口（注册/下发/监控） |

## 2. MQTT 协议（固件侧实现）

### 2.1 连接与鉴权

- **broker**：`mqtts://saudade.site:8883`（EMQX，MQTT over TLS，端口 8883）
- **认证**：用户名 = `DEVICE_ID`，密码 = `DEVICE_KEY`（控制台注册设备时返回的一对凭据）
- **TLS**：校验服务器证书链——`saudade.site.ca.pem` 经 `EMBED_TXTFILES` 嵌入固件，
  在 `mqtt_start()` 中配置 `esp_mqtt_client_config_t.verification.certificate`
- **keepalive**：60s。**没有独立心跳消息**——在线状态靠 5s 遥测持续上报 + 遗嘱兜底

### 2.2 Topic 清单

所有 topic 均带 `devices/<DEVICE_ID>/` 前缀：

| 方向 | Topic | QoS | retain | 说明 |
|---|---|---|---|---|
| 发布 | `devices/<id>/status` = `"online"` | 1 | 1 | 连接成功立即发；retain 让平台持久感知在线 |
| 发布（遗嘱） | `devices/<id>/status` = `"offline"` | 1 | 1 | 异常掉线由 last will 自动补发，覆盖 retain |
| 发布 | `devices/<id>/telemetry` | 1 | 0 | 每 5s 遥测（见 §2.3） |
| 订阅 | `devices/<id>/config` | 1 | - | 参数配置下发（见 §2.4） |
| 订阅 | `devices/<id>/cmd` | 1 | - | 指令下发（见 §2.5） |
| 发布 | `devices/<id>/config/ack` | 1 | 0 | 配置回执，带 `cfg_version` |
| 发布 | `devices/<id>/cmd/ack` | 1 | 0 | 指令回执，原样带回 `req_id` |

### 2.3 遥测上报（上行）

每 5s（`TELEMETRY_MS`）发布一次：

```json
{
  "cfg_version": 2,
  "firmware": "1.1.0",
  "temperature": 25.3,
  "rssi": -45,
  "uptime": 12345,
  "brightness": 200
}
```

| 字段 | 说明 |
|---|---|
| `cfg_version` | 当前生效的配置版本（与平台对账，判断配置是否已同步） |
| `firmware` | 固件版本（OTA 后平台以此确认升级成功） |
| `temperature` | 温度，**当前为 `rand()` 模拟值**（示例占位，接真实传感器时替换，见 §4.3） |
| `rssi` | WiFi 信号强度 dBm |
| `uptime` | 运行秒数 |
| `brightness` | 当前 OLED 亮度 |

### 2.4 参数配置（下行 + 回执）

平台下发（retain，设备上线即收到最新版）：

```json
{"cfg_version": 2, "config": {"brightness": 200, "default_text": "Hello"}}
```

设备处理流程（`mqtt_event_handler` → `apply_config` → `config_save`）：

1. 解析 `cfg_version` 与 `config` 对象
2. 应用：`brightness` → SSD1306 对比度（0x81 指令）；`default_text` → 空闲显示文本
3. NVS 持久化（命名空间 `obc_cfg`：`version`/`bright`/`dtext`），重启不丢
4. 回执 `devices/<id>/config/ack`：

```json
{"ack": true, "cfg_version": 2}
```

控制台据此显示"已同步 v2"；设备离线时下发，retain 保证上线后自动补发（配置自愈）。

### 2.5 指令（下行 + req_id 回执）

平台下发：

```json
{"type": "display", "text": "Hello\n喵", "req_id": "a1b2c3"}
```

设备处理（`is_cmd` 分支——`strstr(topic, "/cmd")` 区分指令/配置）：

1. 解析 `type`：目前仅 `display`（OLED 显示）
2. `text` 支持 `\n` 换行、UTF-8 自动折行（不切汉字）、居中显示，最多 4 行（`MAX_TEXT_LINES`）
3. 回执 `devices/<id>/cmd/ack`——**`req_id` 原样带回**（设备不生成，只回显）：

```json
{"ack": true, "type": "display", "req_id": "a1b2c3"}
```

`req_id` 端到端链路对账（commit 600fbf0 起）：控制台/agent 下发带 `req_id` → 设备回执带回 →
device-service 写入 `cmd_history`——任何一环丢失都能定位（设备没收到 / 设备没回执 / 回执丢失）。
旧版/手动下发无 `req_id` 时回执不带该字段（兼容）。

### 2.6 在线状态机

```
连接成功 → publish "online" (retain)
  ├─ 正常流程：持续遥测，平台侧 rssi/uptime 实时更新
  └─ 异常掉线 → last will 自动 publish "offline" (retain) → 平台感知离线
```

平台侧还有"幽灵在线窗口"防护（设备断网但 broker 未感知期间，device-service 靠
`is_online` 检查 + 心跳窗口判定，见平台文档 §问题记录 2.1）。

## 3. OTA 升级

1. **轮询**：`ota_task` 启动 15s 后首查，之后每 6h（`OTA_CHECK_MS`）——
   `GET https://saudade.site/device-api/api/ota/info`，HTTP Basic 认证（device_id/device_key）
2. **比对**：响应 `{"version":"1.2.0"}` 与本地 `APP_VERSION` 不一致即刷（支持升/降级）
3. **拉取**：`GET .../api/ota/fw/current.bin`（`current` 是平台侧指针，可切换版本，支持回滚）
4. **烧写**：`esp_https_ota` 下载写入 A/B 分区（partitions.csv：4M factory + 4M ota_0 + 4M ota_1），
   重启后新固件生效，遥测上报新 `firmware` 版本
5. **发版流程**：控制台 `PUT /api/ota/firmware?version=<新版本>` 上传 bin → 平台切 `current` 指针 →
   设备下次轮询自动升级

## 4. 拓展指南

### 4.1 新增指令类型

在 `mqtt_event_handler` 的 `is_cmd` 分支加 `type` 判断：

```c
if (is_cmd) {
    const char *type = cJSON_GetObjectItemCaseSensitive(root, "type")->valuestring;
    if (strcmp(type, "display") == 0) {
        // 现有显示逻辑
    } else if (strcmp(type, "buzzer") == 0) {
        // 新增指令：解析参数 → 执行 → 回执（带 req_id，保持链路对账）
    }
}
```

约定：任何指令执行后都要回 `cmd/ack`（含 `req_id`），否则平台 `cmd_history` 看不到回执。

### 4.2 新增配置项

1. `apply_config()` 加 `cJSON_GetObjectItemCaseSensitive` 解析新字段
2. NVS 新键（`config_load()`/`config_save()` 同步读写）
3. 平台侧控制台配置面板加字段——**cfg_version 递增**，设备端收到新版本号才会应用

### 4.3 接入真实传感器

替换 `publish_telemetry()` 里的模拟值：

```c
// 温度传感器（如 DS18B20）：
cJSON_AddNumberToObject(root, "temperature", read_ds18b20());
```

平台侧不改任何东西——遥测字段透传展示。新增字段同理，直接往 JSON 里加，
控制台/device-service 按需消费。

### 4.4 新增下行订阅

`MQTT_EVENT_CONNECTED` 里加 `esp_mqtt_client_subscribe(client, "devices/<id>/xxx", 1)`，
处理逻辑挂在 `MQTT_EVENT_DATA` 的 topic 分支。

## 5. 搭配物联网控制台

`https://saudade.site/device-console/`（复用博客登录态，无二次登录）：

| 控制台功能 | 对应设备行为 |
|---|---|
| 设备列表 / 在线状态 | `status` retain + 遥测心跳 |
| 下发显示指令（输入文字 + 发送） | `cmd` topic → OLED 显示 → `cmd/ack` 回执（控制台显示 ✓） |
| 参数配置（亮度/默认文本） | `config` topic → 应用 + NVS → `config/ack` 回执（显示"已同步 vN"） |
| 遥测图表（温度/RSSI/运行时长） | `telemetry` topic 5s 上报 |
| 固件管理（上传 bin / 切版本） | OTA 轮询自动升级 |
| 指令历史（cmd_history） | `cmd/ack` 带 `req_id` 对账 |

**典型联调路径**：注册设备拿到 `DEVICE_ID/DEVICE_KEY` → 填入 `main.c` 配置区 →
烧录 → 控制台看到设备上线 → 下发一条显示指令 → OLED 显示 + 控制台回执 ✓ →
下发配置 → 设备亮度变化 + 控制台"已同步" → 全程 `cmd_history` 可查。

## 6. 配置区速查（main/main.c 顶部）

| 宏 | 默认值 | 说明 |
|---|---|---|
| `WIFI_SSID` / `WIFI_PASS` | 硬编码 | WiFi 凭据 |
| `MQTT_URI` | `mqtts://saudade.site:8883` | broker |
| `DEVICE_ID` / `DEVICE_KEY` | 控制台注册返回值 | MQTT 用户名/密码 + OTA Basic 认证 |
| `APP_VERSION` | `1.1.0` | 固件版本（OTA 比对，发版手动 +1） |
| `OTA_CHECK_MS` | 6h | OTA 轮询间隔 |
| `TELEMETRY_MS` | 5000ms | 遥测间隔 |
