// ElTech-Online ESP32 Desk Timer — a countdown and Pomodoro timer with an OLED
// menu, worked with a single knob, that beeps and buzzes when time is up.
//
// BETA: this sketch compiles but has not been fully tested on hardware yet.
//
// What this kit teaches is mostly about how to ORGANISE a program:
//   - TIMERS         -> counting down with millis() instead of delay()
//   - MENUS          -> a list on a small screen, moved through with a knob
//   - STATE MACHINE  -> the timer is always in exactly one "state" (menu,
//                       setting, running, paused, finished), each with its own
//                       simple rules
//
// Libraries needed (Arduino IDE Library Manager):
//   Adafruit SH110X
//   Adafruit GFX Library
// (click "Install all" if it offers Adafruit BusIO)
// Board package: esp32 by Espressif Systems
// This kit does not use WiFi.
//
// How to use:
//   Turn the knob  = move through a menu, or change a number
//   Press          = choose / start / pause
//   Hold 1 second  = go back / cancel
// Full source, wiring diagram and setup guide: github.com/eltech-online/eltech-esp32-desk-timer
//
// ---------------------------------------------------------------------------
// New to Arduino code? How to read this file
// ---------------------------------------------------------------------------
// Lines starting with // are comments: notes for people, ignored by the board.
// The file is in this order, and you can read it top to bottom:
//   1. Settings       - pin numbers and times you can safely change
//   2. Outputs        - the buzzer and the vibration motor
//   3. Knob           - counting clicks and telling a press from a hold
//   4. States         - what each state does with a turn, a press and a hold
//   5. Screens        - what each state draws on the OLED
//   6. setup()        - runs ONCE when the board is powered on
//   7. loop()         - then runs over and over, forever
// A good first experiment: change POMODORO_WORK_MINUTES below and upload.

#include <Preferences.h>   // saves your settings in flash
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "logo_bitmap.h"   // shop logo bitmap for the OLED splash screen

// ---- Pins ----
#define I2C_SDA    8    // OLED SDA
#define I2C_SCL    9    // OLED SCK
#define MOTOR_PIN  4    // vibration motor module IN
#define ENC_CLK    5    // knob CLK
#define ENC_DT     6    // knob DT
#define ENC_SW     7    // knob SW (the push button)
#define BUZZER_PIN 10   // buzzer module I/O
#define OLED_ADDR  0x3C // try 0x3D if the screen stays blank

// Our buzzer module is printed "Low level trigger": it sounds when its pin is
// LOW. The motor module is the usual way round: it runs when its pin is HIGH.
const bool BUZZER_ACTIVE_LOW = true;
const bool MOTOR_ACTIVE_LOW  = false;

// Turning the knob the "wrong" way round? Change false to true.
const bool ENC_REVERSE = false;

// ---- Times ----
const int POMODORO_WORK_MINUTES  = 25;   // the Pomodoro method: 25 minutes of work...
const int POMODORO_BREAK_MINUTES = 5;    // ...then a 5 minute break, and repeat
const int MAX_TIMER_MINUTES = 180;
const int ALERT_SECONDS = 30;            // the alert stops by itself after this long
const unsigned long HOLD_MS = 800;       // how long counts as "holding" the knob

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Saved in flash, so they survive the power being turned off:
int timerMinutes = 10;        // the last countdown you set
bool soundOn = true;
bool vibrateOn = true;

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------
// One small function per part, so the rest of the code can say setBuzzer(true)
// without caring whether "on" is HIGH or LOW for that part.
void setBuzzer(bool on) { digitalWrite(BUZZER_PIN, (on != BUZZER_ACTIVE_LOW) ? HIGH : LOW); }
void setMotor(bool on)  { digitalWrite(MOTOR_PIN,  (on != MOTOR_ACTIVE_LOW)  ? HIGH : LOW); }

// A very short beep as feedback for a press. delay() is fine for 15 ms.
void chirp() {
  if (!soundOn) return;
  setBuzzer(true);
  delay(15);
  setBuzzer(false);
}

// ---------------------------------------------------------------------------
// Knob
// ---------------------------------------------------------------------------
// Inside the knob are two switches, CLK and DT, that open and close one after
// the other as it turns. When CLK falls, DT is still high for one direction and
// already low for the other. A click is over in a few milliseconds, so CLK is
// watched by an INTERRUPT: the board drops whatever it is doing, runs
// knobTurned(), and carries on. "volatile" tells the compiler the value can
// change at any moment.
volatile int knobSteps = 0;
volatile unsigned long lastKnobUs = 0;

