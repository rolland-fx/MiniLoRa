#include <Wire.h>
#include <Adafruit_LIS3DH.h>
#include <Adafruit_Sensor.h>
#include <Arduino.h>
#include <SensirionI2cSht4x.h>
#include "SparkFun_I2C_GPS_Arduino_Library.h"
//#include <BLEDevice.h>
//#include <BLEUtils.h>
//#include <BLEServer.h>
#include <TinyGPSPlus.h>

#include "soc/soc_caps.h"
#include "esp_sleep.h"
#include "Arduino.h"
#include "esp_timer.h"
#include "driver/rtc_io.h"
#include "soc/rtc.h"
#include "driver/gpio.h"
#include <ESP32Time.h>
#include <ByteConvert.hpp>

#include "config.h"

#define DEBUG false
#define uS_TO_mS_FACTOR 1000ULL


#define RANGE 3
int const SLEEP_T[6] = { 60000, 300000, 600000, 900000, 1200000, 1800000};

#define WIFI false
#define BLE false
#define LORAWAN true
#define LOCAL_STORE false

#define GNSS true
#define SHT40 true
#define OPT30 true
#define LIS3DH false // for now, cause issue with the SPI for LoRa. Will be fixed later

const gpio_num_t LORA_RST = GPIO_NUM_2;
const gpio_num_t GNSS_RST = GPIO_NUM_0;
const gpio_num_t ORANGE = GPIO_NUM_15;
const gpio_num_t BLUE = GPIO_NUM_4;
const gpio_num_t BUTTON = GPIO_NUM_7;
const gpio_num_t BAT_ON = GPIO_NUM_6;
const gpio_num_t BAT_LV = GPIO_NUM_5;

struct Data { //Total size : 55 bytes. To Store data with all the precisions
  int8_t Temp;
  uint8_t Hum;
  uint16_t Bat_vol;
  uint16_t Lum;
  uint16_t Acc_x;
  uint16_t Acc_y;
  uint16_t Acc_z;
  double Lat;
  double Lon;
  int16_t Alt;
  uint8_t Sat;
  uint32_t Time;
  char UTC[20];
};

struct LoRaWanData { //Total size : 24 bytes. To send via LoRaWan
  int8_t Temp;
  uint8_t Hum;
  uint8_t Bat_vol;
  uint16_t Lum;
  uint8_t Acc_x;
  uint8_t Acc_y;
  uint8_t Acc_z;
  int32_t Lat;
  int32_t Lon;
  int16_t Alt;
  uint8_t Sat;
  uint32_t Time;
};


char* GNSS_DEEP_SLEEP = "$PMTK225,4*2F\r\n";
char* GNSS_MID_POWER = "$PMTK220,200*2C\r\n";

SensirionI2cSht4x sensor;
Adafruit_LIS3DH lis = Adafruit_LIS3DH();
Data LiveData;
LoRaWanData LoRaData;
float fTemp;
float fHum;

ESP32Time rtc;

uint8_t buf[4];
uint8_t uplinkPayload[23];

double Lat_buffer;
double Lon_buffer;


uint16_t Lum_man;
uint16_t Lum_exp;

I2CGPS myI2CGPS;
TinyGPSPlus gps;
esp_sleep_wakeup_cause_t wakeup_reason;


#define BUTTON_PIN_BITMASK(GPIO) (1ULL << GPIO)  // 2 ^ GPIO_NUMBER in hex
#define WAKEUP_GPIO              GPIO_NUM_7     // Only RTC IO are allowed - ESP32 Pin example

