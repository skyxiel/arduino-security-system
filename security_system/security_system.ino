// Electronic Security System - keypad access control as a state machine
//
// PIN entry on a 4x4 keypad, LCD feedback, servo "bolt", buzzer and LEDs.
// Features: masked entry, auto-relock, failed-attempt counter with escalating
// lockouts, a latched alarm that only the admin PIN clears (and survives a
// power cycle), user PIN change, admin menu, PINs stored in EEPROM.
//
// Wiring (see README.md):
//   Keypad (left to right, 8 wires): R1 D9, R2 D8, R3 D7, R4 D6, C1 D5, C2 D4, C3 D3, C4 D2
//   LCD 1602:  RS A0, E A1, D4 A2, D5 A3, D6 A4, D7 A5, RW -> GND, V0 -> 1k -> GND,
//              VSS GND, VDD 5V, A -> 220R -> 5V, K GND
//   Servo SG90: signal D10, red 5V, brown GND
//   Passive buzzer: D11 -> buzzer -> GND
//   Green LED D12, red LED D13 (each via 220R to GND)
//
// Keys:  0-9 enter digits   # = enter   * = clear   C = backspace
//        When unlocked:  A = change PIN   B = lock now
//        When locked:    D = admin menu
// Default PINs: user 1234, admin 9999  (change them!)

#include <LiquidCrystal.h>
#include <Servo.h>
#include <EEPROM.h>

// ---------- Pins ----------
const byte ROW_PINS[4] = {9, 8, 7, 6};
const byte COL_PINS[4] = {5, 4, 3, 2};
const char KEYS[4][4] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
const int PIN_SERVO = 10, PIN_BUZZER = 11, PIN_LED_GREEN = 12, PIN_LED_RED = 13;
LiquidCrystal lcd(A0, A1, A2, A3, A4, A5);
Servo bolt;

// ---------- Settings ----------
const int LOCKED_ANGLE = 0, UNLOCKED_ANGLE = 90;
const unsigned long RELOCK_MS = 10000;
const unsigned long ENTRY_TIMEOUT_MS = 10000;
const unsigned long MENU_TIMEOUT_MS = 20000;
const unsigned long BASE_LOCKOUT_MS = 30000;  // doubles after each lockout
const byte MAX_ATTEMPTS = 3;                  // wrong PINs before a lockout
const byte LOCKOUTS_BEFORE_ALARM = 3;
const byte MIN_PIN = 4, MAX_PIN = 8;

struct Settings {
  uint16_t magic;
  char userPin[MAX_PIN + 1];
  char adminPin[MAX_PIN + 1];
  byte alarmLatched;
  byte lockoutCount;
};
const uint16_t SETTINGS_MAGIC = 0x5EC5;
Settings cfg;

// ---------- State machine ----------
enum State {
  LOCKED, UNLOCKED, LOCKOUT, ALARM,
  NEW_PIN, CONFIRM_PIN,
  ADMIN_LOGIN, ADMIN_MENU, NEW_ADMIN_PIN, CONFIRM_ADMIN_PIN,
  MESSAGE
};
const char *const STATE_NAMES[] = {
  "LOCKED", "UNLOCKED", "LOCKOUT", "ALARM",
  "NEW_PIN", "CONFIRM_PIN",
  "ADMIN_LOGIN", "ADMIN_MENU", "NEW_ADMIN_PIN", "CONFIRM_ADMIN_PIN",
  "MESSAGE"
};

State state = LOCKED;
State afterMessage = LOCKED;
unsigned long stateStart = 0;
unsigned long lastKeyAt = 0;
unsigned long lockoutMs = 0;
unsigned long messageMs = 0;
byte failedAttempts = 0;

char entry[MAX_PIN + 1] = "";
byte entryLen = 0;
char pendingPin[MAX_PIN + 1] = "";

// ---------- Keypad scanning (no library needed) ----------
char scanKeypad() {
  char found = 0;
  for (byte r = 0; r < 4; r++) {
    pinMode(ROW_PINS[r], OUTPUT);
    digitalWrite(ROW_PINS[r], LOW);
    delayMicroseconds(10);
    for (byte c = 0; c < 4; c++) {
      if (digitalRead(COL_PINS[c]) == LOW) found = KEYS[r][c];
    }
    pinMode(ROW_PINS[r], INPUT);  // high impedance so rows never fight
  }
  return found;
}

