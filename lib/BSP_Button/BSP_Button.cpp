#include "BSP_Button.h"

QueueHandle_t queueButton;

struct ButtonState {
    uint8_t pin;
    BtnID_t id;
    bool isPressed;
    uint32_t pressTime;
    bool longPressTriggered;
};

// 对应引脚 GPIO4, 5, 6, 7
ButtonState btns[4] = {
    {4, BTN_UP, false, 0, false},
    {5, BTN_DOWN, false, 0, false},
    {6, BTN_ENTER, false, 0, false},
    {7, BTN_BACK, false, 0, false}
};

void TaskButtonScan(void *pvParameters) {
    ButtonMsg_t msg;
    for(;;) {
        for (int i = 0; i < 4; i++) {
            bool currentState = (digitalRead(btns[i].pin) == LOW); 

            if (currentState && !btns[i].isPressed) {
                btns[i].isPressed = true;
                btns[i].pressTime = millis();
                btns[i].longPressTriggered = false;
            }
            else if (currentState && btns[i].isPressed) {
                // 长按判定：大于1000ms
                if (!btns[i].longPressTriggered && (millis() - btns[i].pressTime > 1000)) {
                    btns[i].longPressTriggered = true;
                    msg.id = btns[i].id;
                    msg.event = EVT_LONG_PRESS;
                    xQueueSend(queueButton, &msg, 0); 
                }
            }
            else if (!currentState && btns[i].isPressed) {
                btns[i].isPressed = false;
                // 短按判定：大于20ms消抖
                if (!btns[i].longPressTriggered && (millis() - btns[i].pressTime > 20)) {
                    msg.id = btns[i].id;
                    msg.event = EVT_SHORT_CLICK;
                    xQueueSend(queueButton, &msg, 0); 
                }
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS); 
    }
}

void BSP_Button_Init() {
    for (int i = 0; i < 4; i++) {
        pinMode(btns[i].pin, INPUT_PULLUP); 
    }
    queueButton = xQueueCreate(10, sizeof(ButtonMsg_t));
    xTaskCreatePinnedToCore(TaskButtonScan, "TaskBtn", 2048, NULL, 3, NULL, 1);
}