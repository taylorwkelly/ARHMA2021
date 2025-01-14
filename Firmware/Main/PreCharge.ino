/**
   @file PreCharge.ino
     @author    Washington Superbike
     @date      1-March-2023
     @brief
          The PreCharge.ino file for the PreCharge/Controls task for the bike's
          firmware. This calls on the stats that are updated
          by CAN and then updates the state of controls (HV Off, ON, etc.)
          based on them. Pretty simple. This also reads in the gyroscope
          data to determine when to kill power to the bike.


    \note
      up all members to be able to use it without any trouble.

    \todo
      Goal 1.
      \n \n
      Goal 2.
      \n \n
      Goal 3.
      \n \n
      Final Goal.
*/
#include "Precharge.h"
#include <Wire.h>

// I2C is incredibly unstable? Or perhaps not using proper wiring causes this,
// but the reading in precharge data can often bug out and output
// "nan" because of randomness? I would personally recommend
// having some sort of true or false based
// indicator at the bottom right of the speedometer screen
// that outputs "true" or something to indicate
// that the gyro is not bugging out. Or perhaps
// output the gyro data on the bottom right
// to indicate danger? Something like that.

// OKAY, MAKE SURE YOU READ THIS IF YOU SEE ISSUES WITH THE GYRO.
// By my *limited* understanding, I think the problem is that
// the gyro needs to be consistently powered, hence why proper
// gyro setup code has a significant delay between
// turning the thing on and actually reading data from it.
// An easy way to work around it, is to power on the Teensy
// and then once everything is up and running,
// reprogram it by using the button on the board.
// In the case of the actual race, I would turn on low-voltage
// and then wait a second and then turn it off and then
// turn it back on.

// The state HV_PRECHARGING, HV_ON are badly named.
// The enum should be renamed to HV_STATE
// and instead we should have HV_OFF, HV_ON, HV_ON states

void prechargeTask(void *taskData) {
  PreChargeTaskData prechargeData = *(PreChargeTaskData *)taskData;
  while (1) {
    preChargeCircuitFSMStateActions(prechargeData);
    preChargeCircuitFSMTransitions(prechargeData);
    Serial.println(*preChargeData.angle_X);
    Serial.println(*preChargeData.angle_Y);
    updateGyroData(preChargeData);

    // 100 ms should be unnoticeable compared to other task updates
    // but should be fast to pick up errors / switch updates
    vTaskDelay((1 * configTICK_RATE_HZ) / 1000);
  }
}

// NOTE: "input" needs to change to the GPIO value for the on-button for the bike
void preChargeCircuitFSMTransitions (PreChargeTaskData preChargeData) {
  HV_STATE old_state = hv_state;
  switch (hv_state) { // transitions
    case HV_OFF:
      if (check_HV_TOGGLE()) {
        hv_state = HV_PRECHARGING;
      }
      break;
    case HV_PRECHARGING:
      if (!check_HV_TOGGLE()) {
        // kill-switch activated or HV switch turned off
        hv_state = HV_OFF;
      }
      else if (!isHVSafe(preChargeData)) {
        // HV error detected
        hv_state = HV_ERROR;
      }
      else if (isPrecharged(preChargeData)) {
        // finished precharging
        hv_state = HV_ON;
      }
      else {
        // no updates, keep precharging
        hv_state = HV_PRECHARGING;
      }
      break;
    case HV_ON:
      //  TODO: add another && next to the !check_HV_TOGGLE, that essentially checks the measured angle > 45 degrees on left or right side (+- 45 degrees?)
      if (!check_HV_TOGGLE() || *preChargeData.angle_Y > 45 || *preChargeData.angle_Y < -45 || *preChargeData.angle_X > 45 || *preChargeData.angle_X < -45) {
        // kill-switch activated or HV switch turned off
        hv_state = HV_OFF;
      }
      else if (!isHVSafe(preChargeData)) {
        // HV error detected
        hv_state = HV_ERROR;
      }
      else {
        // no updates, keep HV on
        hv_state = HV_ON;
      }
      break;
    case HV_ERROR:
      if (isHVSafe(preChargeData)) {
        // if the error has been cleared
        hv_state = HV_OFF;
      } else {
        // otherwise stay here
        hv_state = HV_ERROR;
      }
      break;
    default:
      hv_state = HV_OFF;
      break;
  } // transitions

  if (hv_state != old_state) {
    Serial.printf("HV transitioned from %s to %s state\n", state_name(old_state), state_name(hv_state));
  }
}

void preChargeCircuitFSMStateActions (PreChargeTaskData preChargeData) {
  switch (hv_state) { // state actions
    case HV_OFF:
      digitalWrite(CONTACTOR, LOW);
      digitalWrite(PRECHARGE, LOW);
      break;
    case HV_PRECHARGING:
      digitalWrite(CONTACTOR, LOW);
      digitalWrite(PRECHARGE, HIGH);
      break;
    case HV_ON:
      digitalWrite(CONTACTOR, HIGH);
      digitalWrite(PRECHARGE, LOW);
      break;
    case HV_ERROR:
      digitalWrite(CONTACTOR, LOW);
      digitalWrite(PRECHARGE, LOW);
    default:
      break;
  } // state actions
}