// Returns a key once per press, debounced
char getKey() {
  static char lastRaw = 0, stable = 0;
  static unsigned long changedAt = 0;
  char raw = scanKeypad();
  if (raw != lastRaw) {
    lastRaw = raw;
    changedAt = millis();
  }
  if (millis() - changedAt > 25 && raw != stable) {
    stable = raw;
    if (stable) return stable;
  }
  return 0;
}

// ---------- Outputs ----------
void lcdLine(int row, const char *text) {
  char padded[17];
  snprintf(padded, sizeof(padded), "%-16s", text);
  lcd.setCursor(0, row);
  lcd.print(padded);
}

void showEntry() {
  char masked[17];
  byte i = 0;
  for (; i < entryLen && i < 16; i++) masked[i] = '*';
  masked[i] = 0;
  lcdLine(1, masked);
}

void moveBolt(int angle) {
  bolt.attach(PIN_SERVO);
  bolt.write(angle);
  delay(400);      // give the servo time to get there...
  bolt.detach();   // ...then stop driving it so it doesn't buzz or draw current
}

void beepKey()     { tone(PIN_BUZZER, 2000, 25); }
void beepOk()      { tone(PIN_BUZZER, 1500, 120); }
void beepError()   { tone(PIN_BUZZER, 250, 400); }

// ---------- Settings storage ----------
void loadSettings() {
  EEPROM.get(0, cfg);
  if (cfg.magic != SETTINGS_MAGIC) {
    cfg.magic = SETTINGS_MAGIC;
    strcpy(cfg.userPin, "1234");
    strcpy(cfg.adminPin, "9999");
    cfg.alarmLatched = 0;
    cfg.lockoutCount = 0;
    EEPROM.put(0, cfg);
    Serial.println(F("# first run: default PINs set (user 1234, admin 9999)"));
  }
}

void saveSettings() { EEPROM.put(0, cfg); }  // put() only writes bytes that changed

// Compares every character even after a mismatch, so the time taken doesn't
// reveal how many leading digits were right (a real timing side-channel).
bool pinMatches(const char *a, const char *b) {
  byte la = strlen(a), lb = strlen(b);
  byte diff = la ^ lb;
  for (byte i = 0; i < MAX_PIN; i++) {
    char ca = i < la ? a[i] : 0;
    char cb = i < lb ? b[i] : 0;
    diff |= ca ^ cb;
  }
  return diff == 0;
}

// ---------- State handling ----------
void clearEntry() {
  entryLen = 0;
  entry[0] = 0;
}

void logEvent(const __FlashStringHelper *msg) {
  Serial.print(F("# "));
  Serial.print(millis() / 1000.0, 1);
  Serial.print(F("s  "));
  Serial.println(msg);
}

void enter(State s) {
  state = s;
  stateStart = millis();
  lastKeyAt = millis();
  clearEntry();
  digitalWrite(PIN_LED_GREEN, s == UNLOCKED);
  digitalWrite(PIN_LED_RED, s == LOCKED || s == LOCKOUT);

  switch (s) {
    case LOCKED:
      lcdLine(0, "LOCKED Enter PIN");
      lcdLine(1, "");
      break;
    case UNLOCKED:
      lcdLine(0, "UNLOCKED");
      lcdLine(1, "A:new PIN B:lock");
      break;
    case LOCKOUT:
      lcdLine(0, "Too many tries");
      break;
    case ALARM:
      lcdLine(0, "!! ALARM !!");
      lcdLine(1, "Admin PIN:");
      break;
    case NEW_PIN:
    case NEW_ADMIN_PIN:
      lcdLine(0, s == NEW_PIN ? "New PIN (4-8) #" : "New admin PIN #");
      lcdLine(1, "");
      break;
    case CONFIRM_PIN:
    case CONFIRM_ADMIN_PIN:
      lcdLine(0, "Confirm PIN #");
      lcdLine(1, "");
      break;
    case ADMIN_LOGIN:
      lcdLine(0, "Admin PIN #");
      lcdLine(1, "");
      break;
    case ADMIN_MENU:
      lcdLine(0, "A:admin PIN");
      lcdLine(1, "B:reset user *:x");
      break;
    case MESSAGE:
      break;
  }
  Serial.print(F("# state -> ")); Serial.println(STATE_NAMES[s]);
}

