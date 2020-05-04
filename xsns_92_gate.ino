/*
  xsns_92_gate.ino - Gate status sensor for sliding and swing gates

  Copyright (C) 2020  Marius Bezuidenhout

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef USE_XSNS_92_GATE
/*********************************************************************************************\
 * XSNS_92_GATE - Gate Status LED
 * 
 * Types of states
 * On                       Gate is open
 * Off                      Gate is closed
 * Slow flash               Gate is opening 300ms
 * Fast flash               Gate is closing 150ms
 * 1 flash per 1300 ms      Courtesy light is permanently on
 * 2 flashes per 1300 ms    Mains failure
 * 3 flashes per 1300 ms    Battery low
 * 4 flashes per 1300 ms    Repeated obstruction
 * 
 * In an error state are looking for a 1.3 second pulse interval (LED on or LED off)
 * followed by a up to 4 repeated pulses and ending with another 2 second interval.
 * Each warning pulse is about 240ms in duration
\*********************************************************************************************/

#define XSNS_92                  92

#define _is_between(x,y,z)  (x >= min(y,z) && x < max(y,z))

#ifndef UINT16_MAX
 #define UINT16_MAX 0xffff
#endif
#ifndef UINT8_MAX
 #define UINT8_MAX 0xff
#endif

#ifdef USE_WEBSERVER
const char HTTP_SNS_GATE_STATUS[] PROGMEM =
  "{s}Gate status {m}%s{e}";
const char HTTP_SNS_GATE_WARNING[] PROGMEM =
  "{s}Gate warning {m}%s{e}";
#endif // USE_WEBSERVER

const char GATE_STATUS_STR[] PROGMEM =
  "Unknown|Closed|Open|Closing|Opening";
const char GATE_WARNING_STR[] PROGMEM =
  "None|Courtesy light on|Mains failure|Battery low|Collision";

// Intervals in ms
#define GATE_SLOW_PULSE     300
#define GATE_FAST_PULSE     150
#define GATE_LED_WAIT       1275  // Time interval indicating an error state
#define SENSOR_ERROR_MARGIN 64    // How many milliseconds to allow for error
#define GATE_LED_WAIT_L     (GATE_LED_WAIT - 280)
#define GATE_LED_WAIT_H     (GATE_LED_WAIT + SENSOR_ERROR_MARGIN)
#define GATE_SLOW_PULSE_L   (GATE_SLOW_PULSE - SENSOR_ERROR_MARGIN)
#define GATE_SLOW_PULSE_H   (GATE_SLOW_PULSE + SENSOR_ERROR_MARGIN)
#define GATE_FAST_PULSE_L   (GATE_FAST_PULSE - SENSOR_ERROR_MARGIN)
#define GATE_FAST_PULSE_H   (GATE_FAST_PULSE + SENSOR_ERROR_MARGIN)

#define GATE_PULSE_COUNT    12     // How many pulses to keep track of

// Gate states
#define GATE_UNKNOWN_STATE  0
#define GATE_CLOSED         1
#define GATE_OPEN           2
#define GATE_CLOSING        3
#define GATE_OPENING        4
#define PLEASE_WAIT         0x80

// Warning states
#define GATE_WARN_NONE         0
#define GATE_COURTESY_LIGHT_ON 1
#define GATE_MAINS_FAILURE     2
#define GATE_BATTERY_LOW       3
#define GATE_OBSTRUCTION       4

#ifndef ARDUINO_ESP8266_RELEASE_2_3_0      // Fix core 2.5.x ISR not in IRAM Exception
void GateStatusRead(void) ICACHE_RAM_ATTR;
void AddPulse(volatile uint16_t *arr, bool pinState, uint32_t pulseWidth) ICACHE_RAM_ATTR;
void RotateUIntArray(volatile uint16_t *arr) ICACHE_RAM_ATTR;
#endif  // ARDUINO_ESP8266_RELEASE_2_3_0

volatile uint16_t pulseWidths[GATE_PULSE_COUNT]; // even numbers are pin LOW odd numbers HIGH

struct GATE {
  bool enabled            = true;
  bool stateChanged       = false;
  uint8_t gateStatus      = GATE_UNKNOWN_STATE;
  uint8_t warnStatus      = GATE_WARN_NONE;
} Gate;

