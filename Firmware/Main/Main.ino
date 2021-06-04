#include "CAN.h"
#include "Display.h"
#include "Main.h"
#include "Precharge.h"
#include "DataLogging.h"
#include "Scheduler.h"

#define DATA_LOG_ENABLE 0

static int bms_status_flag = 0;
static int bms_c_id = 0;
static int bms_c_fault = 0;
static int ltc_fault = 0;
static int ltc_count = 0;
static float cellVoltagesArr[BMS_CELLS];  // voltages starting with the first LTC
static float seriesVoltage;
static float thTemps[10];       // assuming a message with 7 LTCs
static int thermistorEnabled;          // assuming only 2 LTCs
static int thermistorPresent;

static float auxiliaryBatteryVoltage = 0;

static float RPM = 0;
static float motorCurrent = 0;
static float motorControllerBatteryVoltage = 0;
static float throttle = 0;
static float motorControllerTemp = 0;
static float motorTemp = 0;
static int errorMessage = 0;
static byte controllerStatus = 0;
static byte switchSignalsStatus = 0;

static float stateOfCharge;
static int stateOfChargeMemoryAddress;
static float oldDischargingCurrent;
static float newDischargingCurrent; // used motorCurrent instead
static float oldChargingCurrent;
static float newChargingCurrent;
static float batteryCapacity;

static PC_STATE PC_State; 
static SOC_STATE SOC_State; 

static Screen screen = {};

static MeasurementScreenData measurementData = {};
static MotorStats motorStats = {};
static MotorTemps motorTemps = {};
static CellVoltages cellVoltages = {};
static PreChargeTaskData preChargeData = {};
static BMSStatus bmsStatus = {};
static ThermistorTemps thermistorTemps = {};
static SOCTaskData socData = {};

static CSVWriter motorTemperatureLog = {};
static CSVWriter motorControllerTemperatureLog = {};
static CSVWriter motorControllerVoltageLog = {};
static CSVWriter motorCurrentLog = {};
static CSVWriter rpmLog = {};
static CSVWriter thermistorLog = {};
static CSVWriter bmsVoltageLog = {};
static CSVWriter *logs[] = {&motorTemperatureLog, &motorControllerTemperatureLog, &motorControllerVoltageLog, &motorCurrentLog, &rpmLog, &thermistorLog, &bmsVoltageLog};


unsigned long timer = millis();
int cycleCount = 0;

int lowerUpperCells = -1;
unsigned long ms = millis();
byte sdStarted = 0;

void initializeLogs() {
  motorTemperatureLog = {MOTOR_TEMPERATURE_LOG, 1, &motorTemp, FLOAT};
  motorControllerTemperatureLog = {MOTOR_CONTROLLER_TEMPERATURE_LOG, 1, &motorControllerTemp, FLOAT};
  motorControllerVoltageLog = {MOTOR_CONTROLLER_VOLTAGE_LOG, 1, &motorControllerBatteryVoltage, FLOAT};
  motorCurrentLog = {MOTOR_CURRENT_LOG, 1, &motorCurrent, FLOAT};
  rpmLog = {RPM_LOG, 1, &RPM, FLOAT};
  thermistorLog = {THERMISTOR_LOG, 10, &thTemps[0], FLOAT};
  bmsVoltageLog = {BMS_VOLTAGE_LOG, 1, &seriesVoltage, FLOAT};
}

void initializeCANStructs() {
  motorStats = {&RPM, &motorCurrent, &motorControllerBatteryVoltage, &errorMessage};
  motorTemps = {&throttle, &motorControllerTemp, &motorTemp, &controllerStatus};
  cellVoltages = {&cellVoltagesArr[0]};
  bmsStatus = { &bms_status_flag, &bms_c_id, &bms_c_fault, &ltc_fault, &ltc_count};
  thermistorTemps = {thTemps}; // IS THIS MISSING AN &??
}

void initializePreChargeStruct() {
  preChargeData = {&seriesVoltage, &PC_State, &motorControllerBatteryVoltage};
}

