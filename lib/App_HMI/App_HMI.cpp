#include "App_HMI.h"
#include "BSP_Button.h"
#include <U8g2lib.h>
#include <WiFi.h>

extern U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2;
extern bool sdCardReady;
extern uint32_t totalBytesWritten;
extern const char* udpAddress; 

// 🌟 引入新增加的全局波特率变量
extern const uint32_t UART_BAUD_RATE; 

// 引入跨核控制指令标志位
extern volatile bool requestSafeEject; 
extern volatile bool isRecording;
extern volatile bool requestToggleRecord;

enum HMIState {
    PAGE_HOME,
    PAGE_STORAGE,
    PAGE_NETWORK
};

HMIState currentPage = PAGE_HOME;
bool isScreenOff = false;
bool isKeyLocked = false;
bool isAPMode = false; 

void ToggleWiFiMode() {
    if (!isAPMode) {
        WiFi.disconnect(); 
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ESP32_Logger_AP", "12345678"); 
        isAPMode = true;
    } else {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin("DDWW_Nwpu", "acq902m3"); 
        isAPMode = false;
    }
}

void DrawScreen() {
    if (isScreenOff) return; 

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr); 

    u8g2.drawLine(0, 12, 128, 12); 
    if (isKeyLocked) u8g2.drawStr(110, 10, "LK"); 
    
    switch (currentPage) {
        case PAGE_HOME:
            u8g2.drawStr(2, 10, "HOME | Logger");
            
            if (isRecording) {
                u8g2.drawStr(5, 28, "Status: [ REC ]");
            } else {
                u8g2.drawStr(5, 28, "Status: STANDBY");
            }
            
            u8g2.setCursor(5, 43);
            u8g2.print("Uptime: ");
            u8g2.print(millis() / 1000);
            u8g2.print(" s");
            
            // 🌟 动态显示全局波特率
            u8g2.setCursor(5, 58);
            u8g2.print("UART: ");
            u8g2.print(UART_BAUD_RATE);
            u8g2.print(" bps");
            break;

        case PAGE_STORAGE:
            u8g2.drawStr(2, 10, "SD CARD INFO");
            u8g2.setCursor(5, 28);
            if (sdCardReady) u8g2.print("State: MOUNTED OK");
            else u8g2.print("State: FAULT / NO SD");
            u8g2.setCursor(5, 43);
            u8g2.print("Saved: ");
            u8g2.print(totalBytesWritten / 1024);
            u8g2.print(" KB");
            u8g2.drawStr(5, 58, "File: log_current.bin");
            break;

        case PAGE_NETWORK:
            u8g2.drawStr(2, 10, "NETWORK (UDP)");
            u8g2.setCursor(5, 28);
            if (isAPMode) {
                u8g2.print("Mode: AP (Hotspot)");
                u8g2.setCursor(5, 43);
                u8g2.print("IP: ");
                u8g2.print(WiFi.softAPIP().toString());
            } else {
                if (WiFi.status() == WL_CONNECTED) {
                    u8g2.print("Mode: STA (Connected)");
                    u8g2.setCursor(5, 43);
                    u8g2.print("IP: ");
                    u8g2.print(udpAddress);
                } else {
                    u8g2.print("Mode: STA (Connecting)");
                    u8g2.setCursor(5, 43);
                    u8g2.print("Target: ");
                    u8g2.print(udpAddress);
                }
            }
            u8g2.drawStr(5, 58, "Port: 6500");
            break;
    }
    u8g2.sendBuffer(); 
}

void ProcessButtonEvent(ButtonMsg_t msg) {
    if (isKeyLocked && msg.id != BTN_COMBO_UP_DOWN) return;

    if (isScreenOff) {
        isScreenOff = false; 
        DrawScreen();
        return; 
    }

    switch (msg.id) {
        case BTN_UP:
            if (msg.event == EVT_SHORT_CLICK) {
                currentPage = (currentPage == PAGE_HOME) ? PAGE_NETWORK : (HMIState)(currentPage - 1);
            } else if (msg.event == EVT_LONG_PRESS) {
                ToggleWiFiMode(); 
            }
            break;

        case BTN_DOWN:
            if (msg.event == EVT_SHORT_CLICK) {
                currentPage = (currentPage == PAGE_NETWORK) ? PAGE_HOME : (HMIState)(currentPage + 1);
            } else if (msg.event == EVT_LONG_PRESS) {
                isScreenOff = true; 
                u8g2.clearBuffer();
                u8g2.sendBuffer(); 
            }
            break;

        case BTN_ENTER:
            if (msg.event == EVT_LONG_PRESS) {
                if (sdCardReady) {
                    bool willRecord = !isRecording; 
                    requestToggleRecord = true;     
                    
                    u8g2.clearBuffer();
                    u8g2.setFont(u8g2_font_ncenB14_tr); 
                    if (willRecord) {
                        u8g2.drawStr(15, 40, "RECORDING!");
                    } else {
                        u8g2.drawStr(15, 40, "STOPPED!");
                    }
                    u8g2.sendBuffer(); 
                    
                    vTaskDelay(1000 / portTICK_PERIOD_MS); 
                    currentPage = PAGE_HOME; 
                }
            }
            break;

        case BTN_BACK:
            if (msg.event == EVT_SHORT_CLICK) {
                currentPage = PAGE_HOME; 
            } else if (msg.event == EVT_LONG_PRESS) {
                if (sdCardReady) {
                    requestSafeEject = true;
                    
                    u8g2.clearBuffer();
                    u8g2.setFont(u8g2_font_ncenB14_tr); 
                    u8g2.drawStr(15, 30, "SAFE TO");
                    u8g2.drawStr(15, 50, "REMOVE");
                    u8g2.sendBuffer(); 
                    
                    vTaskDelay(2000 / portTICK_PERIOD_MS); 
                    currentPage = PAGE_STORAGE; 
                }
            }
            break;
            
        default:
            break;
    }
}

void TaskHMI(void *pvParameters) {
    ButtonMsg_t rxMsg;
    DrawScreen(); 

    for(;;) {
        if (xQueueReceive(queueButton, &rxMsg, 1000 / portTICK_PERIOD_MS) == pdPASS) {
            ProcessButtonEvent(rxMsg);
        }
        if (!isScreenOff) DrawScreen();
    }
}

void App_HMI_Init() {
    xTaskCreatePinnedToCore(TaskHMI, "TaskHMI", 4096, NULL, 2, NULL, 1);
}