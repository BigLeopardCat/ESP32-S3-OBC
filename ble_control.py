import asyncio
import sys

# 尝试导入 bleak，如果没有安装则提示
try:
    from bleak import BleakScanner, BleakClient
except ImportError:
    print("Error: 需要安装 'bleak' 库。请运行: pip install bleak")
    sys.exit(1)

# --- 配置部分 ---
DEVICE_NAME = "ESP32S3_DIMMER"

# 将 16-bit UUID 转换为标准的 128-bit UUID 字符串
# 蓝牙基准 UUID: 0000xxxx-0000-1000-8000-00805F9B34FB
def uuid16_to_uuid128(uuid16_hex_str):
    return f"0000{uuid16_hex_str}-0000-1000-8000-00805F9B34FB"

CHAR_DIMMER_UUID = uuid16_to_uuid128("FF01")   # 调光
CHAR_OTA_CTRL_UUID = uuid16_to_uuid128("FF02") # OTA 控制
# CHAR_OTA_DATA_UUID = uuid16_to_uuid128("FF03") # OTA 数据 (暂不用)

async def main():
    print(f"正在扫描设备: {DEVICE_NAME} ...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10.0)

    if not device:
        print(f"未找到设备 '{DEVICE_NAME}'。请确保设备已上电且蓝牙未被其他手机连接。")
        return

    print(f"找到设备: {device.name} [{device.address}]")
    print("正在连接...")

    try:
        async with BleakClient(device) as client:
            print(f"连接成功: {client.is_connected}")

            # ---------------------------------------------------------
            # 演示 1: 发送调光指令 (例如设置亮度 80%)
            # ---------------------------------------------------------
            brightness = 80
            print(f"1. 发送亮度指令: {brightness}% (向 {CHAR_DIMMER_UUID} 写入 0x{brightness:02X})")
            # response=True 对应 ESP32 代码中的 param->write.need_rsp
            await client.write_gatt_char(CHAR_DIMMER_UUID, bytearray([brightness]), response=True)
            print("   -> 写入成功 (收到设备响应)")

            await asyncio.sleep(1) # 稍作停顿

            # ---------------------------------------------------------
            # 演示 2: 发送 WiFi OTA 触发指令
            # ---------------------------------------------------------
            ota_cmd = 0x02 # 0x02 对应代码中的 Trigger WiFi OTA
            print(f"2. 发送 WiFi OTA 指令: {ota_cmd} (向 {CHAR_OTA_CTRL_UUID} 写入 0x02)")
            
            await client.write_gatt_char(CHAR_OTA_CTRL_UUID, bytearray([ota_cmd]), response=True)
            print("   -> 写入成功 (收到设备响应)")
            print("   设备应该正在开始下载固件并重启...")
            
    except Exception as e:
        print(f"发生错误: {e}")

if __name__ == "__main__":
    asyncio.run(main())
