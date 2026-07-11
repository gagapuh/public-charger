#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Servo.h>
#include <sys/time.h>
#include <time.h>

// --- BLE SETTINGS ---
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// --- PIN MAPPING ---
#define SERVO1_PIN    0
#define MOSFET_5V     4  // controls "Signboard" (Вывеска)
#define MOSFET_12V    5  // controls "LED Strip" (Светолента)
#define LED1_PIN      9  // Addressable strip data pin

// --- LED CONFIGURATION ---
#define LED1_COUNT    40

Servo servo1;
Adafruit_NeoPixel strip1(LED1_COUNT, LED1_PIN, NEO_GRB + NEO_KHZ800);

// --- SMOOTH SERVO VARIABLES ---
int s1_target_us = 1500;  
int s1_current_us = 1500; 
bool s1_is_moving = false; 

const int SERVO_STEP_SIZE = 5;     
const int SERVO_UPDATE_INTERVAL = 15; 
unsigned long lastServoTime = 0;

// --- TELEMETRY TIMER ---
unsigned long lastTxTime = 0;
const unsigned long TX_INTERVAL = 2000; 

// --- CONTROLS ---
// 0 - OFF, 1 - Idle (Cyan), 2 - Charging Flow Animation
int mode_led1 = 2; 
bool is_led1_active = false;
bool prev_led1_active = false;

unsigned long lastLedAnimTime = 0;
float phase = 0.0;

bool m5_state = false;  // Signboard status
bool m12_state = false; // LED Strip status

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("[BLE] Client connected!");
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("[BLE] Client disconnected!");
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String cmd = pCharacteristic->getValue().c_str();
      cmd.trim();
      
      if (cmd.length() > 0) {
        Serial.print("[BLE RX] Command: ");
        Serial.println(cmd);
        
        int colonIdx = cmd.indexOf(':');
        if (colonIdx > 0) {
          String key = cmd.substring(0, colonIdx);
          String val = cmd.substring(colonIdx + 1);
          
          if (key == "s1") {
            s1_target_us = val.toInt();
            s1_target_us = constrain(s1_target_us, 500, 2500);
            Serial.printf("  - Servo 1 target: %d us\n", s1_target_us);
          }
          else if (key == "m5") {
            m5_state = (val == "1");
            digitalWrite(MOSFET_5V, m5_state ? HIGH : LOW);
            Serial.printf("  - Signboard (MOSFET 5V): %s\n", m5_state ? "ON" : "OFF");
          }
          else if (key == "m12") {
            m12_state = (val == "1");
            digitalWrite(MOSFET_12V, m12_state ? HIGH : LOW);
            Serial.printf("  - LED Strip (MOSFET 12V): %s\n", m12_state ? "ON" : "OFF");
          }
          else if (key == "m1") {
            mode_led1 = val.toInt();
            Serial.printf("  - LED1 Mode: %s\n", mode_led1 == 2 ? "CHARGING FLOW" : (mode_led1 == 1 ? "ON" : "OFF"));
          }
          else if (key == "time") {
            uint32_t epoch = val.toInt();
            struct timeval tv;
            tv.tv_sec = epoch;
            tv.tv_usec = 0;
            settimeofday(&tv, NULL);
            
            time_t now = epoch;
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            char logTime[32];
            strftime(logTime, sizeof(logTime), "%Y-%m-%d %H:%M:%S", &timeinfo);
            Serial.printf("  - Time synchronized: %s\n", logTime);
          }
        }
      }
    }
};

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=============================================");
  Serial.println("    SYSTEM START: PUBLIC CHARGER (BLE)       ");
  Serial.println("=============================================");

  pinMode(MOSFET_5V, OUTPUT);
  pinMode(MOSFET_12V, OUTPUT);
  digitalWrite(MOSFET_5V, LOW);
  digitalWrite(MOSFET_12V, LOW);
  Serial.println("[System] MOSFET pins configured (5V & 12V -> OFF)");

  strip1.begin(); strip1.show();
  Serial.println("[System] Addressable LED strip 1 (40 LEDs) initialized");

  servo1.setPeriodHertz(50);
  servo1.attach(SERVO1_PIN, 500, 2500);
  servo1.writeMicroseconds(s1_current_us);
  Serial.printf("[System] Default Servo 1 position: %d us\n", s1_current_us);

  // Initialize BLE Device
  BLEDevice::init("Public Charger");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                                           CHARACTERISTIC_UUID_RX,
                                           BLECharacteristic::PROPERTY_WRITE |
                                           BLECharacteristic::PROPERTY_WRITE_NR
                                         );
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  
  BLEDevice::startAdvertising();
  
  Serial.println("[BLE] Server started. Device name: \"Public Charger\"");
  Serial.println("[BLE] Waiting for clients...");
  Serial.println("=============================================\n");
}

