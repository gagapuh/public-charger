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

// --- CONTROLS AND MODES ---
// 0 - Force OFF, 1 - Force ON, 2 - AUTO (Timer schedule 18:00 - 05:00)
int mode_led1 = 2;      // Addressable strip
int mode_mosfet5 = 2;   // 5V MOSFET (Signboard)
int mode_mosfet12 = 2;  // 12V MOSFET (LED Strip)

// Active states (determined by mode and time schedule)
bool is_led1_active = false;
bool is_m5_active = false;
bool is_m12_active = false;

// Transition trackers
bool prev_led1_active = false;

unsigned long lastLedAnimTime = 0;
float phase = 0.0;

// --- FLICKERING LOGIC VARIABLES (Horror movie style for 5V Signboard) ---
unsigned long nextFlickerTime = 0;
unsigned long flickerEndTime = 0;
bool isFlickering = false;
unsigned long lastFlickerToggleTime = 0;
int flickerState = HIGH;

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
            mode_mosfet5 = val.toInt();
            Serial.printf("  - Signboard (5V) Mode: %s\n", mode_mosfet5 == 2 ? "AUTO" : (mode_mosfet5 == 1 ? "ON" : "OFF"));
          }
          else if (key == "m12") {
            mode_mosfet12 = val.toInt();
            Serial.printf("  - LED Strip (12V) Mode: %s\n", mode_mosfet12 == 2 ? "AUTO" : (mode_mosfet12 == 1 ? "ON" : "OFF"));
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

// Evaluate schedule (active between 18:00 and 05:00)
bool isScheduleActive() {
  time_t now;
  struct tm timeinfo;
  time(&now);
  if (now < 1000000) {
    // Default to active if clock not yet synchronized via BLE
    return true; 
  }
  localtime_r(&now, &timeinfo);
  int hour = timeinfo.tm_hour;
  if (hour >= 18 || hour < 5) {
    return true;
  }
  return false;
}

// Film-style flickering effect for 5V Neon Signboard
void updateSignboardFlicker(unsigned long currentMillis, bool active) {
  if (!active) {
    digitalWrite(MOSFET_5V, LOW);
    isFlickering = false;
    return;
  }

  // Normal state is solid ON
  if (!isFlickering) {
    digitalWrite(MOSFET_5V, HIGH);
    
    // Set next flicker burst interval (randomly between 15 to 45 seconds)
    if (nextFlickerTime == 0) {
      nextFlickerTime = currentMillis + random(15000, 45000);
    }
    
    if (currentMillis >= nextFlickerTime) {
      isFlickering = true;
      // Flicker burst duration: 250ms to 800ms
      flickerEndTime = currentMillis + random(250, 800);
      lastFlickerToggleTime = currentMillis;
      flickerState = LOW;
      digitalWrite(MOSFET_5V, flickerState);
    }
  } else {
    // Active flicker burst
    if (currentMillis >= flickerEndTime) {
      isFlickering = false;
      digitalWrite(MOSFET_5V, HIGH); // Restore solid ON
      // Schedule next burst
      nextFlickerTime = currentMillis + random(15000, 45000);
    } else {
      // Rapidly toggle state at irregular intervals (35ms to 110ms)
      if (currentMillis - lastFlickerToggleTime >= random(35, 110)) {
        lastFlickerToggleTime = currentMillis;
        flickerState = !flickerState;
        digitalWrite(MOSFET_5V, flickerState ? HIGH : LOW);
      }
    }
  }
}

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

  // --- EVALUATE SCHEDULE & MODES ---
  bool sched = isScheduleActive();

  is_led1_active = (mode_led1 == 1 || (mode_led1 == 2 && sched));
  is_m5_active  = (mode_mosfet5 == 1 || (mode_mosfet5 == 2 && sched));
  is_m12_active = (mode_mosfet12 == 1 || (mode_mosfet12 == 2 && sched));

  // --- MOSFET 12V CONTROL (LED Strip) ---
  digitalWrite(MOSFET_12V, is_m12_active ? HIGH : LOW);

  // --- MOSFET 5V CONTROL (Signboard with Horror Flicker) ---
  updateSignboardFlicker(currentMillis, is_m5_active);

  // --- ADDRESSABLE LED1 CONTROL (40 LEDs) ---
  if (is_led1_active) {
    if (mode_led1 == 2) {
      // Charging Flow Animation (Green/Blue pulses running along 40 LEDs)
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
      // Solid Cyan/White Status
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
      // c:Clock,m5:Mode5V(0/1/2),m12:Mode12V(0/1/2),l1:LED1Active(0/1),r1:LED1Mode(0/1/2),s5:Phys5V(0/1),s12:Phys12V(0/1)
      char txBuffer[96];
      snprintf(txBuffer, sizeof(txBuffer), 
               "c:%s,m5:%d,m12:%d,l1:%d,r1:%d,s5:%d,s12:%d", 
               timeStr, 
               mode_mosfet5, 
               mode_mosfet12, 
               is_led1_active ? 1 : 0, 
               mode_led1,
               is_m5_active ? 1 : 0,
               is_m12_active ? 1 : 0);
      
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
