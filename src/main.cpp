/*
  ESP8266 无线串口调试器 (Web版)
  
  功能:
  - ESP8266 作为 SoftAP (热点模式)
  - TCP 服务器 (端口 80) 提供 Web 页面
  - Web 页面包含:
    * 波特率设置下拉框 (支持标准波特率)
    * 接收数据显示文本区域 (自动滚动)
    * 发送数据可编辑文本框
    * 开始监听 / 停止监听 按钮 (控制串口接收是否显示)
    * 发送按钮 (将文本框内容通过串口发送)
  - 通过 WebSocket 实现实时双向通信: 
      串口收到数据 -> 发送到网页
      网页点击发送 -> 数据通过串口发送
      更改波特率 -> 重新配置硬件串口

  硬件连接:
  - ESP8266 的 TX (GPIO1) 连接外部设备的 RX
  - ESP8266 的 RX (GPIO3) 连接外部设备的 TX
  - 外部设备与 ESP8266 共地

  使用说明:
  1. 上传代码到 ESP8266
  2. 上电后 ESP8266 会创建热点: SSID = "SerialMonitor", 密码 = "12345678"
  3. 用手机或电脑连接该 WiFi
  4. 浏览器访问 http://192.168.4.1
  5. 在网页上设置波特率, 点击"开始监听", 即可看到串口接收数据, 并可发送数据
*/


//#define DEBUG 1

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <SoftwareSerial.h>

// 热点配置
const char ssid[] = "SerialMonitor";
const char password[] = "12345678";

// IP地址固定
IPAddress local_ip(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

// 创建 WebServer 和 WebSocket 服务器
ESP8266WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// 硬件串口 (UART0) 用于与外部设备通信
// 注意: ESP8266 的 Serial (UART0) 默认用于调试输出, 但我们可以重新配置它用于通信
// 为了不影响启动时的串口打印, 我们在 setup() 后期再重新配置 Serial
// 实际上, ESP8266 的 Serial 和 Serial1 都可以使用, 这里直接使用 Serial (TX=GPIO1, RX=GPIO3)

// 全局变量
bool serialMonitorEnabled = false;   // 是否将串口接收的数据通过 WebSocket 发送
unsigned long currentBaud = 115200;  // 当前波特率

// 函数声明
void handleRoot();
void handleNotFound();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);
void updateSerialBaud(unsigned long baud);

// HTML 网页 (包含 CSS, JavaScript, WebSocket 客户端)
//PROGMEM = R"()"结构内的文本将以原始形态储存在flash中，html代码与../web.html中的一致
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=yes">
    <title>ESP8266 无线串口调试器</title>
    <style>
        * {
            box-sizing: border-box;
        }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: #1e1e2f;
            color: #eee;
            margin: 0;
            padding: 16px;
        }
        .container {
            max-width: 1000px;
            margin: 0 auto;
            background: #2d2d3a;
            border-radius: 16px;
            padding: 20px;
            box-shadow: 0 8px 20px rgba(0,0,0,0.3);
        }
        h1 {
            text-align: center;
            font-size: 1.8rem;
            margin-top: 0;
            margin-bottom: 20px;
            color: #ffaa44;
        }
        .control-bar {
            display: flex;
            flex-wrap: wrap;
            gap: 12px;
            align-items: center;
            margin-bottom: 20px;
            background: #3a3a4a;
            padding: 12px 16px;
            border-radius: 12px;
        }
        .control-group {
            display: flex;
            align-items: center;
            gap: 8px;
            background: #2d2d3a;
            padding: 5px 12px;
            border-radius: 24px;
        }
        .control-group label {
            font-weight: bold;
            font-size: 0.9rem;
        }
        select, button {
            background: #4a4a5a;
            border: none;
            color: white;
            padding: 8px 16px;
            border-radius: 24px;
            font-size: 0.9rem;
            cursor: pointer;
            transition: 0.2s;
        }
        select:hover, button:hover {
            background: #6a6a7a;
        }
        button:active {
            transform: scale(0.97);
        }
        button.stop {
            background: #8b3c3c;
        }
        button.stop:hover {
            background: #a54c4c;
        }
        button.start {
            background: #3c6e47;
        }
        button.start:hover {
            background: #4e8a5c;
        }
        .data-area {
            margin: 16px 0;
        }
        .data-label {
            font-weight: bold;
            margin-bottom: 8px;
            display: block;
        }
        textarea {
            width: 100%;
            background: #1e1e2a;
            color: #0f0;
            font-family: 'Courier New', monospace;
            font-size: 14px;
            border: 1px solid #4a4a5a;
            border-radius: 12px;
            padding: 12px;
            resize: vertical;
        }
        #receivedData {
            height: 300px;
            overflow-y: auto;
            white-space: pre-wrap;
            word-wrap: break-word;
        }
        #sendData {
            height: 100px;
            margin-bottom: 12px;
        }
        .send-button {
            background: #2c6e9e;
            width: 100%;
            padding: 12px;
            font-size: 1.1rem;
            font-weight: bold;
        }
        .status {
            margin-top: 16px;
            text-align: center;
            font-size: 0.85rem;
            color: #aaa;
            border-top: 1px solid #4a4a5a;
            padding-top: 12px;
        }
        .footer {
            font-size: 12px;
            text-align: center;
            margin-top: 16px;
            color: #888;
        }
        @media (max-width: 600px) {
            .control-bar {
                flex-direction: column;
                align-items: stretch;
            }
            .control-group {
                justify-content: space-between;
            }
        }
    </style>
