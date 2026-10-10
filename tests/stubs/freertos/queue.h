#pragma once
using QueueHandle_t = void *;
constexpr int pdTRUE = 1;
int xQueueSend(QueueHandle_t queue, const void *item, unsigned wait);
