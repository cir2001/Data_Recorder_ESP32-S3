#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <Wire.h>

#include "BSP_Button.h"
#include "App_HMI.h"

// ==========================================
// 1. 硬件引脚与网络配置
// ==========================================
const char* ssid = "DDWW_Nwpu";           
const char* password = "acq902m3";       
const char* udpAddress = "192.168.0.112"; 
const int udpPort = 6500;                 

extern const uint32_t UART_BAUD_RATE = 115200; 

const int LED_SYS  = 15;  
const int LED_STAT = 16;  
const int I2C_SDA  = 1;   
const int I2C_SCL  = 2;   
const int SD_CS    = 10; 
const int SD_MOSI  = 11; 
const int SD_SCK   = 12; 
const int SD_MISO  = 13; 
const int UART_RX  = 18;  
const int UART_TX  = 17;  
const int SD_CD    = 8;   

// ==========================================
// 2. 核心数据结构与全局变量
// ==========================================
#define CHUNK_SIZE 1024  

typedef struct {
  uint16_t length;             
  uint8_t payload[CHUNK_SIZE]; 
} LogChunk_t;

// 🌟 新增：256 字节结构化二进制文件头
#define HEADER_SIZE 256
#define MAGIC_WORD 0xAABBCCDD // 身份验证识别码

#pragma pack(push, 1) // 强制编译器 1 字节对齐，防止内存空洞导致文件错位
typedef struct {
    uint32_t magicWord;       // 4 Bytes: 魔数 (0xAABBCCDD)
    uint8_t  version[4];      // 4 Bytes: 版本号 [主, 次, 修补, 内部]
    uint32_t baudRate;        // 4 Bytes: 记录时的波特率
    uint32_t createTimeMs;    // 4 Bytes: 文件创建时的系统开机时间(毫秒)
    char     deviceName[16];  // 16 Bytes: 设备标识符
    uint8_t  padding[HEADER_SIZE - 32]; // 224 Bytes: 预留空白占位符凑齐 256 字节
} LogFileHeader_t;
#pragma pack(pop)

QueueHandle_t queueSD;
QueueHandle_t queueWiFi;
WiFiUDP udp;

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
bool sdCardReady = false;
uint32_t totalBytesWritten = 0; 

volatile bool requestSafeEject = false; 
volatile bool isRecording = false;         
volatile bool requestToggleRecord = false; 

// ==========================================
// 3. 任务 A：Wi-Fi UDP 发送 (Core 0)
// ==========================================
void TaskWiFiUpload(void *pvParameters) {
  Serial.printf("TaskWiFi running on Core %d\n", xPortGetCoreID());

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
  Serial.println("Wi-Fi Connected!");

  LogChunk_t txChunk;
  for (;;) {
    if (xQueueReceive(queueWiFi, &txChunk, portMAX_DELAY) == pdPASS) {
      udp.beginPacket(udpAddress, udpPort);
      udp.write(txChunk.payload, txChunk.length);
      udp.endPacket();
    }
  }
}

