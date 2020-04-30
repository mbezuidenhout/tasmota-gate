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

#define GATE_PULSE_COUNT    14     // How many pulses to keep track of
#define DEBOUNCE_DELAY      60    // How many milliseconds to give for debounce

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

struct GATE {
  bool enabled            = true;
  bool stateChanged       = false;
  uint8_t gateStatus      = GATE_UNKNOWN_STATE;
  uint8_t warnStatus      = GATE_WARN_NONE;
  uint16_t pulseWidths[GATE_PULSE_COUNT]; // even numbers are pin LOW odd numbers HIGH
} Gate;

bool Debounce(bool pinState) {
  static bool newPinState = LOW;
  static bool lastPinState = LOW;
  static unsigned long lastDebounceTime = millis();

  if (pinState != newPinState) {
    lastDebounceTime = millis();
    newPinState = pinState;
  }
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY && lastPinState != pinState ) {
    lastPinState = pinState;
  }
  return lastPinState;
}

void AddPulse(uint16_t *arr, bool pinState) {
  static unsigned long startMs;
  static bool prevPinState = LOW;
  unsigned long now = millis();
  unsigned long pulseWidth;

  if (now < startMs) { // handle unsigned long rollover
    startMs = now;
  }
  pulseWidth = now - startMs;

  if (pulseWidth > UINT16_MAX) { // Cap pulseWidth to UINT16_MAX or 65.5 seconds
    pulseWidth = UINT16_MAX;
  }

  if (pinState != prevPinState) {
    Gate.pulseWidths[prevPinState] = pulseWidth;
    prevPinState = pinState;
    startMs = now;
    if (HIGH == pinState) {
      RotateUIntArray(&Gate.pulseWidths[0]);
      Gate.pulseWidths[LOW] = 0;
      Gate.pulseWidths[HIGH] = 0;
    }
  } else {
    Gate.pulseWidths[pinState] = pulseWidth;
  }
}

void GateStatusRead(void)
{
  bool pinState;
  
  pinState = Debounce(digitalRead(pin[GPIO_GATE1_NP]));

  AddPulse(&Gate.pulseWidths[0], pinState);
  SetGateStatus(GetGateStatus(Gate.pulseWidths, pinState));
  SetGateWarning(GetWarning(Gate.pulseWidths));
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

bool HasWarning(uint16_t arr[]) {
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

uint8_t GetGateStatus(uint16_t arr[], bool pinState)
{
  static uint8_t prevState = GATE_UNKNOWN_STATE;
  uint16_t pulseInterval;
  if (arr[pinState] > GATE_SLOW_PULSE_H) {
    if (pinState == HIGH) {
      prevState = GATE_OPEN;
      return prevState;
    } else {
      prevState = GATE_CLOSED;
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

uint8_t GetWarning(uint16_t arr[])
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
    case 9:
      prevWarning = GATE_OBSTRUCTION;
      return GATE_OBSTRUCTION;
      break;
  }
  return prevWarning;
}

// Get the average time of the pulses.
// There should be at least 2 pulses before a value is returned.
long LedPulseInterval(uint16_t arr[])
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
  return round(pulseTimeTotal / pulseCount);
}

void RotateUIntArray(uint16_t *arr)
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
        ResponseAppend_P(PSTR(",\"Timings\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]"), Gate.pulseWidths[0], Gate.pulseWidths[1], Gate.pulseWidths[2], Gate.pulseWidths[3], Gate.pulseWidths[4],
          Gate.pulseWidths[5], Gate.pulseWidths[6], Gate.pulseWidths[7], Gate.pulseWidths[8], Gate.pulseWidths[9]);
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
        }
        sleep = 0;
        break;
      case FUNC_EVERY_50_MSECOND:
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