void showMessage(const char *l1, const char *l2, unsigned long ms, State next) {
  enter(MESSAGE);
  lcdLine(0, l1);
  lcdLine(1, l2);
  messageMs = ms;
  afterMessage = next;
}

void unlock() {
  failedAttempts = 0;
  if (cfg.lockoutCount) { cfg.lockoutCount = 0; saveSettings(); }
  logEvent(F("access granted"));
  beepOk();
  enter(UNLOCKED);
  moveBolt(UNLOCKED_ANGLE);
}

void lockNow(const __FlashStringHelper *why) {
  logEvent(why);
  moveBolt(LOCKED_ANGLE);
  enter(LOCKED);
}

void raiseAlarm() {
  cfg.alarmLatched = 1;
  saveSettings();
  logEvent(F("ALARM raised"));
  moveBolt(LOCKED_ANGLE);
  enter(ALARM);
}

void wrongPin() {
  failedAttempts++;
  beepError();
  Serial.print(F("# wrong PIN, attempt ")); Serial.println(failedAttempts);
  if (failedAttempts < MAX_ATTEMPTS) {
    char l2[17];
    byte left = MAX_ATTEMPTS - failedAttempts;
    snprintf(l2, sizeof(l2), "%d %s left", left, left == 1 ? "try" : "tries");
    showMessage("Wrong PIN", l2, 1500, LOCKED);
    return;
  }
  failedAttempts = 0;
  cfg.lockoutCount++;
  saveSettings();
  if (cfg.lockoutCount >= LOCKOUTS_BEFORE_ALARM) {
    raiseAlarm();
  } else {
    lockoutMs = BASE_LOCKOUT_MS << (cfg.lockoutCount - 1);  // 30 s, 60 s, ...
    logEvent(F("lockout started"));
    enter(LOCKOUT);
  }
}

// Collects digits into `entry`. Returns true when # is pressed.
bool collectDigits(char key) {
  if (key >= '0' && key <= '9') {
    if (entryLen < MAX_PIN) {
      entry[entryLen++] = key;
      entry[entryLen] = 0;
    }
  } else if (key == 'C' && entryLen > 0) {
    entry[--entryLen] = 0;
  } else if (key == '*') {
    clearEntry();
  } else if (key == '#') {
    return true;
  }
  showEntry();
  return false;
}

void handleKey(char key) {
  lastKeyAt = millis();
  beepKey();

  switch (state) {
    case LOCKED:
      if (key == 'D' && entryLen == 0) { enter(ADMIN_LOGIN); break; }
      if (collectDigits(key)) {
        if (pinMatches(entry, cfg.userPin) || pinMatches(entry, cfg.adminPin)) unlock();
        else wrongPin();
      }
      break;

    case UNLOCKED:
      if (key == 'A') enter(NEW_PIN);
      else if (key == 'B') lockNow(F("locked by user"));
      break;

    case ALARM:
      if (collectDigits(key)) {
        if (pinMatches(entry, cfg.adminPin)) {
          cfg.alarmLatched = 0;
          cfg.lockoutCount = 0;
          saveSettings();
          noTone(PIN_BUZZER);
          logEvent(F("alarm cleared by admin"));
          showMessage("Alarm cleared", "", 1500, LOCKED);
        } else {
          logEvent(F("wrong admin PIN during alarm"));
          clearEntry();
          showEntry();
        }
      }
      break;

    case NEW_PIN:
    case NEW_ADMIN_PIN:
      if (collectDigits(key)) {
        if (entryLen < MIN_PIN) {
          beepError();
          showMessage("PIN too short", "4-8 digits", 1500, state);
        } else {
          strcpy(pendingPin, entry);
          enter(state == NEW_PIN ? CONFIRM_PIN : CONFIRM_ADMIN_PIN);
        }
      }
      break;

    case CONFIRM_PIN:
    case CONFIRM_ADMIN_PIN:
      if (collectDigits(key)) {
        bool isAdmin = (state == CONFIRM_ADMIN_PIN);
        if (strcmp(entry, pendingPin) == 0) {
          strcpy(isAdmin ? cfg.adminPin : cfg.userPin, pendingPin);
          saveSettings();
          beepOk();
          logEvent(isAdmin ? F("admin PIN changed") : F("user PIN changed"));
          showMessage("PIN saved", "", 1500, isAdmin ? LOCKED : UNLOCKED);
        } else {
          beepError();
          showMessage("PINs differ", "Try again", 1500, isAdmin ? NEW_ADMIN_PIN : NEW_PIN);
        }
        memset(pendingPin, 0, sizeof(pendingPin));
      }
      break;

    case ADMIN_LOGIN:
      if (collectDigits(key)) {
        if (pinMatches(entry, cfg.adminPin)) {
          logEvent(F("admin login"));
          enter(ADMIN_MENU);
        } else {
          wrongPin();  // admin guesses count as failed attempts too
        }
      }
      break;

    case ADMIN_MENU:
      if (key == 'A') enter(NEW_ADMIN_PIN);
      else if (key == 'B') {
        strcpy(cfg.userPin, "1234");
        saveSettings();
        logEvent(F("user PIN reset to default"));
        showMessage("User PIN reset", "to 1234", 1500, LOCKED);
      } else if (key == '*' || key == '#') enter(LOCKED);
      break;

    case LOCKOUT:
    case MESSAGE:
      break;  // keys ignored
  }
}

