#pragma once

#include <Arduino.h>
#include "../config.h"

class NVSManager;
class PowerCalculator;

// The one and only system-state serialiser.
//
// It used to live at the bottom of firebase_bridge.cpp because the cloud push
// was its first caller, but the LAN WebSocket borrowed it too. With the cloud
// gone it is promoted to its own translation unit: the WebSocket broadcast is
// now the ONLY consumer, and burying the wire format inside a deleted bridge
// is how it would have ended up duplicated.
//
// Callers must hold dataMutex (the caller serialises SystemData plus live
// PowerCalculator fields, which are written by sensorTask on the other core).
void buildSystemJson(const SystemData &data, PowerCalculator *powerCalc,
                     NVSManager *nvs, String &json);