#include "log.h"

struct LogLine {
  unsigned long t;
  char msg[96];
};

static LogLine      lines[LOG_LINES];
static int          head  = 0;  // next slot to write
static int          count = 0;
static portMUX_TYPE mux   = portMUX_INITIALIZER_UNLOCKED;

void logEvent(const char *fmt, ...) {
  char buf[sizeof(lines[0].msg)];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  // Trailing newlines are for Serial only.
  size_t n = strlen(buf);
  while (n > 0 && buf[n - 1] == '\n') buf[--n] = 0;
  Serial.println(buf);

  portENTER_CRITICAL(&mux);
  lines[head].t = millis();
  memcpy(lines[head].msg, buf, n + 1);
  head = (head + 1) % LOG_LINES;
  if (count < LOG_LINES) count++;
  portEXIT_CRITICAL(&mux);
}

void logSnapshot(JsonArray out) {
  // Copy under the lock, serialize outside it (JSON allocates). Static to keep
  // ~4 KB off the caller's stack; only the web server task calls this.
  static LogLine snap[LOG_LINES];
  int n, start;
  portENTER_CRITICAL(&mux);
  memcpy(snap, lines, sizeof(lines));
  n = count;
  start = (head - count + LOG_LINES) % LOG_LINES;
  portEXIT_CRITICAL(&mux);

  for (int i = 0; i < n; i++) {
    const LogLine &l = snap[(start + i) % LOG_LINES];
    JsonObject o = out.add<JsonObject>();
    o["t"]   = l.t;
    o["msg"] = l.msg;
  }
}