void loop() {
  unsigned long currentMillis = millis();

  // --- SERVO 1 STEPPING BLOCK ---
  if (currentMillis - lastServoTime >= SERVO_UPDATE_INTERVAL) {
    lastServoTime = currentMillis;
    if (s1_current_us != s1_target_us) {
      if (!s1_is_moving) s1_is_moving = true;
      if (s1_target_us > s1_current_us) {
        s1_current_us += SERVO_STEP_SIZE;
        if (s1_current_us > s1_target_us) s1_current_us = s1_target_us;
      } else {
        s1_current_us -= SERVO_STEP_SIZE;
        if (s1_current_us < s1_target_us) s1_current_us = s1_target_us;
      }
      servo1.writeMicroseconds(s1_current_us);
      if (s1_current_us == s1_target_us) s1_is_moving = false;
    }
  }

  // --- LED1 CONTROL LOGIC ---
  is_led1_active = (mode_led1 != 0);

  if (is_led1_active) {
    if (mode_led1 == 2) {
      // Charging Wave Animation (Green/Blue pulses running along 40 LEDs)
      if (currentMillis - lastLedAnimTime >= 30) {
        lastLedAnimTime = currentMillis;
        phase += 0.08;
        if (phase > 2 * PI) phase -= 2 * PI;
        for (int i = 0; i < LED1_COUNT; i++) {
          float sinVal = sin(i * 0.3 - phase);
          int g = sinVal > 0 ? (int)(50 + 205 * sinVal) : 20;
          int b = sinVal > 0 ? (int)(20 + 80 * sinVal) : 10;
          strip1.setPixelColor(i, strip1.Color(0, g, b));
        }
        strip1.show();
      }
    } else {
      // Solid Cyan/White
      if (!prev_led1_active || (mode_led1 == 1 && prev_led1_active && strip1.getPixelColor(0) == 0)) {
        for (int i = 0; i < LED1_COUNT; i++) strip1.setPixelColor(i, strip1.Color(0, 180, 200)); // Cyan
        strip1.show();
      }
    }
  } else {
    if (prev_led1_active) {
      for (int i = 0; i < LED1_COUNT; i++) strip1.setPixelColor(i, 0);
      strip1.show();
    }
  }
  prev_led1_active = is_led1_active;

  // --- TELEMETRY AND CLOCK BROADCAST OVER BLE (Every 2 seconds) ---
  if (deviceConnected) {
    if (currentMillis - lastTxTime >= TX_INTERVAL) {
      lastTxTime = currentMillis;
      
      time_t now;
      struct tm timeinfo;
      time(&now);
      localtime_r(&now, &timeinfo);
      
      char timeStr[16] = "--:--:--";
      if (now > 1000000) {
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
      }
      
      // Payload format:
      // c:Clock,m5:Signboard(0/1),m12:LEDStrip(0/1),l1:LED1Active(0/1),r1:LED1Mode(0/1/2)
      char txBuffer[64];
      snprintf(txBuffer, sizeof(txBuffer), 
               "c:%s,m5:%d,m12:%d,l1:%d,r1:%d", 
               timeStr, 
               m5_state ? 1 : 0, 
               m12_state ? 1 : 0, 
               is_led1_active ? 1 : 0, 
               mode_led1);
      
      pTxCharacteristic->setValue((uint8_t*)txBuffer, strlen(txBuffer));
      pTxCharacteristic->notify();
      
      Serial.printf("[BLE TX] Sent: %s\n", txBuffer);
    }
  }

  // --- BLE CONNECTION MANAGER ---
  if (!deviceConnected && oldDeviceConnected) {
    delay(500); 
    pServer->startAdvertising(); 
    Serial.println("[BLE] Client disconnected. Restarting advertising.");
    oldDeviceConnected = deviceConnected;
  }
  
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
  
  delay(1);
}