// Time-based behaviour
void updateTimers() {
  unsigned long now = millis();
  unsigned long inState = now - stateStart;

  switch (state) {
    case UNLOCKED:
      if (inState > RELOCK_MS) lockNow(F("auto relock"));
      break;

    case LOCKED:
      if (entryLen > 0 && now - lastKeyAt > ENTRY_TIMEOUT_MS) {
        clearEntry();
        showEntry();
      }
      break;

    case LOCKOUT: {
      if (inState >= lockoutMs) {
        logEvent(F("lockout ended"));
        enter(LOCKED);
        break;
      }
      static unsigned long lastShown = 0;
      if (now - lastShown >= 250) {
        lastShown = now;
        char l2[17];
        snprintf(l2, sizeof(l2), "Wait %lus", (lockoutMs - inState + 999) / 1000);
        lcdLine(1, l2);
      }
      break;
    }

    case ALARM: {
      // Siren: sweep 600-1200 Hz, flash the red LED
      unsigned long phase = now % 1000;
      int freq = phase < 500 ? 600 + phase * 6 / 5 : 1200 - (phase - 500) * 6 / 5;
      tone(PIN_BUZZER, freq);
      digitalWrite(PIN_LED_RED, (now / 150) % 2);
      break;
    }

    case NEW_PIN:
    case CONFIRM_PIN:
      if (now - lastKeyAt > MENU_TIMEOUT_MS) lockNow(F("PIN change timed out"));
      break;

    case ADMIN_LOGIN:
    case ADMIN_MENU:
    case NEW_ADMIN_PIN:
    case CONFIRM_ADMIN_PIN:
      if (now - lastKeyAt > MENU_TIMEOUT_MS) enter(LOCKED);
      break;

    case MESSAGE:
      if (inState >= messageMs) enter(afterMessage);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  for (byte c = 0; c < 4; c++) pinMode(COL_PINS[c], INPUT_PULLUP);
  for (byte r = 0; r < 4; r++) pinMode(ROW_PINS[r], INPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  lcd.begin(16, 2);

  Serial.println(F("# Electronic security system"));
  loadSettings();
  moveBolt(LOCKED_ANGLE);  // always start locked

  if (cfg.alarmLatched) {
    logEvent(F("alarm was active before power loss - still latched"));
    enter(ALARM);
  } else if (cfg.lockoutCount > 0) {
    // Failed attempts before the reset: make the attacker wait again rather than
    // letting a power cycle skip the lockout
    lockoutMs = BASE_LOCKOUT_MS << (cfg.lockoutCount - 1);
    logEvent(F("restarted after failed attempts - lockout"));
    enter(LOCKOUT);
  } else {
    enter(LOCKED);
  }
}

void loop() {
  char key = getKey();
  if (key) handleKey(key);
  updateTimers();
}