</head>
<body>
<div class="container">
    <h1>📡 无线串口调试器</h1>
    <div class="control-bar">
        <div class="control-group">
            <label>⚙️ 波特率:</label>
            <select id="baudRate">
                <option value="300">300</option>
                <option value="600">600</option>
                <option value="1200">1200</option>
                <option value="2400">2400</option>
                <option value="4800">4800</option>
                <option value="9600">9600</option>
                <option value="14400">14400</option>
                <option value="19200">19200</option>
                <option value="28800">28800</option>
                <option value="38400">38400</option>
                <option value="57600">57600</option>
                <option value="115200" selected>115200</option>
                <option value="230400">230400</option>
                <option value="460800">460800</option>
                <option value="921600">921600</option>
            </select>
        </div>
        <div class="control-group">
            <label>🎛️ 监听:</label>
            <button id="btnStart" class="start">▶ 开始监听</button>
            <button id="btnStop" class="stop">⏹️ 停止监听</button>
        </div>
        <div class="control-group">
            <span id="wsStatus" style="color:#ffaa44;">● 未连接</span>
        </div>
    </div>

    <div class="data-area">
        <div class="data-label">📥 接收数据 (来自串口)</div>
        <textarea id="receivedData" readonly placeholder="点击「开始监听」后，这里会显示收到的串口数据..."></textarea>
    </div>

    <div class="data-area">
        <div class="data-label">📤 发送数据 (通过串口发送)</div>
        <textarea id="sendData" placeholder="输入要发送的文本或十六进制数据..."></textarea>
        <button id="btnSend" class="send-button">✉️ 发送数据</button>
    </div>
    <div class="status">
        💡 提示: 需要先点击「开始监听」才能看到接收数据。发送按钮不受监听状态影响。
        <br>确保外部设备与 ESP8266 的 TX/RX 交叉连接并共地。
    </div>
    <div class="footer">
        ESP8266 无线串口调试器 | 热点: <strong>SerialMonitor</strong> 密码: <strong>12345678</strong>
    </div>
</div>