void IRAM_ATTR knobTurned() {
  unsigned long now = micros();
  if (now - lastKnobUs < 1500) return;      // ignore contact bounce
  lastKnobUs = now;
  bool clockwise = digitalRead(ENC_DT) == HIGH;
  if (ENC_REVERSE) clockwise = !clockwise;
  knobSteps += clockwise ? 1 : -1;
}

// What the knob did since last time: how many clicks it turned (+ or -), and
// whether it was pressed or held. loop() asks once each time round.
int turned = 0;
bool pressed = false, held = false;

void readKnob() {
  static bool wasDown = false, holdReported = false;
  static unsigned long downSince = 0;

  // Take the count and reset it, with interrupts paused for those two lines so
  // a click can't land in between and get lost.
  noInterrupts();
  turned = knobSteps;
  knobSteps = 0;
  interrupts();

  pressed = false;
  held = false;
  bool down = digitalRead(ENC_SW) == LOW;   // the button connects SW to GND

  if (down && !wasDown) {                   // just pressed
    downSince = millis();
    holdReported = false;
  }
  // A hold is reported as soon as it has lasted long enough, while the knob is
  // still down, so you can feel when to let go.
  if (down && !holdReported && millis() - downSince > HOLD_MS) {
    held = true;
    holdReported = true;
  }
  // A press is reported on release, and only if it wasn't a hold or a bounce.
  if (!down && wasDown && !holdReported && millis() - downSince > 30) {
    pressed = true;
  }
  wasDown = down;
}

// ---------------------------------------------------------------------------
// States
// ---------------------------------------------------------------------------
// The timer is always in exactly ONE of these states. Each state has its own
// short function that says what a turn, a press and a hold do THERE. Writing a
// program this way is called a "state machine". Without it, every button would
// need a tangle of "if we're running but not paused and not in the menu...".
//
//   MENU --press--> SET_TIME --press--> RUNNING --press--> PAUSED
//                                          |                  |
//                                      time is up          press = resume
//                                          v
//                                        DONE --press--> MENU (or next Pomodoro)
//   hold = back to MENU from anywhere
const int MENU = 0, SET_TIME = 1, RUNNING = 2, PAUSED = 3, DONE = 4, SETTINGS = 5;
int state = MENU;
unsigned long stateSince = 0;

const char* MENU_ITEMS[] = { "Timer", "Pomodoro", "Settings" };
const int MENU_COUNT = 3;
int menuIndex = 0;       // which menu line is highlighted
int settingsIndex = 0;   // which settings line is highlighted (0 sound, 1 vibrate, 2 back)

// The countdown.
unsigned long endMs = 0;          // millis() at which the countdown reaches zero
unsigned long remainingMs = 0;    // time left (kept up to date while running, frozen while paused)
unsigned long totalMs = 1;        // length of this countdown, for the progress bar
bool pomodoroMode = false;
bool onBreak = false;             // Pomodoro: are we in the break?
int pomodorosDone = 0;

void saveSettings() {
  Preferences prefs;
  prefs.begin("timer", false);
  prefs.putInt("minutes", timerMinutes);
  prefs.putBool("sound", soundOn);
  prefs.putBool("vibrate", vibrateOn);
  prefs.end();
}

void enterState(int newState) {
  state = newState;
  stateSince = millis();
  setBuzzer(false);
  setMotor(false);
}

// Starts a countdown of this many minutes.
//
// The key idea: we do NOT count seconds one by one. We work out the moment the
// countdown will END, and every time round loop() we just compare that with
// the clock. The board can be busy for a moment and the timer is still exact.
void startCountdown(int minutes) {
  totalMs = (unsigned long)minutes * 60000UL;     // 60000 ms in a minute
  remainingMs = totalMs;
  endMs = millis() + totalMs;
  enterState(RUNNING);
}

void startPomodoroPhase() {
  startCountdown(onBreak ? POMODORO_BREAK_MINUTES : POMODORO_WORK_MINUTES);
}

// --- What each state does with the knob. Called from loop(). ---

void runMenu() {
  // (x + n) % n keeps the index inside 0..n-1 when it runs off either end.
  if (turned != 0) menuIndex = (menuIndex + turned % MENU_COUNT + MENU_COUNT) % MENU_COUNT;
  if (pressed) {
    chirp();
    if (menuIndex == 0) { pomodoroMode = false; enterState(SET_TIME); }
    if (menuIndex == 1) { pomodoroMode = true; onBreak = false; pomodorosDone = 0; startPomodoroPhase(); }
    if (menuIndex == 2) { settingsIndex = 0; enterState(SETTINGS); }
  }
}

