// ElTech-Online ESP32 Desk Timer — rotary knob test
//
// BETA: this sketch compiles but has not been fully tested on hardware yet.
//
// The smallest useful sketch for the KY-040 rotary encoder (the knob): it
// keeps a number that goes up when you turn one way and down the other, and
// prints it to Serial Monitor. Pressing the knob sets it back to 0. No
// display, no buzzer. Use it to check the knob is wired correctly.
//
// Wiring (5 wires):
//   knob +   -> ESP32-C3 3V3
//   knob GND -> ESP32-C3 GND
//   knob CLK -> ESP32-C3 GPIO 5
//   knob DT  -> ESP32-C3 GPIO 6
//   knob SW  -> ESP32-C3 GPIO 7
//
// Number going the wrong way? Swap the CLK and DT wires.

#define ENC_CLK 5
#define ENC_DT  6
#define ENC_SW  7

// Some knobs give one click per two moves of their switches, others one click
// per four. Only every second click counts? Use true. Each click counts twice?
// Use false.
const bool ENC_TWO_MOVE_CLICKS = true;

// Inside the knob are two switches, CLK and DT, that open and close one after
// the other as it turns. A full cycle takes both wires through four small
// moves and back to where they started (both HIGH):
//
//   one way:    CLK falls, DT falls, CLK rises, DT rises
//   other way:  DT falls, CLK falls, DT rises, CLK rises
//
// Real switches BOUNCE: for a millisecond the contact chatters, so one move
// looks like several, forwards and backwards. Watching only CLK and peeking
// at DT gets fooled by that, most of all on a slow turn (the count steps up,
// down, up...). So the sketch follows every move of BOTH wires and adds +1 or
// -1 for each. A bounce is one move forward and one back, which adds up to
// nothing. Only a whole click, every move of it the same way, counts as a
// step. A click is two moves on some knobs (they rest with both wires HIGH and
// again with both LOW) and all four on others: see ENC_TWO_MOVE_CLICKS.
//
// Both wires are watched by an INTERRUPT: the board drops whatever it is
// doing, runs knobTurned(), and carries on, so no click is missed.
// "volatile" tells the compiler the value can change at any moment.
// (IRAM_ATTR keeps the function in fast memory, which interrupts need.)
volatile int counter = 0;

// What each move is worth. The row is picked by the old and new readings of the
// two wires put together as one number: old CLK, old DT, new CLK, new DT.
// 0 = nothing changed, or a move that can't really happen (noise).
const int8_t KNOB_MOVES[16] = { 0, -1, 1, 0,  1, 0, 0, -1,  -1, 0, 0, 1,  0, 1, -1, 0 };

void IRAM_ATTR knobTurned() {
  static uint8_t last = 3;                  // both wires HIGH = knob at rest
  static int8_t moves = 0;                  // moves since the last rest
  const int8_t need = ENC_TWO_MOVE_CLICKS ? 2 : 3;
  uint8_t now = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  moves += KNOB_MOVES[(last << 2) | now];
  last = now;
  bool atRest = now == 3 || (ENC_TWO_MOVE_CLICKS && now == 0);
  if (atRest) {                             // at rest: was it a whole click?
    int step = 0;
    if (moves >= need) step = 1;
    if (moves <= -need) step = -1;
    counter += step;
    moves = 0;
  }
}

// setup() runs once, when the board is powered on or reset.
void setup() {
  Serial.begin(115200);
  // INPUT_PULLUP holds a pin HIGH until a switch connects it to GND.
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  // "Run knobTurned() every time ENC_CLK or ENC_DT changes."
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), knobTurned, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT), knobTurned, CHANGE);
  Serial.println("Knob test - turn it, and press it to reset to 0");
}

// loop() runs over and over, forever.
void loop() {
  if (digitalRead(ENC_SW) == LOW) counter = 0;   // pressed: SW is connected to GND

  // Print only when the number changes. ("static" makes lastPrinted keep its
  // value between one run of loop() and the next.)
  static int lastPrinted = -999;
  int now = counter;
  if (now != lastPrinted) {
    lastPrinted = now;
    Serial.print("Counter: ");
    Serial.println(now);
  }
  delay(10);
}