void AddPulse(volatile uint16_t *arr, bool pinState, uint32_t pulseWidth) {
  static bool prevPinState = LOW;

  if (pulseWidth > UINT16_MAX) { // Cap pulseWidth to UINT16_MAX or 65.5 seconds
    pulseWidth = UINT16_MAX;
  }

  pulseWidths[pinState] = pulseWidth;
  if (HIGH == pinState && LOW == prevPinState) {
    RotateUIntArray(&pulseWidths[0]);
    pulseWidths[LOW] = 0;
    pulseWidths[HIGH] = 0;
  }
  prevPinState = pinState;
}

void GateStatusRead(void)
{
  static bool newPinState = LOW;
  static uint32_t lastDebounceTime = millis();

  if (millis() - lastDebounceTime > Settings.button_debounce) {
    AddPulse(&pulseWidths[0], newPinState, millis() - lastDebounceTime);    
    SetGateStatus(GetGateStatus(pulseWidths, newPinState));
    SetGateWarning(GetWarning(pulseWidths));
  }

  if (newPinState != digitalRead(pin[GPIO_GATE1_NP])) {
    lastDebounceTime = millis();
    newPinState = !newPinState;
  }
}

void SetGateStatus(uint8_t status)
{
  if (status != Gate.gateStatus) {
    if (GATE_UNKNOWN_STATE != status) {
      Gate.stateChanged = true;
    }
    Gate.gateStatus = status;
  }
}

void SetGateWarning(uint8_t warning)
{
  if (warning != Gate.warnStatus) {
    Gate.stateChanged = true;
    Gate.warnStatus = warning;
  }
}

bool HasWarning(volatile uint16_t arr[]) {
  if (arr[0] > GATE_LED_WAIT_H || arr[1] > GATE_LED_WAIT_H) {
    return false;
  }
  for (uint8_t i = 2; i < GATE_PULSE_COUNT; i++) {
    if (_is_between(arr[i], GATE_LED_WAIT_L, GATE_LED_WAIT_H)) {
      return true;
    }
  }
  return false;
}

uint8_t GetGateStatus(volatile uint16_t arr[], bool pinState)
{
  static uint8_t prevState = GATE_UNKNOWN_STATE;
  uint16_t pulseInterval;
  if (arr[pinState] > GATE_SLOW_PULSE_H) {
    if (pinState == HIGH) {
      prevState = GATE_OPEN;
      sleep = Settings.sleep;
      return prevState;
    } else {
      prevState = GATE_CLOSED;
      sleep = Settings.sleep;
      return prevState;
    }
  } else {
    if (!HasWarning(arr)) {
      pulseInterval = LedPulseInterval(arr);
      if (_is_between(pulseInterval,GATE_FAST_PULSE_L,GATE_FAST_PULSE_H)) {
        prevState = GATE_CLOSING;
        return prevState;
      } else if(_is_between(pulseInterval,GATE_SLOW_PULSE_L,GATE_SLOW_PULSE_H)) {
        prevState = GATE_OPENING;
        return prevState;
      }
    }
  }
  return prevState;
}

uint8_t GetWarning(volatile uint16_t arr[])
{
  static uint8_t prevWarning = GATE_WARN_NONE;
  bool warningState = false;
  uint8_t pulseCount = 0;
  uint16_t pulseTime;

  if (!HasWarning(arr)) {
    prevWarning = GATE_WARN_NONE;
    return GATE_WARN_NONE;
  }

  for (uint8_t i = 2; i < GATE_PULSE_COUNT; i++) {
    if (_is_between(arr[i], GATE_LED_WAIT_L, GATE_LED_WAIT_H)) {
      warningState = true;
    } else if (warningState) {
      if (arr[i] > 0 && arr[i] < GATE_SLOW_PULSE_H) {
        pulseCount++;
      } else {
        break;
      }
    }
  }

  switch (pulseCount) {
    case 1:
      prevWarning = GATE_COURTESY_LIGHT_ON;
      return GATE_COURTESY_LIGHT_ON;
      break;
    case 3:
      prevWarning = GATE_MAINS_FAILURE;
      return GATE_MAINS_FAILURE;
      break;
    case 5:
      prevWarning = GATE_BATTERY_LOW;
      return GATE_BATTERY_LOW;
      break;
    case 7:
      prevWarning = GATE_OBSTRUCTION;
      return GATE_OBSTRUCTION;
      break;
  }
  return prevWarning;
}