void runSetTime() {
  if (turned != 0) timerMinutes = constrain(timerMinutes + turned, 1, MAX_TIMER_MINUTES);
  if (pressed) {
    chirp();
    saveSettings();                 // remember this length for next time
    startCountdown(timerMinutes);
  }
  if (held) enterState(MENU);
}

void runRunning() {
  unsigned long now = millis();
  if (now >= endMs) {               // time is up
    remainingMs = 0;
    enterState(DONE);
    return;
  }
  remainingMs = endMs - now;
  if (pressed) { chirp(); enterState(PAUSED); }   // remainingMs is now frozen
  if (held) enterState(MENU);
}

void runPaused() {
  if (pressed) {
    chirp();
    endMs = millis() + remainingMs; // a new end moment, the frozen time from now
    enterState(RUNNING);
  }
  if (held) enterState(MENU);
}

void runDone() {
  // The alert: three short pulses, then a pause, repeating. now % 1200 counts
  // 0-1199 over and over; the pulses are on during 0-99, 200-299 and 400-499.
  unsigned long t = (millis() - stateSince) % 1200;
  bool pulse = t < 500 && (t / 100) % 2 == 0;
  bool alerting = millis() - stateSince < (unsigned long)ALERT_SECONDS * 1000;
  setBuzzer(alerting && pulse && soundOn);
  setMotor(alerting && pulse && vibrateOn);

  if (pressed) {
    if (pomodoroMode) {
      if (!onBreak) pomodorosDone++;
      onBreak = !onBreak;           // work -> break -> work -> ...
      startPomodoroPhase();
    } else {
      enterState(MENU);
    }
  }
  if (held) enterState(MENU);
}