void setup() {
  // Wake-up procedure
  pinMode(LORA_RST, OUTPUT);
  pinMode(GNSS_RST, OUTPUT);
  pinMode(BAT_ON, OUTPUT);
  pinMode(BLUE, OUTPUT);
  pinMode(BAT_LV, INPUT);
  digitalWrite(LORA_RST, LOW);
  gpio_hold_dis(GNSS_RST);
  gpio_hold_dis(BLUE);
  gpio_hold_dis(LORA_RST);
  gpio_hold_dis(BAT_ON);
  wakeup_reason = esp_sleep_get_wakeup_cause();
  if (DEBUG) {
    Serial.begin(115200);
    delay(100);
    Serial.println(wakeup_reason);
  }
  if(wakeup_reason == ESP_SLEEP_WAKEUP_EXT1)
  {
    delay(20000);
  }
  Wire.begin();
  digitalWrite(BLUE, LOW);
  if (GNSS) {
    digitalWrite(GNSS_RST, HIGH);
    delay(100);
    Wire.beginTransmission(0x10);
    Wire.write((uint8_t*)GNSS_MID_POWER, 20);
    Wire.endTransmission();
  }
  if (SHT40) {
    sensor.begin(Wire, 0x44);
    sensor.softReset();
  }
  if (OPT30) {
    Wire.beginTransmission(0x47);  // ADDR pulled to GND
    Wire.write(0x01);              // config register
    Wire.write(0xCE);              // range, etc
    Wire.write(0x10);
    Wire.endTransmission();
    Wire.beginTransmission(0x47);
    Wire.write(0);           // result register
    Wire.endTransmission();  // sends a STOP
  }
  if (LIS3DH) {
    lis.begin(0x19);
    lis.setRange(LIS3DH_RANGE_4_G);
    lis.setPerformanceMode(LIS3DH_MODE_LOW_POWER);
    lis.setDataRate(LIS3DH_DATARATE_10_HZ);
    Wire.endTransmission();
  }
  if (LORAWAN) {
    digitalWrite(LORA_RST, HIGH);
  }
  if (LOCAL_STORE) {
  }
  delay(250);
  digitalWrite(BLUE, HIGH);
  delay(2250);
  // Data acquisition
  if (GNSS) {
    uint8_t turn = 0;
    myI2CGPS.begin();
    while (!gps.location.isValid() || turn < 5)
    {
       while (myI2CGPS.available() )  //available() returns the number of new bytes available from the GPS module
      {
        gps.encode(myI2CGPS.read());  //Feed the GPS parser
      }
      if (gps.time.isUpdated())  //Check to see if new GPS info is available
     {
        if (gps.time.isValid() && gps.location.isValid()) {
          LiveData.Lon = gps.location.lng();
          LiveData.Lat = gps.location.lat();
          if (gps.altitude.meters() > 32767) LiveData.Alt = 0;
          else LiveData.Alt = (int16_t)gps.altitude.meters();
          LiveData.Sat = gps.satellites.value();
          rtc.setTime(gps.time.second(), gps.time.minute(), gps.time.hour(), gps.date.day(), gps.date.month(), gps.date.year());
          sprintf(LiveData.UTC,"%4d-%02d-%02dT%02d:%02d:%02dZ",gps.date.year(),gps.date.month(),gps.date.day(),gps.time.hour(),gps.time.minute(),gps.time.second());
          LiveData.Time = rtc.getEpoch();
        } 
     }
     delay(2500);
     turn++;
    }
  }
  if (SHT40) {
    sensor.measureMediumPrecision(fTemp, fHum);
    Wire.endTransmission();
    LiveData.Temp = (int)fTemp;
    LiveData.Hum = (int)fHum;
  }
  if (OPT30) {
    Wire.requestFrom(0x47, 2);  // read 2 bytes from last address
    Wire.readBytes(buf, 2);
    Lum_exp = buf[0] >> 4;
    Lum_man = buf[1] | ((buf[0] & 0x0F) << 8);
    LiveData.Lum = (int)Lum_man * (0.01 * pow(2, Lum_exp));
    Wire.endTransmission();
  }
  if (LIS3DH) {
    lis.read();
    LiveData.Acc_x = (int)lis.x;
    LiveData.Acc_y = (int)lis.y;
    LiveData.Acc_z = (int)lis.z;
    Wire.endTransmission();
  }
  digitalWrite(BAT_ON, HIGH);
  delay(50);
  LiveData.Bat_vol = 2 * analogReadMilliVolts(BAT_LV);
  delay(50);
  digitalWrite(BAT_ON, LOW);
  // Data storage // sending
  if (LORAWAN) {
    LoRaData.Temp = LiveData.Temp;
    LoRaData.Hum = LiveData.Hum;
    LoRaData.Bat_vol = (uint8_t) map(LiveData.Bat_vol, 2500, 4300, 0, 255);
    LoRaData.Lum = LiveData.Lum;
    LoRaData.Acc_x = (uint8_t) map(LiveData.Acc_x, 0, 10000, 0, 255);
    LoRaData.Acc_y = (uint8_t) map(LiveData.Acc_y, 0, 10000, 0, 255);
    LoRaData.Acc_z = (uint8_t) map(LiveData.Acc_z, 0, 10000, 0, 255);
    Lat_buffer = LiveData.Lat* 1000000.0;
    Lon_buffer =  LiveData.Lon* 1000000.0;
    LoRaData.Lat = (int32_t)Lat_buffer;
    LoRaData.Lon = (int32_t)Lon_buffer; 
    LoRaData.Alt = LiveData.Alt;
    LoRaData.Sat = LiveData.Sat;
    LoRaData.Time = LiveData.Time;
    
    SPI.begin(SX1262_SCK, SX1262_MISO, SX1262_MOSI, SX1262_CS);
    delay(50);
    size_t s_lat = 0;
    size_t s_lon = 0;
    size_t s_time = 0;    
    int16_t state = radio.begin();
    Serial.print("Begin : ");
    Serial.println(stateDecode(state));
    state = node.beginOTAA(joinEUI, devEUI, nwkKey, appKey);
    Serial.print("OTAA Start : ");
    Serial.println(stateDecode(state));
    state = node.activateOTAA();
    Serial.print("OTAA Next : ");
    Serial.println(stateDecode(state));
    uplinkPayload[0] = LoRaData.Temp;
    uplinkPayload[1] = LoRaData.Hum;
    uplinkPayload[2] = LoRaData.Bat_vol;
    uplinkPayload[3] = lowByte(LoRaData.Lum);
    uplinkPayload[4] = highByte(LoRaData.Lum);  
    uplinkPayload[5] = LoRaData.Acc_x;
    uplinkPayload[6] = LoRaData.Acc_y;
    uplinkPayload[7] = LoRaData.Acc_z;
    uint8_t *a_lat = ByteConvert::varToArray<int>(s_lat,LoRaData.Lat);
    uplinkPayload[8] = a_lat[3];
    uplinkPayload[9] = a_lat[2];
    uplinkPayload[10] = a_lat[1];
    uplinkPayload[11] = a_lat[0];
    uint8_t *a_lon = ByteConvert::varToArray<int>(s_lon,LoRaData.Lon);     
    uplinkPayload[12] = a_lon[3];      
    uplinkPayload[13] = a_lon[2];       
    uplinkPayload[14] = a_lon[1];  
    uplinkPayload[15] = a_lon[0];  
    uplinkPayload[16] = lowByte(LoRaData.Alt);
    uplinkPayload[17] = highByte(LoRaData.Alt);
    uplinkPayload[18] = LoRaData.Sat;
    uint8_t *a_time = ByteConvert::varToArray<int>(s_time,LoRaData.Time);
    uplinkPayload[19] = a_time[3]; 
    uplinkPayload[20] = a_time[2]; 
    uplinkPayload[21] = a_time[1]; 
    uplinkPayload[22] = a_time[0]; 
    state = node.sendReceive(uplinkPayload, sizeof(uplinkPayload));
    Serial.print("Data Sending : ");
    Serial.println(stateDecode(state)); 
  }
  if (LOCAL_STORE) {
  }
  if (DEBUG) {
    Serial.print("<");
    Serial.print(LiveData.UTC);
    Serial.print(",");
    Serial.print(LiveData.Lat, 6);
    Serial.print(",");
    Serial.print(LiveData.Lon, 6);
    Serial.print(",");
    Serial.print(LiveData.Alt,0);
    Serial.print(",");
    Serial.print(LiveData.Sat);   
    Serial.print(",");
    Serial.print(LiveData.Temp);
    Serial.print(",");
    Serial.print(LiveData.Hum);
    Serial.print(",");
    Serial.print(LiveData.Lum);
    Serial.print(",");
    Serial.print(LiveData.Acc_x);
    Serial.print(",");
    Serial.print(LiveData.Acc_y);
    Serial.print(",");
    Serial.print(LiveData.Acc_z);
    Serial.print(",");
    Serial.print(LiveData.Bat_vol);
    Serial.print(">");
    Serial.print("\n");
  }
  // Prepare for sleep
  esp_sleep_enable_timer_wakeup(SLEEP_T[RANGE] * uS_TO_mS_FACTOR);
  if (GNSS) {
    Wire.beginTransmission(0x10);
    Wire.write((uint8_t*)GNSS_DEEP_SLEEP, 17);
    Wire.endTransmission();
    digitalWrite(GNSS_RST, LOW);
  }
  if (SHT40) {
  }
  if (OPT30) {
  }
  if (LIS3DH) {
    lis.setDataRate(LIS3DH_DATARATE_POWERDOWN);
  }
  if (LORAWAN) {
    digitalWrite(LORA_RST, LOW);
  }
  if (LOCAL_STORE) {
  }
  if (DEBUG) {
    Serial.flush();
  }
  esp_sleep_enable_ext1_wakeup_io(BUTTON_PIN_BITMASK(WAKEUP_GPIO), ESP_EXT1_WAKEUP_ANY_LOW);
  rtc_gpio_pulldown_dis(WAKEUP_GPIO);  // GPIO33 is tie to GND in order to wake up in HIGH
  rtc_gpio_pullup_dis(WAKEUP_GPIO);   // Disable PULL_UP in order to allow it to wakeup on HIGH

  digitalWrite(BLUE, HIGH);
  gpio_hold_en(GNSS_RST);
  gpio_hold_en(BLUE);
  gpio_hold_en(LORA_RST);
  gpio_hold_dis(BAT_ON);
  esp_deep_sleep_start();
}

void loop() {
}
