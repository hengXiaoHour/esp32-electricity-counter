#include "limit_manager.h"

void LimitManager::begin() {
  warningThreshold = WARNING_THRESHOLD_PCT;
}

void LimitManager::check(const float *currentRMS, const float *activePower,
                         const float *powerLimit, const float *currentLimit,
                         ChannelData *channels, RelayController &relays,
                         Event *events, uint8_t &eventCount) {
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    float power = activePower[ch];
    float current = currentRMS[ch];
    float pLimit = powerLimit[ch];
    float cLimit = currentLimit[ch];

    // Use power-based limiting as primary (V×I with shared voltage)
    float limit = (pLimit > 0.0f) ? pLimit : cLimit * 230.0f; // fallback to V×I estimate
    float usage = (pLimit > 0.0f) ? power : current;
    float pct = (limit > 0.0f) ? (usage / limit) * 100.0f : 0.0f;

    switch (channels[ch].status) {
      case STATUS_OK:
        if (pct >= 100.0f) {
          // Trip
          channels[ch].status = STATUS_TRIPPED;
          if (hasRelay(ch)) {
            relays.trip(ch);
            channels[ch].relayOn = false;
          }
          logEvent(events, eventCount, ch, STATUS_TRIPPED,
                   pct, "Tripped: over limit");
        } else if (pct >= warningThreshold) {
          channels[ch].status = STATUS_WARNING;
          logEvent(events, eventCount, ch, STATUS_WARNING,
                   pct, "Warning: approaching limit");
        }
        break;

      case STATUS_WARNING:
        if (pct >= 100.0f) {
          channels[ch].status = STATUS_TRIPPED;
          if (hasRelay(ch)) {
            relays.trip(ch);
            channels[ch].relayOn = false;
          }
          logEvent(events, eventCount, ch, STATUS_TRIPPED,
                   pct, "Tripped: over limit");
        } else if (pct < warningThreshold) {
          channels[ch].status = STATUS_OK;
        }
        break;

      case STATUS_TRIPPED:
        // Stay tripped until manually reset
        break;

      case STATUS_DISABLED:
        break;
    }
  }
}

void LimitManager::resetChannel(uint8_t channel, RelayController &relays,
                                ChannelData *channels) {
  if (channel >= NUM_CHANNELS) return;
  if (channels[channel].status != STATUS_TRIPPED) return;

  channels[channel].status = STATUS_OK;
  if (hasRelay(channel)) {
    relays.reset(channel);
    channels[channel].relayOn = true;
  }
}

bool LimitManager::hasRelay(uint8_t channel) const {
  return channel < RELAY_CHANNEL_COUNT;
}

void LimitManager::logEvent(Event *events, uint8_t &eventCount,
                            uint8_t channel, ChannelStatus status,
                            float value, const char *msg) {
  if (eventCount >= EVENT_LOG_SIZE) {
    // Shift events to make room (drop oldest)
    for (uint8_t i = 1; i < EVENT_LOG_SIZE; i++) {
      events[i - 1] = events[i];
    }
    eventCount = EVENT_LOG_SIZE - 1;
  }

  Event &ev = events[eventCount];
  ev.timestamp = millis();
  ev.channel = channel;
  ev.status = status;
  ev.value = value;
  strncpy(ev.message, msg, EVENT_MSG_LEN - 1);
  ev.message[EVENT_MSG_LEN - 1] = '\0';
  eventCount++;
}