void runSettings() {
  if (turned != 0) settingsIndex = (settingsIndex + turned % 3 + 3) % 3;
  if (pressed) {
    if (settingsIndex == 0) soundOn = !soundOn;
    if (settingsIndex == 1) vibrateOn = !vibrateOn;
    if (settingsIndex == 2) enterState(MENU);
    saveSettings();
    chirp();
  }
  if (held) enterState(MENU);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
// Positions are in pixels: x counts across from the left edge (0-127), y counts
// DOWN from the top (0-63). Nothing appears until display.display() sends the
// finished picture.

// Draws `text` horizontally centered at the given y, for the given text size.
// Each character of the default font is 6 pixels wide at size 1.
void centerText(const String& text, int y, int textSize) {
  display.setTextSize(textSize);
  int x = (SCREEN_WIDTH - (int)text.length() * 6 * textSize) / 2;
  if (x < 0) x = 0;
  display.setCursor(x, y);
  display.print(text);
}

// One line of a menu. The highlighted line is drawn as black text on a white bar.
void menuLine(const String& text, int y, bool highlighted) {
  display.setTextSize(1);
  if (highlighted) {
    display.fillRect(0, y - 2, SCREEN_WIDTH, 12, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
  }
  display.setCursor(6, y);
  display.print(text);
  display.setTextColor(SH110X_WHITE);
}

// Milliseconds as "MM:SS". The + 999 rounds UP to the next whole second, so a
// 10 minute timer starts by showing 10:00, not 09:59.
String asMinutesSeconds(unsigned long ms) {
  unsigned long seconds = (ms + 999) / 1000;
  char text[16];
  snprintf(text, sizeof(text), "%02lu:%02lu", seconds / 60, seconds % 60);
  return String(text);
}

void drawScreen() {
  display.clearDisplay();

  if (state == MENU) {
    centerText("ElTech Desk Timer", 0, 1);
    display.drawLine(0, 10, SCREEN_WIDTH, 10, SH110X_WHITE);
    for (int i = 0; i < MENU_COUNT; i++) menuLine(MENU_ITEMS[i], 17 + i * 14, i == menuIndex);

  } else if (state == SET_TIME) {
    centerText("Set minutes", 0, 1);
    centerText(String(timerMinutes), 16, 4);
    centerText("press = start", 54, 1);

  } else if (state == RUNNING || state == PAUSED) {
    String title = "Timer";
    if (pomodoroMode) title = onBreak ? "Break" : "Work  #" + String(pomodorosDone + 1);
    if (state == PAUSED) title = "PAUSED";
    centerText(title, 0, 1);
    centerText(asMinutesSeconds(remainingMs), 16, 3);
    // Progress bar: an outline, filled from the left as time goes by.
    int barWidth = map(totalMs - remainingMs, 0, totalMs, 0, SCREEN_WIDTH - 4);
    display.drawRect(0, 46, SCREEN_WIDTH, 8, SH110X_WHITE);
    display.fillRect(2, 48, barWidth, 4, SH110X_WHITE);
    centerText(state == PAUSED ? "press = resume" : "press = pause", 56, 1);

  } else if (state == DONE) {
    // Flash the whole screen while the alert is going.
    bool inverted = (millis() / 400) % 2 == 0;
    if (inverted) display.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SH110X_WHITE);
    display.setTextColor(inverted ? SH110X_BLACK : SH110X_WHITE);
    centerText("TIME'S", 6, 2);
    centerText("UP!", 26, 2);
    String hint = "press = menu";
    if (pomodoroMode) hint = onBreak ? "press = work" : "press = break";
    centerText(hint, 52, 1);
    display.setTextColor(SH110X_WHITE);

  } else if (state == SETTINGS) {
    centerText("Settings", 0, 1);
    display.drawLine(0, 10, SCREEN_WIDTH, 10, SH110X_WHITE);
    menuLine(String("Sound:   ") + (soundOn ? "on" : "off"), 17, settingsIndex == 0);
    menuLine(String("Vibrate: ") + (vibrateOn ? "on" : "off"), 31, settingsIndex == 1);
    menuLine("Back", 45, settingsIndex == 2);
  }
  display.display();
}

// ---------------------------------------------------------------------------
// setup() runs once at power-on, loop() then runs forever
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Set each output to its OFF level BEFORE making the pin an output, so the
  // buzzer and motor don't switch on for an instant at power-on.
  setBuzzer(false);
  setMotor(false);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MOTOR_PIN, OUTPUT);

  // Is the knob connected? Its module pulls CLK and DT up to 3.3 V. With the
  // ESP32's own weak pull-DOWN switched on, the pins read HIGH only if the
  // module is really there.
  pinMode(ENC_CLK, INPUT_PULLDOWN);
  pinMode(ENC_DT, INPUT_PULLDOWN);
  delay(10);
  bool knobOK = digitalRead(ENC_CLK) == HIGH && digitalRead(ENC_DT) == HIGH;

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);   // the button has no pull-up of its own
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), knobTurned, FALLING);

  Wire.begin(I2C_SDA, I2C_SCL);
  bool oledOK = display.begin(OLED_ADDR, true);
  display.setTextColor(SH110X_WHITE);   // required, or no text is drawn
  display.setTextWrap(false);

  Preferences prefs;
  prefs.begin("timer", true);           // true = read only
  timerMinutes = constrain(prefs.getInt("minutes", 10), 1, MAX_TIMER_MINUTES);
  soundOn = prefs.getBool("sound", true);
  vibrateOn = prefs.getBool("vibrate", true);
  prefs.end();

  // The buzzer and motor can't tell the board they are there, so the test is
  // one you check yourself: one beep, one buzz.
  setBuzzer(true); delay(80);  setBuzzer(false);
  delay(150);
  setMotor(true);  delay(250); setMotor(false);

  Serial.println("========================================");
  Serial.println("           ElTech-Online");
  Serial.println("      ESP32 Desk Timer (BETA)");
  Serial.println("========================================");
  Serial.println("--- Self-test ---");
  Serial.print("OLED (SH1106): "); Serial.println(oledOK ? "OK" : "NOT FOUND");
  Serial.print("Rotary knob:   "); Serial.println(knobOK ? "OK" : "NOT FOUND");
  Serial.println("Buzzer:        beeped once (check by ear)");
  Serial.println("Motor:         buzzed once (check by touch)");
  Serial.print("RESULT:        "); Serial.println(oledOK && knobOK ? "PASS" : "FAIL");

  display.clearDisplay();
  display.drawBitmap((SCREEN_WIDTH - LOGO_WIDTH) / 2, 0, logo_bmp, LOGO_WIDTH, LOGO_HEIGHT, SH110X_WHITE);
  centerText("ElTech-Online", 36, 1);
  centerText("Desk Timer", 48, 1);
  display.display();
  delay(2000);

  enterState(MENU);
}

void loop() {
  readKnob();

  // Hand the knob to whichever state we are in.
  if (state == MENU)          runMenu();
  else if (state == SET_TIME) runSetTime();
  else if (state == RUNNING)  runRunning();
  else if (state == PAUSED)   runPaused();
  else if (state == DONE)     runDone();
  else if (state == SETTINGS) runSettings();

  // Redraw the screen 20 times a second. Sending a full picture to the OLED
  // takes a while, so doing it every time round loop() would make the knob
  // feel slow. ("static" makes lastDraw keep its value between runs of loop().)
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw >= 50) {
    lastDraw = millis();
    drawScreen();
  }
}
