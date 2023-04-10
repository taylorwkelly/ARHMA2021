/**
   @file CAN.ino
     @author    Washington Superbike
     @date      1-March-2023
     @brief
          The CAN.ino file operates the CAN bus for the bike's firmware. This
          updates all the variables in the CAN.h file using the various helper
          methods. Really simple. Just reads in the message using the CAN bus
          and then updates them according to the values derived from the datasheets.
    \note
      Dont mess with this too much. It's great.

    \todo
      Change the "if CAN_NODES != 0" to be an ifndef statement in the start.
      That means the checkCAN and requestCellVoltages lines will not execute unless
      CAN_NODES is a non-zero number in Main.h
      before compiling.
      \n \n
      Potentially, check if the data is actually being updated by HIL. There is
      no real instantiation of associating the CAN variables with the 
      messages being read in in the canTask(), so maybe call on those methods
      to read in those values, in case it doesnt work.
      \n \n
      Goal 3.
      \n \n
      Final Goal.
*/
#include "CAN.h"
#include "FreeRTOS_TEENSY4.h"

void canTask(void *canData) {
  int iter = 0;
  int requestCells = 1;
  while (1) {
    /// First canTask() checks for new incoming messages

    /// NOTE: CAN breaks if we try sending messages with 0 other nodes on the bus
    /// as there is no node to 'ACK' our message. Therefore,
    /// change CAN_NODES in Main.h to make sure things dont break.
    if (CAN_NODES !=0) {
      checkCAN(*(CANTaskData *)canData);
      if (iter == (1000 / 20) * 2) {
         /// Ask for other half of cell voltages from BMS every 2 seconds
          requestCellVoltages(requestCells);
          requestCells *= -1;
          iter = 0;
        }
        iter++;
    }
    // delay 20ms
    vTaskDelay((20 * configTICK_RATE_HZ) / 1000);
  }
}

void setupCAN() {
  CAN_bus.begin();
  CAN_bus.setBaudRate(250000);
}

void decipherEVCCStats(CAN_message_t msg, ChargeControllerStats evccStats) {
  *(evccStats.en) = (msg.buf[0]);
  *(evccStats.chargeVoltage) = ((msg.buf[2] << 8) | msg.buf[1]) / 10.0;
  *(evccStats.chargeCurrent) = (3200 - ((msg.buf[4] << 8) | msg.buf[3])) / 10.0;
}

void decipherChargerStats(CAN_message_t msg, ChargerStats chargerStats) {
  *(chargerStats.statusFlag) = msg.buf[0];
  *(chargerStats.chargeFlag) = msg.buf[1];
  *(chargerStats.outputVoltage) = ((msg.buf[3] << 8) | msg.buf[2]) / 10.0;
  *(chargerStats.outputCurrent) = (3200 - ((msg.buf[5] << 8) | msg.buf[4])) / 10.0;
  *(chargerStats.chargerTemp) = msg.buf[6] - 40;
}

void decodeMotorStats(CAN_message_t msg, MotorStats motorStats ) {
  *(motorStats.RPM) = (float) ((msg.buf[1] << 8) | msg.buf[0]);
  *(motorStats.motorCurrent) = ((msg.buf[3] << 8) | msg.buf[2]) / 10.0;
  *(motorStats.motorControllerBatteryVoltage) = ((msg.buf[5] << 8) | msg.buf[4]) / 10.0;
  *(motorStats.errorMessage) = ((msg.buf[7] << 8) | msg.buf[6]);
}

void decodeMotorTemps(CAN_message_t msg, MotorTemps motorTemps) {
  *(motorTemps.throttle) = msg.buf[0] / 255.0;
  *(motorTemps.motorControllerTemperature) = msg.buf[1] - 40;
  *(motorTemps.motorTemperature) = msg.buf[2] - 30;
  *(motorTemps.controllerStatus) = msg.buf[4];
}

void decipherBMSStatus(CAN_message_t msg, BMSStatus bmsStatus) {
  *(bmsStatus.bms_status_flag) = msg.buf[0];
  *(bmsStatus.bms_c_id) = msg.buf[1];
  *(bmsStatus.bms_c_fault) = msg.buf[2];
  *(bmsStatus.ltc_fault) = msg.buf[3];
  *(bmsStatus.ltc_count) = msg.buf[4];
}

void decipherCellsVoltage(CAN_message_t msg, CellVoltages cellVoltages) {
  // THE FOLLOWING DATATYPE NEEDS TO BE CHANGED
  uint32_t msgID = msg.id;
  int totalOffset = 0; // totalOffset equals the index of array cellVoltages
  int cellOffset = (((msgID >> 8) & 0xF) - 0x9);
  int ltcOffset = (msgID & 0x1);
  totalOffset = (cellOffset * 4) + (ltcOffset * 12);
  int cellIndex;

  for (cellIndex = 0; cellIndex < 8; cellIndex += 2) {
    // TODO: analyze this line and find a better way to do it
    *(cellVoltages.cellVoltages + (cellIndex / 2 + totalOffset)) = ((((float)(msg.buf[cellIndex + 1] << 8) + (float)(msg.buf[cellIndex]) / 10000)) / 10000) ;
    cellVoltagesReady[cellIndex / 2 + totalOffset] = true;
  }

  calculateSeriesVoltage(cellVoltages);

  if (!*cellVoltages.ready) {
    for (int i = 0; i < BMS_CELLS; i++ ) {
      if (!cellVoltagesReady[i]) {
        return;
      }
    }
    *cellVoltages.ready = true;
  }
}

