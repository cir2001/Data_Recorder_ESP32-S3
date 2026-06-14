#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <Wire.h>

// ==========================================
// 1. 硬件引脚与网络配置
// ==========================================
const char* ssid = "YOUR_WIFI_SSID";       // 替换为你的 Wi-Fi 名称
const char* password = "YOUR_WIFI_PASS";   // 替换为你的 Wi-Fi 密码
const char* udpAddress = "192.168.1.100";  // 替换为上位机电脑的局域网 IP
const int udpPort = 8080;                  // 替换为上位机监听的 UDP 端口

const int LED_SYS  = 15;  
const int LED_STAT = 16; 
const int I2C_SDA  = 1;   
const int I2C_SCL  = 2;   
const int SD_CS    = 10; 
const int SD_MOSI  = 11; 
const int SD_SCK   = 12; 
const int SD_MISO  = 13; 
const int UART_RX  = 18;  // H743 TX 连接至此
const int UART_TX  = 17;  // H743 RX 连接至此

// ==========================================
// 2. 核心数据结构与全局变量
// ==========================================
#define CHUNK_SIZE 512  // 每次搬运的最大数据块大小 (字节)

typedef struct {
  uint16_t length;             // 当前块实际装了多少字节
  uint8_t payload[CHUNK_SIZE]; // 纯净的字节流容器
} LogChunk_t;

QueueHandle_t queueSD;
QueueHandle_t queueWiFi;

WiFiUDP udp;
// U8G2_SH1106_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/ I2C_SCL, /* data=*/ I2C_SDA, /* reset=*/ U8X8_PIN_NONE);
// 换成 HW_I2C（硬件 I2C）
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

bool sdCardReady = false;
uint32_t totalBytesWritten = 0; // 用于 OLED 显示写入量

// ==========================================
// 3. 任务 A：Wi-Fi UDP 发送 (跑在 Core 0)
// ==========================================
void TaskWiFiUpload(void *pvParameters) {
  Serial.printf("🌐 TaskWiFi running on Core %d\n", xPortGetCoreID());

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
  Serial.println("✅ Wi-Fi Connected!");

  LogChunk_t txChunk;
  for (;;) {
    // 阻塞等待邮筒里的数据
    if (xQueueReceive(queueWiFi, &txChunk, portMAX_DELAY) == pdPASS) {
      udp.beginPacket(udpAddress, udpPort);
      // 原汁原味直接发送字节流
      udp.write(txChunk.payload, txChunk.length);
      udp.endPacket();
    }
  }
}

// ==========================================
// 4. 任务 B：TF 卡记录与切割 (跑在 Core 1)
// ==========================================
void TaskSDLog(void *pvParameters) {
  Serial.printf("💾 TaskSD running on Core %d\n", xPortGetCoreID());

  File dataFile;
  char fileName[32];
  int fileIndex = 0;
  uint32_t currentFileSize = 0;
  const uint32_t MAX_FILE_SIZE = 5 * 1024 * 1024; // 5MB 切割一次
  uint32_t flushCounter = 0;

  // 辅助函数：寻找下一个文件名并打开
  auto openNextFile = [&]() {
    if (dataFile) dataFile.close();
    while (true) {
      snprintf(fileName, sizeof(fileName), "/log_%03d.bin", fileIndex);
      if (!SD.exists(fileName)) break;
      fileIndex++;
    }
    dataFile = SD.open(fileName, FILE_WRITE);
    currentFileSize = 0;
    Serial.printf("📂 Opened new log: %s\n", fileName);
  };

  if (sdCardReady) {
    openNextFile();
  }

  LogChunk_t rxChunk;
  for (;;) {
    // 阻塞等待数据
    if (xQueueReceive(queueSD, &rxChunk, portMAX_DELAY) == pdPASS) {
      if (dataFile) {
        // 直接写入纯字节流
        dataFile.write(rxChunk.payload, rxChunk.length);
        
        currentFileSize += rxChunk.length;
        totalBytesWritten += rxChunk.length;
        flushCounter += rxChunk.length;

        // 积攒约 50KB 强制落盘一次，避免频繁 flush 卡顿
        if (flushCounter >= 50000) {
          dataFile.flush();
          flushCounter = 0;
        }

        // 文件大小达到 5MB 时自动切割
        if (currentFileSize >= MAX_FILE_SIZE) {
          openNextFile();
        }
      }
    }
  }
}