<script>
    let ws = null;
    let isWebSocketOpen = false;
    const receivedTextarea = document.getElementById('receivedData');
    const sendTextarea = document.getElementById('sendData');
    const baudSelect = document.getElementById('baudRate');
    const btnStart = document.getElementById('btnStart');
    const btnStop = document.getElementById('btnStop');
    const btnSend = document.getElementById('btnSend');
    const wsStatusSpan = document.getElementById('wsStatus');

    function connectWebSocket() {
        const host = window.location.hostname;
        const wsUrl = `ws://${host}:81`;
        ws = new WebSocket(wsUrl);
        
        ws.onopen = function() {
            console.log('WebSocket 连接成功');
            isWebSocketOpen = true;
            wsStatusSpan.innerHTML = '● 已连接';
            wsStatusSpan.style.color = '#88ff88';
        };
        
        ws.onmessage = function(event) {
            // 接收来自 ESP8266 串口的数据
            let data = event.data;
            // 追加到接收文本框
            receivedTextarea.value += data;
            // 自动滚动到底部
            receivedTextarea.scrollTop = receivedTextarea.scrollHeight;
        };
        
        ws.onerror = function(error) {
            console.error('WebSocket 错误:', error);
            wsStatusSpan.innerHTML = '⚠️ 连接错误';
            wsStatusSpan.style.color = '#ff8888';
        };
        
        ws.onclose = function() {
            console.log('WebSocket 断开');
            isWebSocketOpen = false;
            wsStatusSpan.innerHTML = '● 未连接';
            wsStatusSpan.style.color = '#ffaa44';
            // 尝试重连
            setTimeout(connectWebSocket, 2000);
        };
    }

    // 发送数据到 ESP8266 (通过串口发送到外部设备)
    function sendSerialData() {
        if (!isWebSocketOpen) {
            alert('WebSocket 未连接, 请刷新页面');
            return;
        }
        let data = sendTextarea.value;
        if (data.length === 0) return;
        // 通过 WebSocket 发送, 服务端收到后会通过串口发出
        ws.send(data);
        // 可选: 在接收区也回显一下发送的内容 (加个标记)
        // 但为了模拟真实串口调试器, 暂时不自动回显, 如果外部设备会回传数据, 则能显示
        // 为了方便, 可以在本地增加一个本地回显 (注释掉, 更真实)
        // 如果想本地回显发送内容, 可以取消下面注释:
        // receivedTextarea.value += "[SENT] " + data + "\n";
        // receivedTextarea.scrollTop = receivedTextarea.scrollHeight;
    }

    // 设置波特率
    function setBaudRate() {
        if (!isWebSocketOpen) {
            alert('WebSocket 未连接, 请等待连接成功后再修改波特率');
            return;
        }
        let baud = baudSelect.value;
        // 通过 WebSocket 发送特殊指令: "BAUD:xxxxx"
        ws.send(`BAUD:${baud}`);
    }

    // 开始监听: 告诉 ESP8266 将串口收到的数据转发到 WebSocket
    function startMonitor() {
        if (!isWebSocketOpen) {
            alert('WebSocket 未连接');
            return;
        }
        ws.send("CMD:START_MONITOR");
        // 界面反馈: 清除旧数据 (可选)
        // receivedTextarea.value = "";
    }

    // 停止监听
    function stopMonitor() {
        if (!isWebSocketOpen) {
            alert('WebSocket 未连接');
            return;
        }
        ws.send("CMD:STOP_MONITOR");
    }

    // 绑定事件
    btnSend.addEventListener('click', sendSerialData);
    btnStart.addEventListener('click', startMonitor);
    btnStop.addEventListener('click', stopMonitor);
    baudSelect.addEventListener('change', setBaudRate);

    // 页面加载后连接 WebSocket
    connectWebSocket();
    
    // 支持 Ctrl+Enter 快速发送
    sendTextarea.addEventListener('keydown', function(e) {
        if (e.ctrlKey && e.key === 'Enter') {
            e.preventDefault();
            sendSerialData();
        }
    });
</script>
</body>
</html>
)rawliteral";

// 处理根目录请求
void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleNotFound() {
  server.send(404, "text/plain", "404: Not Found");
}

