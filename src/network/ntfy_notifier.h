#pragma once

#include <Arduino.h>

// Fire-and-forget ntfy.sh HTTPS push client.
// notify() is called from the sensor core; loop() runs on the network core
// and sends any pending message over WiFiClientSecure (non-blocking, short
// timeouts). The pending flag survives WiFi outages — the message is retried
// until it goes out or the notifier is disabled.
class NtfyNotifier {
public:
  void begin(bool enabled, const String &topic);
  void setConfig(bool enabled, const String &topic);

  // Queue a push. Stores the message and sets the pending flag.
  void notify(const String &title, const String &message);

  // Send pending push if WiFi is up. Call from networkTask.
  void loop();

private:
  bool enabled;
  String topic;
  volatile bool pending;
  String pendingTitle;
  String pendingMessage;

  uint32_t lastAttempt;
  static const uint32_t RETRY_INTERVAL_MS = 10000;
};