// ==========================================
// 4. 任务 B：TF 卡记录与热插拔 (Core 1)
// ==========================================
void TaskSDLog(void *pvParameters) {
  Serial.printf("TaskSD running on Core %d\n", xPortGetCoreID());

  File dataFile;
  char fileName[32];
  int fileIndex = 0;
  uint32_t currentFileSize = 0;
  const uint32_t MAX_FILE_SIZE = 5 * 1024 * 1024; // 5MB 自动切割
  uint32_t flushCounter = 0;

  auto openNextFile = [&]() {
    if (dataFile) dataFile.close();
    while (true) {
      snprintf(fileName, sizeof(fileName), "/log_%03d.bin", fileIndex);
      if (!SD.exists(fileName)) break;
      fileIndex++;
    }
    dataFile = SD.open(fileName, FILE_WRITE);
    currentFileSize = 0;
    Serial.printf("Opened new log: %s\n", fileName);

    // 🌟🌟🌟 核心动作：文件打开成功后，第一时间写入 256 字节表头 🌟🌟🌟
    if (dataFile) {
        LogFileHeader_t header = {0}; // 全部初始化为 0 (清空 Padding 区)
        header.magicWord = MAGIC_WORD;
        header.version[0] = 1; header.version[1] = 0; header.version[2] = 0; header.version[3] = 0; // v1.0.0.0
        header.baudRate = UART_BAUD_RATE;
        header.createTimeMs = millis();
        strncpy(header.deviceName, "ESP32-S3-Logger", sizeof(header.deviceName) - 1);

        // 将结构体强制转换为纯字节流写入 TF 卡
        dataFile.write((const uint8_t*)&header, sizeof(LogFileHeader_t));
        
        currentFileSize += sizeof(LogFileHeader_t);
        totalBytesWritten += sizeof(LogFileHeader_t);
        Serial.println("System: 256-byte Metadata Header Written.");
    }
  };

  LogChunk_t rxChunk;
  uint32_t lastFlushTime = millis(); 
  bool lastCdState = (digitalRead(SD_CD) == LOW); 

  for (;;) {
    bool currentCdState = (digitalRead(SD_CD) == LOW);
    if (currentCdState != lastCdState) {
        vTaskDelay(500 / portTICK_PERIOD_MS); 
        if (digitalRead(SD_CD) == (currentCdState ? LOW : HIGH)) {
            if (currentCdState) { 
                Serial.println("Hardware: SD Card Inserted!");
                if (!sdCardReady) {
                    SD.end(); 
                    vTaskDelay(100 / portTICK_PERIOD_MS); 
                    if (SD.begin(SD_CS, SPI, 10000000)) {
                        sdCardReady = true;
                        digitalWrite(LED_SYS, HIGH); 
                        Serial.println("System: SD Card Remounted Successfully!");
                    } else {
                        sdCardReady = false;
                        digitalWrite(LED_SYS, LOW);  
                        Serial.println("System: SD Card Remount Failed!");
                    }
                }
            } else { 
                Serial.println("Hardware: SD Card Removed!");
                if (sdCardReady) {
                    if (dataFile) dataFile.close(); 
                    SD.end(); 
                    sdCardReady = false;
                    isRecording = false; 
                    digitalWrite(LED_SYS, LOW); 
                    Serial.println("System: SD Card Unmounted by Hardware Pull.");
                }
            }
            lastCdState = currentCdState;
        }
    }

    if (requestSafeEject && sdCardReady) {
      if (dataFile) {
        dataFile.flush();
        dataFile.close();
      }
      SD.end(); 
      sdCardReady = false;
      isRecording = false; 
      requestSafeEject = false; 
      digitalWrite(LED_SYS, LOW); 
      Serial.println("SD Card Safely Unmounted via HMI!");
    }

    if (requestToggleRecord && sdCardReady) {
      requestToggleRecord = false;
      if (isRecording) {
        if (dataFile) {
          dataFile.flush();
          dataFile.close();
        }
        isRecording = false;
        Serial.println("Recording STOPPED.");
      } else {
        openNextFile(); // <--- 重新启动记录时，会再次自动写入 256 字节表头
        isRecording = true;
        Serial.println("Recording STARTED.");
      }
    }
    if (requestToggleRecord && !sdCardReady) requestToggleRecord = false;

    if (xQueueReceive(queueSD, &rxChunk, 50 / portTICK_PERIOD_MS) == pdPASS) {
      if (isRecording && dataFile && sdCardReady) {
        dataFile.write(rxChunk.payload, rxChunk.length);
        currentFileSize += rxChunk.length;
        totalBytesWritten += rxChunk.length;
        flushCounter += rxChunk.length;

        if (currentFileSize >= MAX_FILE_SIZE) {
          openNextFile(); // <--- 5MB 自动切割文件时，也会在新文件开头写入表头
          flushCounter = 0;
          lastFlushTime = millis();
        }
      }
    }

    if (isRecording && dataFile && sdCardReady && (flushCounter >= 50000 || (millis() - lastFlushTime >= 2000 && flushCounter > 0))) {
      dataFile.flush();
      flushCounter = 0;
      lastFlushTime = millis();
    }
  }
}

// ==========================================
// 5. Setup 初始化
// ==========================================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  
  Serial1.setRxBufferSize(16384);
  Serial1.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX, UART_TX);

  pinMode(LED_SYS, OUTPUT);
  pinMode(LED_STAT, OUTPUT);
  digitalWrite(LED_SYS, LOW);  
  digitalWrite(LED_STAT, LOW); 

  pinMode(SD_CD, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL, 400000); 
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr); 
  u8g2.drawStr(5, 15, "System Booting...");
  u8g2.sendBuffer();

  BSP_Button_Init();
  App_HMI_Init();

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 10000000)) { 
    Serial.println("SD Mount Failed on Boot!");
    sdCardReady = false;
    digitalWrite(LED_SYS, LOW); 
  } else {
    Serial.println("SD Ready on Boot!");
    sdCardReady = true;
    digitalWrite(LED_SYS, HIGH); 
  }

  queueSD = xQueueCreate(40, sizeof(LogChunk_t));
  queueWiFi = xQueueCreate(40, sizeof(LogChunk_t));

  xTaskCreatePinnedToCore(TaskWiFiUpload, "TaskWiFi", 8192, NULL, 2, NULL, 0); 
  xTaskCreatePinnedToCore(TaskSDLog, "TaskSD", 8192, NULL, 2, NULL, 1);        
}

// ==========================================
// 6. 主循环 (专职超高速 UART 搬运工)
// ==========================================
static LogChunk_t currentChunk = {0, {0}};
static unsigned long lastReceiveTime = 0;

void loop() {
  unsigned long currentMillis = millis();
  
  size_t avail = Serial1.available();
  while (avail > 0) {
    size_t spaceLeft = CHUNK_SIZE - currentChunk.length;
    size_t bytesToRead = (avail < spaceLeft) ? avail : spaceLeft;
    
    size_t readCount = Serial1.read(currentChunk.payload + currentChunk.length, bytesToRead);
    currentChunk.length += readCount;
    lastReceiveTime = currentMillis;

    if (currentChunk.length >= CHUNK_SIZE) {
      xQueueSend(queueSD, &currentChunk, 5 / portTICK_PERIOD_MS);
      if (WiFi.status() == WL_CONNECTED) {
        xQueueSend(queueWiFi, &currentChunk, 0); 
      }
      currentChunk.length = 0; 
      digitalWrite(LED_STAT, !digitalRead(LED_STAT)); 
    }
    avail = Serial1.available(); 
  }

  if (currentChunk.length > 0 && (currentMillis - lastReceiveTime > 2)) {
    xQueueSend(queueSD, &currentChunk, 5 / portTICK_PERIOD_MS);
    if (WiFi.status() == WL_CONNECTED) {
      xQueueSend(queueWiFi, &currentChunk, 0); 
    }
    currentChunk.length = 0;
    digitalWrite(LED_STAT, !digitalRead(LED_STAT));
  }

  vTaskDelay(1 / portTICK_PERIOD_MS);
}