#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "ssd1306h.h"
#include "MAX30102.h"
#include "Pulse.h"
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <pgmspace.h>

// Pins0
#define LED LED_BUILTIN
#define ONE_WIRE_BUS D3  // GPIO0 (D3 for DS18B20)

// Display & Sensor Objects
SSD1306 oled;
MAX30102 sensor;
Pulse pulseIR;
Pulse pulseRed;
MAFilter bpm;

// DS18B20 Setup
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
float tempC = 0.0;

const uint8_t spo2_table[184] PROGMEM = {
  // (same values)
  95,95,95,96,96,96,97,97,97,97,97,98,98,98,98,98,99,99,99,99,
  99,99,99,99,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,
  100,100,100,100,99,99,99,99,99,99,99,99,98,98,98,98,98,98,97,97,
  97,97,96,96,96,96,95,95,95,94,94,94,93,93,93,92,92,92,91,91,
  90,90,89,89,89,88,88,87,87,86,86,85,85,84,84,83,82,82,81,81,
  80,80,79,78,78,77,76,76,75,74,74,73,72,72,71,70,69,69,68,67,
  66,66,65,64,63,62,62,61,60,59,58,57,56,56,55,54,53,52,51,50,
  49,48,47,46,45,44,43,42,41,40,39,38,37,36,35,34,33,31,30,29,
  28,27,26,25,23,22,21,20,19,17,16,15,14,12,11,10,9,7,6,5,
  3,2,1
};

class Waveform {
public:
  Waveform() { wavep = 0; }

  void record(int waveval) {
    waveval = constrain((waveval / 8) + 128, 0, 255);
    waveform[wavep] = waveval;
    wavep = (wavep + 1) % MAXWAVE;
  }

  void scale() {
    uint8_t maxw = 0, minw = 255;
    for (uint8_t i = 0; i < MAXWAVE; i++) {
      maxw = max(maxw, waveform[i]);
      minw = min(minw, waveform[i]);
    }
    uint8_t scale8 = (maxw - minw) / 4 + 1;
    uint8_t index = wavep;
    for (uint8_t i = 0; i < MAXWAVE; i++) {
      disp_wave[i] = 31 - ((uint16_t)(waveform[index] - minw) * 8) / scale8;
      index = (index + 1) % MAXWAVE;
    }
  }

  void draw(uint8_t X) {
    for (uint8_t i = 0; i < MAXWAVE; i++) {
      oled.drawPixel(X + i, disp_wave[i]);
    }
  }

private:
  static const uint8_t MAXWAVE = 72;
  uint8_t waveform[MAXWAVE], disp_wave[MAXWAVE], wavep;
};

Waveform wave;
int beatAvg = 0, SPO2 = 0, SPO2f = 0;
bool drawRed = false, led_on = false, filter = false;
unsigned long lastBeat = 0, displaytime = 0;

void draw_digit(int x, int y, long val, char c = ' ', uint8_t field = 3, const int BIG = 2) {
  uint8_t ff = field;
  do {
    char ch = (val != 0) ? val % 10 + '0' : c;
    oled.drawChar(x + BIG * (ff - 1) * 6, y, ch, BIG);
    val /= 10;
  } while (--ff > 0);
}

void draw_oled(int msg) {
  oled.firstPage();
  do {
    switch (msg) {
      case 0:
        oled.drawStr(10, 0, "Device error", 1);
        break;
      case 1:
        oled.drawStr(13, 10, "PLACE", 1);
        oled.drawStr(10, 20, "FINGER", 1);
        break;
      case 2:
        draw_digit(86, 0, beatAvg);
        wave.draw(8);
        draw_digit(98, 16, SPO2f, ' ', 3, 1); oled.drawChar(116, 16, '%');
        draw_digit(98, 24, SPO2, ' ', 3, 1); oled.drawChar(116, 24, '%');
        oled.drawStr(0, 56, "T: ");
        oled.drawNum(18, 56, tempC, 1); oled.drawStr(45, 56, "C");
        break;
    }
  } while (oled.nextPage());
}

void setup() {
  pinMode(LED, OUTPUT);
  oled.init();
  oled.fill(0x00);
  tempSensor.begin();
  draw_oled(1);
  delay(3000);

  if (!sensor.begin()) {
    draw_oled(0);
    while (1);
  }

  sensor.setup();
}

void loop() {
  sensor.check();
  if (!sensor.available()) return;

  tempSensor.requestTemperatures(); 
  tempC = tempSensor.getTempCByIndex(0);

  uint32_t irValue = sensor.getIR(), redValue = sensor.getRed();
  sensor.nextSample();

  if (irValue < 5000) {
    draw_oled(1);
    delay(200);
    return;
  }

  unsigned long now = millis();
  int16_t IR_signal = filter ? pulseIR.ma_filter(pulseIR.dc_filter(irValue)) : pulseIR.dc_filter(irValue);
  int16_t Red_signal = filter ? pulseRed.ma_filter(pulseRed.dc_filter(redValue)) : pulseRed.dc_filter(redValue);
  bool beatIR = pulseIR.isBeat(IR_signal);
  bool beatRed = pulseRed.isBeat(Red_signal);

  wave.record(drawRed ? -Red_signal : -IR_signal);

  if ((drawRed ? beatRed : beatIR)) {
    long bpm_val = 60000 / (now - lastBeat);
    if (bpm_val > 0 && bpm_val < 200) beatAvg = bpm.filter(bpm_val);
    lastBeat = now;
    digitalWrite(LED, HIGH);
    led_on = true;

    long numerator = (pulseRed.avgAC() * pulseIR.avgDC()) / 256;
    long denominator = (pulseRed.avgDC() * pulseIR.avgAC()) / 256;
    int RX100 = (denominator > 0) ? (numerator * 100) / denominator : 999;
    SPO2f = (10400 - RX100 * 17 + 50) / 100;
    if (RX100 >= 0 && RX100 < 184) SPO2 = pgm_read_byte_near(&spo2_table[RX100]);
  }

  if (now - displaytime > 50) {
    displaytime = now;
    wave.scale();
    draw_oled(2);
  }

  if (led_on && (now - lastBeat) > 25) {
    digitalWrite(LED, LOW);
    led_on = false;
  }
}