// WebSocket 事件处理
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      #ifdef DEBUG 
        Serial.printf("[WebSocket] 客户端 %u 断开\n", num); 
      #endif
      break;
    case WStype_CONNECTED:
      {
    #ifdef DEBUG
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[WebSocket] 客户端 %u 连接, IP: %s\n", num, ip.toString().c_str());
        // 连接成功后, 发送当前状态: 暂时不自动发送, 等待用户指令
    #endif
      }
      break;
    case WStype_TEXT:
      {
        String msg = String((char*)payload);
        // 解析特殊指令
        if (msg.startsWith("CMD:START_MONITOR")) {
          serialMonitorEnabled = true;
          #ifdef DEBUG
          Serial.println("[命令] 开始监听串口, 将转发数据到 WebSocket");
          #endif
          // 可选: 给网页发送一个确认消息
          webSocket.sendTXT(num, "\n[info]监听已开启\n");
        }
        else if (msg.startsWith("CMD:STOP_MONITOR")) {
          serialMonitorEnabled = false;
          #ifdef DEBUG
          Serial.println("[命令] 停止监听串口");
          #endif
          webSocket.sendTXT(num, "\n[info]监听已停止\n");
        }
        else if (msg.startsWith("BAUD:")) {
          unsigned long newBaud = msg.substring(5).toInt();
          if (newBaud > 0) {
            currentBaud = newBaud;
            updateSerialBaud(currentBaud);
            #ifdef DEBUG
            Serial.printf("[命令] 更改波特率为 %lu\n", currentBaud);
            #endif
            String message = "\n[info]波特率已更改为 " + String(currentBaud)+"\n";
            webSocket.sendTXT(num, message);
          }
        }
        else {
          // 普通文本: 通过串口发送出去
          if (Serial) {
            Serial.print(msg);
            #ifdef DEBUG
            Serial.printf("[串口发送] %s\n", msg.c_str());
            #endif
          } else {
            #ifdef DEBUG
            Serial.println("[错误] 串口未初始化");
            #endif
          }
        }
      }
      break;
    default:
      break;
  }
}

// 重新配置硬件串口波特率
void updateSerialBaud(unsigned long baud) {
  // 结束当前串口配置
  Serial.end();
  delay(50);
  // 重新开启串口, 使用指定的波特率
  Serial.begin(baud);
  delay(50);
  // 可选: 清空缓冲区
  while(Serial.available()) Serial.read();
  Serial.printf("[SYS] Serial BAUD= %lu\n", baud);
}

void setup() {
  // 首先使用默认波特率用于调试输出 (启动时信息)
  Serial.begin(115200);
  delay(2000);//避免上电时的默认输出打乱软件打印的log
  // 读取并丢弃所有缓冲的启动信息
  while(Serial.available()) {
    Serial.read();
  }
  #ifdef DEBUG
  Serial.println();
  Serial.println("\n\n===================================");
  Serial.println("ESP8266 wireless Serial Mointor ...");
  Serial.println("===================================");
  #endif
  // 配置 SoftAP
  WiFi.softAPConfig(local_ip, gateway, subnet);
  bool apOk = WiFi.softAP(ssid, password);
  if (apOk) {
    #ifdef DEBUG
    Serial.println("hot spot setup SUCCESS");
    Serial.print("SSID:");Serial.println(ssid);
    Serial.print("PassWords:");Serial.println(password);
    Serial.print("IP:");
    Serial.println(WiFi.softAPIP());
    #endif
  } else {
    #ifdef DEBUG
    Serial.println("ERROR:hot spot setup FAILE");
    #endif
  }

  // 配置 Web 服务器
  server.on("/", handleRoot);
  server.onNotFound(handleNotFound);
  server.begin();
  #ifdef DEBUG
  Serial.println("HTTP Server start,oprt:80");
  #endif 

  // 配置 WebSocket 服务器
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  #ifdef DEBUG
  Serial.println("WebSocket Server start,port:81");
  #endif

  serialMonitorEnabled = false;  // 初始不监听, 需网页点击开始
  #ifdef DEBUG
  Serial.println("setup SUCCESS link and visit http://192.168.4.1");
  #endif
}

void loop() {
  // 处理 HTTP 请求
  server.handleClient();
  // 处理 WebSocket 消息
  webSocket.loop();

  // 如果监听模式开启, 且串口有数据, 则通过 WebSocket 广播给所有客户端
  if (serialMonitorEnabled) {
    if (Serial.available()) {
      // 读取一行或按字节读取均可, 为了实时性, 我们读取所有可用数据并发送
      // 由于 WebSocket 发送文本最好一次发送一段, 可以累积一下缓冲区
      // 简单实现: 读取所有字节, 组合成字符串发送
      String data = "";
      while (Serial.available()) {
        char c = Serial.read();
        data += c;
        // 为避免过长字符串阻塞, 可设置最大长度, 但一般串口数据不会太爆发
        if (data.length() > 512) break;
      }
      if (data.length() > 0) {
        // 广播给所有连接的 WebSocket 客户端
        webSocket.broadcastTXT(data);
        // 可选: 也输出到串口监视器 (用于调试)
        // Serial.print("[接收转发] ");
        // Serial.print(data);
      }
    }
  }
}