// ==========================================
// 5. Setup 初始化
// ==========================================
void setup() {
  Serial.begin(115200);
  
  // 🚨 终极防护：将硬件 RX 缓冲区开到 16KB，防止 OLED 刷新期间漏数据！
  Serial1.setRxBufferSize(16384);
  Serial1.begin(921600, SERIAL_8N1, UART_RX, UART_TX);

  pinMode(LED_SYS, OUTPUT);
  pinMode(LED_STAT, OUTPUT);
  digitalWrite(LED_SYS, HIGH);

  // 初始化 OLED
  // 将硬件 I2C 的 SDA 绑定到 GPIO1，SCL 绑定到 GPIO2，并设置 400kHz 极速模式
  Wire.begin(I2C_SDA, I2C_SCL, 400000); 
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr); 
  u8g2.drawStr(5, 15, "System Booting...");
  u8g2.sendBuffer();

  // 初始化 TF 卡
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 10000000)) { // 降频到 10MHz 增加稳定性
    Serial.println("SD Mount Failed!");
    sdCardReady = false;
  } else {
    Serial.println("SD Ready!");
    sdCardReady = true;
  }

  // 创建两个邮筒，每个邮筒最多装 20 个 Chunk (约 10KB 缓存)
  queueSD = xQueueCreate(20, sizeof(LogChunk_t));
  queueWiFi = xQueueCreate(20, sizeof(LogChunk_t));

  // 启动双核任务
  xTaskCreatePinnedToCore(TaskWiFiUpload, "TaskWiFi", 8192, NULL, 2, NULL, 0); // 绑定 Core 0
  xTaskCreatePinnedToCore(TaskSDLog, "TaskSD", 8192, NULL, 2, NULL, 1);        // 绑定 Core 1
}

// ==========================================
// 6. 主循环 (高速搬运工 + 屏幕刷新)
// ==========================================
void loop() {
  unsigned long currentMillis = millis();
  
  // 1. 高速读取 UART 数据
  size_t availableBytes = Serial1.available();
  if (availableBytes > 0) {
    LogChunk_t newChunk;
    // 最多读取 CHUNK_SIZE (512字节)，如果有多少读多少
    size_t bytesToRead = availableBytes < CHUNK_SIZE ? availableBytes : CHUNK_SIZE;
    
    newChunk.length = Serial1.read(newChunk.payload, bytesToRead);
    
    if (newChunk.length > 0) {
      // 0阻塞时间投入邮筒，若队列满则丢弃该包，保护系统不死机
      xQueueSend(queueSD, &newChunk, 0);
      xQueueSend(queueWiFi, &newChunk, 0);
      
      // 状态灯闪烁提示接收到数据
      digitalWrite(LED_STAT, !digitalRead(LED_STAT));
    }
  }

  // 2. 定时刷新 OLED 屏幕 (每 1 秒刷新一次，避免频繁占用 CPU)
  static unsigned long lastOledUpdate = 0;
  if (currentMillis - lastOledUpdate >= 1000) {
    lastOledUpdate = currentMillis;
    
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    
    // 显示 Wi-Fi 状态
    if(WiFi.status() == WL_CONNECTED) u8g2.drawStr(5, 15, "WiFi: Connected");
    else u8g2.drawStr(5, 15, "WiFi: Connecting...");
    
    // 显示 SD 卡与写入量
    u8g2.setCursor(5, 35);
    if(sdCardReady) {
      u8g2.print("SD: OK  [");
      u8g2.print(totalBytesWritten / 1024); // 显示已写入 KB 数
      u8g2.print(" KB]");
    } else {
      u8g2.print("SD: FAULT");
    }

    // 显示当前波特率标志
    u8g2.drawStr(5, 55, "UART: 921600 bps");
    
    u8g2.sendBuffer(); // 耗时操作，已被底层 16KB RX Buffer 保护
  }

  // 短暂让出 CPU 给 FreeRTOS 调度器
  vTaskDelay(1 / portTICK_PERIOD_MS);
}