void initializeSOCStruct() {
  socData = {&SOC_State, &stateOfCharge, &stateOfChargeMemoryAddress, &oldDischargingCurrent, 
    &motorCurrent, &oldChargingCurrent, &newChargingCurrent, &batteryCapacity}; // 
  *socData.newDischargingCurrent = -1.0; // DUMMY VALUE, SHOULD BE DISREGARDABLE
  *socData.newChargingCurrent = -1.0;
  *socData.SOC_State = SOC_START;
// A CHECK IS NECESSARY HERE TO DETERMINE IF THE EEPROM actually contains the SoC and Capacity
  *socData.stateOfCharge = readDoubleFromEEPROM(0); // see function below
  *socData.batteryCapacity = readDoubleFromEEPROM(8);
}

// This is a function that will read all 8 bytes of a double from the EEPROM and return it. 
// function needs checking
double readDoubleFromEEPROM(int startAddress) {
  double readDouble = 0;
  for (int currentByte = startAddress; currentByte < startAddress + 7; currentByte++) {
    readDouble &= EEPROM.read() << currentByte;
  }
  return readDouble;
}

/ needs checking
void writeDoubleToEEPROM(int startAddress, double data) {
  byte currentByte;
  for (int currentAddress = startAddress; currentAddress < startAddress + 7; currentAddress++) {
    currentByte = data >> (startAddress + 7 - currentAddress);
    EEPROM.write(currentAddress, currentBtye);
  }
}

void setup() {
  pinMode(3, OUTPUT);
  digitalWrite(3, LOW);
  pinMode(4, OUTPUT);
  digitalWrite(3, LOW);
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  pinMode(TS_CS, OUTPUT);
  digitalWrite(TS_CS, HIGH);
  pinMode(16, OUTPUT);
  pinMode(16, LOW);
  //motor temp points to motor controller temp for now
  measurementData = {&seriesVoltage, &motorControllerBatteryVoltage, &auxiliaryBatteryVoltage, &RPM, &motorControllerTemp, &motorCurrent, &errorMessage, thTemps};
  initializeCANStructs();
  // initial
  initializeLogs();
  Serial.print("Starting SD: ");
  if (startSD()) {
    Serial.println("SD successfully started");
    sdStarted = 1;
  } else {
    sdStarted = 0;
    Serial.println("Error starting SD card");
  }
  setupDisplay(screen);
  setupCAN();
  initializePreChargeStruct();
  setupFastTimerISR();
  setupSlowTimerISR(preChargeData);
}

void loop() {
  if (fastTimerFlag == 1) { // 20 ms interval
    fastTimerFlag == 0;
    // UPDATE NEW OLD CURRENT TO NEW CURRENT HERE for socData (charging and discharging current)
    canTask({motorStats, motorTemps, bmsStatus, thermistorTemps, cellVoltages,  &seriesVoltage}); // how many messages does this process at a time? It may be necessary to track the increment between the old and new currents
    if (fastTimerIncrement % 2 == 0) { // 40 ms interval
      // ADD FUNCTION HERE TO TICK THE PRECHARGE FSM
      preChargeCircuitFSMTransitionActions(preChargeData, bmsStatus, motorTemps);
      preChargeCircuitFSMStateActions(preChargeData);
    }
  }
  if (fastTimerIncrement % 5 == 0 && sdStarted) {// 1 second interval
    dataLoggingTask({logs, 7});
  }
  if (slowTimerFlag == 1) { // 500 ms interval
    slowTimerFlag = 0;
    displayTask(measurementData, screen);
    if (slowTimerIncrement % 4 == 0) { // 2 second interval
      requestCellVoltages(lowerUpperCells);
      lowerUpperCells *= -1;
    }
    if (slowTimerIncrement % 20 == 0 && sdStarted) { // 10 s interval
      saveFiles(logs, 7);
      Serial.println("saved logging files");
      writeDoubleToEEPROM(0, socData.stateOfCharge);
      writeDoubleToEEPROM(8, socData.batteryCapacity);
    }

  }
}