// Returns true if the motor controller is done precharging.
// Returns false otherwise.
bool isPrecharged(PreChargeTaskData preChargeData) {

  // Ret false if we haven't received all BMS cell voltages yet
  if (*preChargeData.cellVoltages.ready) {
    return false;
  }

  // Ret true if the difference between the main-accumulator-series-voltage and the
  // motorcontroller-voltage is less than 10% of the main-accumulator-series-voltage
  return ((*preChargeData.cellVoltages.seriesVoltage - *preChargeData.motorControllerBatteryVoltage) <= (*preChargeData.cellVoltages.seriesVoltage * 0.1) &&
          //and main-accumulator-voltage is greater than 80V (but this should be changed later as the bike voltage may be as low as ~60V).
          *preChargeData.cellVoltages.seriesVoltage > 80);
}

// this function returns true if there are no HV errors detected on the bike
bool isHVSafe(PreChargeTaskData preChargeData) {
  //BMSStatus bmsStatus = preChargeData.bmsStatus;
  MotorTemps motorTemps = preChargeData.motorTemps;

  /* !!!! these are commented out now but all of this should be checked when using the real bike !!!!
    though you will have to determine which of these are emergency HV states. i.e. Which ones should turn off the contactor instantly
    and which ones should you simply alert the rider?
    As of now, they all immediately turn off the contactor which may be dangerous for the rider
  */

  //if (*bmsStatus.ltc_fault == 1) return 0;
  //if (*bmsStatus.ltc_count != NUMBER_OF_LTCS) return 0;
  //    the below if can be reduced to if (*bmsStatus.bms_c_fault) which returns true for any non-zero bms_c_fault value
  //if (*bmsStatus.bms_c_fault == 1 || *bmsStatus.bms_c_fault == 2 || *bmsStatus.bms_c_fault == 4 ||    //checks BMS fault error codes
  //    *bmsStatus.bms_c_fault == 8) return 0;
  //if (*bmsStatus.bms_status_flag == 1 || *bmsStatus.bms_status_flag == 2) return 0;  //check if cells are above or below the voltage cutoffs
  if (*motorTemps.motorControllerTemperature >= MOTORCONTROLLER_TEMP_MAX
      || *motorTemps.motorTemperature >= MOTOR_TEMP_MAX)       return 0;
  return 1;
}

bool check_HV_TOGGLE() {
  return !digitalRead(HIGH_VOLTAGE_TOGGLE);
}

char* state_name(HV_STATE state) {
  switch (state) {
    case HV_OFF: return "HV_OFF";
    case HV_PRECHARGING: return "HV_PRECHARGING";
    case HV_ON: return "HV_ON";
    case HV_ERROR: return "HV_ERROR";
  }
}

void setupI2C(PreChargeTaskData preChargeData) {
  Wire.setClock(400000); // MPU6050 supports up to 400k Hz in specifications
  Wire.begin();
  delay(50); // give delay for device to start

  // Start gyro in power mode
  Wire.beginTransmission(0x68);
  Wire.write(0x6B); // 6B is relevant register
  Wire.write(0x00); // all bits must be 0 to start and continue device
  Wire.endTransmission();

  // Perform gyroscope calibration measurements
  // 2000 milliseconds = 2 seconds to add all measured variables to calibration variables
  // This is important because this solves the issue of a non-zero rotation rate when stationary
  for (*preChargeData.RateCalibrationNumber = 0; *preChargeData.RateCalibrationNumber < 2000; *preChargeData.RateCalibrationNumber++) {
    gyro_signals(preChargeData);
    *preChargeData.RateCalibrationRoll += *preChargeData.RateRoll;
    *preChargeData.RateCalibrationPitch += *preChargeData.RatePitch;
    *preChargeData.RateCalibrationYaw += *preChargeData.RateYaw;
    delay(1);
  }

  // Take average of calibrated rotation rate values from each direction
  *preChargeData.RateCalibrationRoll /= 2000;
  *preChargeData.RateCalibrationPitch /= 2000;
  *preChargeData.RateCalibrationYaw /= 2000;
  //  *preChargeData.LoopTimer = micros();
}

