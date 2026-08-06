#include "ntfy_notifier.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "../config.h"

void NtfyNotifier::begin(bool en, const String &tp) {
  enabled = en;
  topic = tp;
  pending = false;
  lastAttempt = 0;
  Serial.printf("  [NTFY] notifier %s (topic \"%s\")\n",
    enabled ? "enabled" : "disabled", topic.c_str());
}

void NtfyNotifier::setConfig(bool en, const String &tp) {
  enabled = en;
  topic = tp;
  if (!enabled) pending = false;
  Serial.printf("  [NTFY] config updated: %s (topic \"%s\")\n",
    enabled ? "enabled" : "disabled", topic.c_str());
}

void NtfyNotifier::notify(const String &title, const String &message) {
  pendingTitle = title;
  pendingMessage = message;
  pending = true;
}

void NtfyNotifier::loop() {
  if (!pending) return;

  if (!enabled || topic.length() == 0) {
    pending = false;
    return;
  }

  if (!WiFi.isConnected()) return;  // keep pending, retry next loop

  // Retry at most once every 10s so a dead server cannot stall networkTask
  if (millis() - lastAttempt < RETRY_INTERVAL_MS) return;
  lastAttempt = millis();

  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(NTFY_HOST, NTFY_PORT, 5000)) {
    Serial.printf("  [NTFY] connect to %s:%d failed — will retry\n", NTFY_HOST, NTFY_PORT);
    return;  // keep pending
  }
  client.setTimeout(5);

  String body = "{\"title\":\"";
  body += pendingTitle;
  body += "\",\"message\":\"";
  body += pendingMessage;
  body += "\"}";

  client.print("POST /");
  client.print(topic);
  client.print(" HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(NTFY_HOST);
  client.print("\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: ");
  client.print(body.length());
  client.print("\r\nConnection: close\r\n\r\n");
  client.print(body);

  unsigned long t0 = millis();
  while (client.available() == 0 && millis() - t0 < 3000) {
    delay(1);
  }

  int code = 0;
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (line.startsWith("HTTP/1.") || line.startsWith("HTTP/2")) {
      int sp = line.indexOf(' ');
      if (sp >= 0) code = line.substring(sp + 1).toInt();
    }
  }
  client.stop();

  bool success = (code >= 200 && code < 300);
  Serial.printf("  [NTFY] push %s (HTTP %d, topic \"%s\")\n",
    success ? "OK" : "FAILED", code, topic.c_str());
  if (success) pending = false;  // keep pending on failure -> retry
}