// Get the average time of the pulses.
// There should be at least 2 pulses before a value is returned.
uint16_t LedPulseInterval(volatile uint16_t arr[])
{
  uint16_t pulseTimeTotal = 0;
  uint8_t pulseCount = 0;

  for (uint16_t i = 2; i < GATE_PULSE_COUNT; i = i + 2) { // Take the duration of each set of LOW and HIGH states.
    if (arr[i] == 0 || arr[i] > GATE_LED_WAIT_L || arr[i + 1] == 0 || arr[i + 1] > GATE_LED_WAIT_L) {
      break;
    }
    pulseTimeTotal += arr[i] + arr[i + 1];
    pulseCount += 2;
  }
  if (pulseCount < 4) {
    return 0;
  }
  return (uint16_t) round(pulseTimeTotal / pulseCount);
}

void RotateUIntArray(volatile uint16_t *arr)
{
  for (uint8_t i = GATE_PULSE_COUNT - 1; i > 1; i--) {
    arr[i] = arr[i - 2];
  }
}

void GateStatusInit(void)
{
  Gate.enabled = false;
  if (pin[GPIO_GATE1_NP] < 99) {
    Gate.enabled = true;
    pinMode(pin[GPIO_GATE1_NP], INPUT);
    AddPulse(&pulseWidths[0], digitalRead(pin[GPIO_GATE1_NP]), UINT16_MAX);
    attachInterrupt(digitalPinToInterrupt(pin[GPIO_GATE1_NP]), GateStatusRead, CHANGE);

    GateStatusRead();
  }
/*  if (pin[GPIO_GATE1_TRG] < 99) {
    pinMode(pin[GPIO_GATE1_TRG], OUTPUT);
    digitalWrite(pin[GPIO_GATE1_TRG], HIGH);
  }*/
}

  // {"Gate":{"Status":Open,Warning":Battery Low}}
void GateStatusShow(bool json)
{
  char gateStatusString[8];
  char gateWarningString[18];
  GetTextIndexed(gateStatusString, sizeof(gateStatusString), Gate.gateStatus, GATE_STATUS_STR);
  GetTextIndexed(gateWarningString, sizeof(gateWarningString), Gate.warnStatus, GATE_WARNING_STR);

  if (json) {
      ResponseAppend_P(PSTR(",\"Gate\":{\"Status\":\"%s\",\"Warning\":\"%s\""), gateStatusString, gateWarningString);
      //if (Gate.stateChanged && (GATE_OPEN == Gate.gateStatus || GATE_CLOSED == Gate.gateStatus)) { // Append the pulse timings if the gate is open or closed
        ResponseAppend_P(PSTR(",\"Timings\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]"), pulseWidths[0], pulseWidths[1], pulseWidths[2], pulseWidths[3], pulseWidths[4],
          pulseWidths[5], pulseWidths[6], pulseWidths[7], pulseWidths[8], pulseWidths[9]);
      //}
      ResponseAppend_P(PSTR("}"));
#ifdef USE_WEBSERVER
  } else {
    WSContentSend_PD(HTTP_SNS_GATE_STATUS, gateStatusString);
    if (Gate.warnStatus != GATE_WARN_NONE) {
      WSContentSend_PD(HTTP_SNS_GATE_WARNING, gateWarningString);
    }
#endif  // USE_WEBSERVER
  }
}

/*********************************************************************************************\
   Interface
  \*********************************************************************************************/

bool Xsns92(uint8_t function)
{
  bool result = false;

  if (Gate.enabled) {
    switch (function) {
      case FUNC_INIT:
        GateStatusInit();
        break;
      case FUNC_LOOP:
        if (Gate.stateChanged && GATE_UNKNOWN_STATE != Gate.gateStatus) {
          MqttPublishSensor();
          Gate.stateChanged = false;
        } else {
          sleep = Settings.sleep;
        }
        break;
      case FUNC_EVERY_SECOND:
        GateStatusRead();
        break;
      case FUNC_JSON_APPEND:
        GateStatusShow(1);
        break;
  #ifdef USE_WEBSERVER
      case FUNC_WEB_SENSOR:
        GateStatusShow(0);
        break;
  #endif  // USE_WEBSERVER
    }
  }
  
  return result;
}

#endif // USE_XSNS_92_GATE
