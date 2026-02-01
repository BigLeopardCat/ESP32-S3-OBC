import http.server
import socketserver
import os
import socket
import sys

# 配置端口
PORT = 8070
# 配置固件所在的目录 (相对于脚本运行位置)
FIRMWARE_DIR = "build"
# 固件文件名
FIRMWARE_BIN = "OBC.bin"

class VerificationHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=FIRMWARE_DIR, **kwargs)

    def log_message(self, format, *args):
        # 打印请求日志
        sys.stderr.write("%s - - [%s] %s\n" %
                         (self.client_address[0],
                          self.log_date_time_string(),
                          format%args))

def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # 不需要实际连接，只是利用这个技巧获取当前使用的网卡 IP
        s.connect(('8.8.8.8', 80))
        IP = s.getsockname()[0]
    except Exception:
        IP = '127.0.0.1'
    finally:
        s.close()
    return IP

if __name__ == "__main__":
    # 检查 build 目录是否存在
    if not os.path.exists(FIRMWARE_DIR):
        print(f"Error: 目录 '{FIRMWARE_DIR}' 不存在。请先编译项目 (idf.py build)。")
        sys.exit(1)
    
    # 检查固件是否存在
    bin_path = os.path.join(FIRMWARE_DIR, FIRMWARE_BIN)
    if not os.path.exists(bin_path):
        print(f"Warning: 固件文件 '{bin_path}' 尚未生成。请先编译项目。")

    ip_address = get_local_ip()
    url = f"http://{ip_address}:{PORT}/{FIRMWARE_BIN}"

    print("-" * 60)
    print(f"OTA Server is running on port {PORT}")
    print(f"Serving firmware from: ./{FIRMWARE_DIR}")
    print("-" * 60)
    print(f"IMPORTANT: Please update your main.c OTA_URL to:")
    print(f"           {url}")
    print("-" * 60)
    print("Press Ctrl+C to stop the server.")

    # 允许地址重用
    socketserver.TCPServer.allow_reuse_address = True
    
    with socketserver.TCPServer(("", PORT), VerificationHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nServer stopped.")