void decipherThermistors(CAN_message_t msg, ThermistorTemps thermistorTemps, byte currentMuxSelects) {
  byte ltcID = msg.buf[0];
  thermistorEnabled = msg.buf[1];
  thermistorPresent = msg.buf[2];
  byte *currentThermistor = &msg.buf[3];
  int thermistor;
  for (thermistor = 0; thermistor < 5; thermistor++) {
    thermistorTemps.temps[thermistor + 5 * ltcID + 10 * currentMuxSelects] = currentThermistor[thermistor];
  }
}

// sums the voltage of each cell in main accumulator
void calculateSeriesVoltage(CellVoltages cellVs) {
  float partialSeriesVoltage = 0;
  int currentCell;
  for (currentCell = 0; currentCell < BMS_CELLS; currentCell++) {
    partialSeriesVoltage += *(cellVs.cellVoltages + currentCell);
  }
  *cellVs.seriesVoltage = partialSeriesVoltage;
}

// checks the can bus for any new data
void checkCAN(CANTaskData canData) {
  int readValue = CAN_bus.read(CAN_msg);
  if (readValue != 0) { // if we read a message
    switch (CAN_msg.id) {
      case MOTOR_STATS_MSG:
        decodeMotorStats(CAN_msg, canData.motorStats);
        break;
      case MOTOR_TEMPS_MSG:
        decodeMotorTemps(CAN_msg, canData.motorTemps);
        break;
      case DD_BMS_STATUS_IND:
        decipherBMSStatus(CAN_msg, canData.bmsStatus);
        //printBMSStatus();
        break;
      case EVCC_STATS:
        decipherEVCCStats(CAN_msg, canData.chargeControllerStats);
        break;
      case CHARGER_STATS:
        decipherChargerStats(CAN_msg, canData.chargerStats);
      case BMSC1_LTC1_CELLS_04:
        decipherCellsVoltage(CAN_msg,  canData.cellVoltages);
        break;
      case BMSC1_LTC1_CELLS_58:
        decipherCellsVoltage(CAN_msg,  canData.cellVoltages);
        break;
      case BMSC1_LTC1_CELLS_912:
        decipherCellsVoltage(CAN_msg,  canData.cellVoltages);
        break;
      case BMSC1_LTC2_CELLS_04:
        decipherCellsVoltage(CAN_msg,  canData.cellVoltages);
        break;
      case BMSC1_LTC2_CELLS_58:
        decipherCellsVoltage(CAN_msg,  canData.cellVoltages);
        break;
      case BMSC1_LTC2_CELLS_912:
        decipherCellsVoltage(CAN_msg,  canData.cellVoltages);
        break;
      case DD_BMSC_TH_STATUS_IND:
        decipherThermistors(CAN_msg, canData.thermistorTemps, *(canData.currentMuxSelects));
        break;
    }
  }
}

// unused currently but should be implemented into the current firmware
void printBMSStatus() {
  switch (bms_status_flag) {
    case 1:
      Serial.printf("at least one cell V is > High Voltage Cutoff\n");
      break;
    case 2:
      Serial.printf("at least one cell V is < Low Voltage Cutoff\n");
      break;
    case 4:
      Serial.printf("at least one cell V is > Balance Voltage Cutoff\n");
      break;
  }
  Serial.printf("The BMSC ID is %d\n", bms_c_id);
  switch (bms_c_fault) {
    case 1:
      Serial.printf("BMS Fault: configuration not locked\n");
      break;
    case 2:
      Serial.printf("BMS Fault: not all cells present\n");
      break;
    case 4:
      Serial.printf("BMS Fault: thermistor overtemp\n");
      break;
    case 8:
      Serial.printf("BMS Fault: not all thermistors present\n");
      break;
  }
  if (ltc_fault == 1) {
    Serial.printf("LTC fault detected\n");
  }
  Serial.printf("%d LTCs detected\n");
}

// used to print the contents of a CAN msg
void printMessage(CAN_message_t msg) {
  for (int i = 0; i < msg.len; i++) {
    Serial.print(msg.buf[i]);
    Serial.print(":");
  }
  Serial.println();
}

// should only be called if there is at least one other CAN node on the bus
// if none are on the bus when CAN_bus.write() occurs, the Teensy will reset
void requestCellVoltages(int LTC) {
  if (LTC == -1) {
    CAN_msg.id = 0x01de0800;
    CAN_bus.write(CAN_msg);
  } else if (LTC == 1) {
    CAN_msg.id = 0x01de0801;
    CAN_bus.write(CAN_msg);
  }
}