void gyro_signals(PreChargeTaskData preChargeData) {
  // Start I2C communication with MPU6050
  Wire.beginTransmission(0x68); // 0x68 is default register value for MPU6050

  // Switch on low pass filter
  Wire.write(0x1A); // activate low pass filter
  Wire.write(0x05); // cut off frequency of 10 Hz
  Wire.endTransmission();

  // Configure accelerometer output
  Wire.beginTransmission(0x68);
  Wire.write(0x1C); // 1C is relevant register
  Wire.write(0x10); // full scale range of +/-8g
  Wire.endTransmission();

  // Access registers storing accelerometer measurements
  Wire.beginTransmission(0x68);
  Wire.write(0x3B);
  Wire.endTransmission();
  Wire.requestFrom(0x68, 6);

  // Read accelerometer measurements
  int16_t AccXLSB = Wire.read() << 8 | Wire.read(); // x-direction
  int16_t AccYLSB = Wire.read() << 8 | Wire.read(); // y-direction
  int16_t AccZLSB = Wire.read() << 8 | Wire.read(); // z-direction

  // Configure gyroscope output and pull rotation rate measurements from sensor
  // Set sensitivity scale factor
  Wire.beginTransmission(0x68);
  Wire.write(0x1B); // 1B is hexadecimal associated with gyroscope configuration
  Wire.write(0x8); // 8 is hexadecimal for LSB sensitivity of 65.6 LSB/degree/second
  Wire.endTransmission();

  // Access registers storing gyro measurements
  Wire.beginTransmission(0x68);
  Wire.write(0x43);
  Wire.endTransmission();
  Wire.requestFrom(0x68, 6);

  // Read gyro measurements
  int16_t GyroX = Wire.read() << 8 | Wire.read(); // around x-axis
  int16_t GyroY = Wire.read() << 8 | Wire.read(); // around y-axis
  int16_t GyroZ = Wire.read() << 8 | Wire.read(); // around z-axis

  // Convert measurement units to degree/second
  *preChargeData.RateRoll = (float)GyroX / 65.5;
  *preChargeData.RatePitch = (float)GyroY / 65.5;
  *preChargeData.RateYaw = (float)GyroZ / 65.5;

  // Convert measurements to from LSB to g
  // Divide 4096 because full range +/-8g is associated with 4096 LSB/g
  *preChargeData.AccX = (float)AccXLSB / 4096;
  *preChargeData.AccY = (float)AccYLSB / 4096;
  *preChargeData.AccZ = (float)AccZLSB / 4096;

  float AccX = *preChargeData.AccX;
  float AccY = *preChargeData.AccY;
  float AccZ = *preChargeData.AccZ;

  // Calculate absolute angles
  // IMPORTANT LINE OF CODE:
  //    - WE CAN ADD OR SUBTRACT FROM THE ANGLE DEPENDING ON THE PLACEMENT OF GYROSCOPE
  //    - EX: If MPU6050 is laid on port side, add 90 to AnglePitch
  *preChargeData.AngleRoll = (atan(AccY / sqrt(AccX * AccX + AccZ * AccZ)) * 1 / (3.142 / 180)) + 5;
  *preChargeData.AnglePitch = (-atan(AccX / sqrt(AccY * AccY + AccZ * AccZ)) * 1 / (3.142 / 180));
}

void updateGyroData(PreChargeTaskData preChargeData) {
  gyro_signals(preChargeData);

  *preChargeData.RateRoll -= *preChargeData.RateCalibrationRoll;
  *preChargeData.RatePitch -= *preChargeData.RateCalibrationPitch;
  *preChargeData.RateYaw -= *preChargeData.RateCalibrationYaw;

  // Calculate Roll angle (around x axis)
  kalman_1d(*preChargeData.angle_X, *preChargeData.KalmanUncertaintyAngleRoll, *preChargeData.RateRoll, *preChargeData.AngleRoll, preChargeData);

  // Update Kalman output to angle roll and uncertaintity
  *preChargeData.angle_X = preChargeData.Kalman1DOutput[0];
  *preChargeData.KalmanUncertaintyAngleRoll = preChargeData.Kalman1DOutput[1];

  // Calculate Pitch angle (around y-axis)
  kalman_1d(*preChargeData.angle_Y, *preChargeData.KalmanUncertaintyAnglePitch, *preChargeData.RatePitch, *preChargeData.AnglePitch, preChargeData);

  // Update Kalman output to angle pitch and uncertaintity
  *preChargeData.angle_Y = preChargeData.Kalman1DOutput[0];
  *preChargeData.KalmanUncertaintyAnglePitch = preChargeData.Kalman1DOutput[1];
}

void kalman_1d(float KalmanState, float KalmanUncertainty, float KalmanInput, float KalmanMeasurement, PreChargeTaskData preChargeData) {
  KalmanState = KalmanState + 0.004 * KalmanInput;
  KalmanUncertainty = KalmanUncertainty + 0.004 * 0.004 * 4 * 4;
  float KalmanGain = KalmanUncertainty * 1 / (1 * KalmanUncertainty + 3 * 3);
  KalmanState = KalmanState + KalmanGain * (KalmanMeasurement - KalmanState);
  KalmanUncertainty = (1 - KalmanGain) * KalmanUncertainty;
  // Output of filter
  preChargeData.Kalman1DOutput[0] = KalmanState;
  preChargeData.Kalman1DOutput[1] = KalmanUncertainty;
}
