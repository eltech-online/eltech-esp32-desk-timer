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

// Changed inside the interrupt, so it must be "volatile": that tells the
// compiler the value can change at any moment.
volatile int counter = 0;
volatile unsigned long lastTurnUs = 0;

// This function is an INTERRUPT handler. The board runs it the instant the CLK
// pin falls from HIGH to LOW, whatever else it was doing. At that moment DT is
// still HIGH if the knob is turning one way and already LOW for the other.
// (IRAM_ATTR keeps the function in fast memory, which interrupts need.)
void IRAM_ATTR knobTurned() {
  unsigned long now = micros();              // microseconds since power-on
  if (now - lastTurnUs < 1500) return;       // a switch "bounces" for a moment: ignore that
  lastTurnUs = now;
  if (digitalRead(ENC_DT) == HIGH) counter = counter + 1;
  else counter = counter - 1;
}

// setup() runs once, when the board is powered on or reset.
void setup() {
  Serial.begin(115200);
  // INPUT_PULLUP holds a pin HIGH until a switch connects it to GND.
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  // "Run knobTurned() every time ENC_CLK falls."
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), knobTurned, FALLING);
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
