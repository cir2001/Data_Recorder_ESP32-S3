#ifndef BSP_BUTTON_H
#define BSP_BUTTON_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// 按键ID枚举
typedef enum {
    BTN_UP,
    BTN_DOWN,
    BTN_ENTER,
    BTN_BACK,
    BTN_COMBO_UP_DOWN
} BtnID_t;

// 按键事件枚举
typedef enum {
    EVT_SHORT_CLICK,
    EVT_LONG_PRESS
} BtnEvent_t;

// 传递给队列的消息结构体
typedef struct {
    BtnID_t id;
    BtnEvent_t event;
} ButtonMsg_t;

// 暴露给外部的队列句柄和初始化函数
extern QueueHandle_t queueButton;
void BSP_Button_Init();

#endif