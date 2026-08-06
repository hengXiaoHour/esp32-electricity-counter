#pragma once

#include "../config.h"
#include "../utils/nvs_manager.h"
#include "../core/power_calculator.h"

class LimitManager;
class SystemData;

// Processes a dashboard command JSON string (e.g. {"cmd":"set_name","ch":0,...}).
// Shared by the WebSocket server and the Firebase bridge.
// Returns true if a command was matched/executed (caller may force an immediate
// broadcast/push), false otherwise.
bool processCommand(NVSManager *nvs, SystemData *sysData,
                    SemaphoreHandle_t *dataMutex,
                    PowerCalculator *powerCalc, LimitManager *limitMgr,
                    const char *msg);