//IT4735_Thay_Le_Ba_Vui_Nhom25
#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include "driver/rtc_io.h"
#include "time.h"
#include <Preferences.h>

// ================= CẤU HÌNH THIẾT BỊ =================
#define DEVICE_ID    "ESP32_CAM"
#define DEVICE_NAME  "Camera O Dau Do"

// ================= CẤU HÌNH WIFI & FIREBASE =================
#define WIFI_SSID "P may die"
#define WIFI_PASSWORD "19191919"
#define API_KEY "AIzaSyB3rRXDoiSRQITjVAre7nrGKnqntQOR5DA"
#define DATABASE_URL "https://iot-theft-detection-default-rtdb.firebaseio.com"
#define STORAGE_BUCKET "iot-theft-detection.firebasestorage.app"

// ================= CẤU HÌNH PHẦN CỨNG =================
#define PIR_PIN    13
#define RELAY_PIN  12
#define FLASH_PIN  4
#define LED_PIN    33
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// Camera Pinout
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

FirebaseData fbDO;
FirebaseAuth auth;
FirebaseConfig config;
Preferences preferences;

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;
const int   daylightOffset_sec = 0;

// Biến toàn cục để lưu cấu hình (chạy RAM cho nhanh)
int startH = 22; 
int endH = 6;
unsigned long lastConfigUpdate = 0;
unsigned long lastWifiCheck = 0;

// ================= CÁC HÀM KHỞI TẠO (GIỮ NGUYÊN) =================
void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  if(psramFound()){
    config.frame_size = FRAMESIZE_UXGA; config.jpeg_quality = 10; config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA; config.jpeg_quality = 12; config.fb_count = 1;
  }
  esp_camera_init(&config);
}

void initSDCard() {
  if(!SD_MMC.begin("/sdcard", true)){ Serial.println("SD Card Mount Failed"); return; }
}

String getNextOfflineFileName() {
    preferences.begin("counter", false); 
    int count = preferences.getInt("num", 1); 
    String filename = "/offline_" + String(count) + ".jpg";
    count++;
    preferences.putInt("num", count);
    preferences.end();
    return filename;
}

String getDateTimeString() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return String(millis());
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d_%H-%M-%S", &timeinfo);
  return String(timeStringBuff);
}

int getCurrentHour() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return -1; 
  return timeinfo.tm_hour;
}

// ================= HÀM XỬ LÝ CHÍNH (TÁCH RA TỪ SETUP CŨ) =================

// 1. Xử lý khi CÓ MẠNG
void handleOnlineAlert() {
    Serial.println(">>> ONLINE ALARM ACTIVATED");
    digitalWrite(RELAY_PIN, RELAY_ON); 
    digitalWrite(FLASH_PIN, HIGH);

    camera_fb_t * fb = esp_camera_fb_get();
    if(!fb) { Serial.println("Cam Fail"); return; }
    
    String timeStr = getDateTimeString();
    String filename = "/capture_" + timeStr + ".jpg";
    String storagePath = "camera_captures" + filename;
    
    // Upload & Log
    if (Firebase.Storage.upload(&fbDO, STORAGE_BUCKET, fb->buf, fb->len, storagePath, "image/jpeg")) {
         FirebaseJson json;
         json.set("device_id", DEVICE_ID);     
         json.set("device_name", DEVICE_NAME); 
         json.set("timestamp", timeStr);       
         json.set("image_name", filename);     
         json.set("message", "CẢNH BÁO: Đột nhập lúc " + timeStr);
         
         Firebase.RTDB.pushJSON(&fbDO, "/logs", &json);
         Firebase.RTDB.setBool(&fbDO, "/system_status/alarm_active", true); // Bật cờ báo động

         // Vòng lặp hú còi (Chờ lệnh tắt)
         bool alarmState = true;
         unsigned long startAlarm = millis();
         unsigned long lastFirebaseCheck = 0;
         
         while(millis() - startAlarm < 60000) { // Max 1 phút
             // Nháy đèn
             if ((millis() % 500) < 250) {
                 if(!alarmState) { digitalWrite(RELAY_PIN, RELAY_ON); digitalWrite(FLASH_PIN, HIGH); alarmState = true; }
             } else {
                 if(alarmState) { digitalWrite(RELAY_PIN, RELAY_OFF); digitalWrite(FLASH_PIN, LOW); alarmState = false; }
             }
             
             // Check lệnh tắt mỗi 1s
             if (millis() - lastFirebaseCheck > 1000) {
                 lastFirebaseCheck = millis();
                 if (Firebase.RTDB.getBool(&fbDO, "/system_status/alarm_active")) {
                     if (fbDO.boolData() == false) break; // App đã bấm tắt
                 }
             }
             delay(1); 
         }
    }
    esp_camera_fb_return(fb);
    
    // Tắt hết
    digitalWrite(RELAY_PIN, RELAY_OFF);
    digitalWrite(FLASH_PIN, LOW);
}

// 2. Xử lý khi MẤT MẠNG
void handleOfflineAlert() {
    Serial.println(">>> OFFLINE ALARM ACTIVATED");
    
    camera_fb_t * fb = esp_camera_fb_get();
    
    // Lưu thẻ nhớ
    String filename = getNextOfflineFileName();
    fs::FS &fs = SD_MMC; 
    File file = fs.open(filename, FILE_WRITE);
    if(file && fb) { file.write(fb->buf, fb->len); file.close(); }
    if(fb) esp_camera_fb_return(fb);

    // Hú bám đuổi (giống code cũ)
    unsigned long startOffline = millis();
    bool alarmState = false;
    while (digitalRead(PIR_PIN) == HIGH || millis() - startOffline < 5000) {
         if (millis() - startOffline > 60000) break;
         if ((millis() / 250) % 2 == 0) { 
              if(!alarmState) { digitalWrite(RELAY_PIN, RELAY_ON); digitalWrite(FLASH_PIN, HIGH); alarmState = true; }
          } else {
              if(alarmState) { digitalWrite(RELAY_PIN, RELAY_OFF); digitalWrite(FLASH_PIN, LOW); alarmState = false; }
          }
          delay(10);
    }
    
    digitalWrite(RELAY_PIN, RELAY_OFF);
    digitalWrite(FLASH_PIN, LOW);
}

// Hàm cập nhật cấu hình định kỳ
void updateConfig() {
    if (Firebase.RTDB.getInt(&fbDO, "/devices/ESP32_CAM/config/start_hour")) {
        startH = fbDO.intData();
    }
    if (Firebase.RTDB.getInt(&fbDO, "/devices/ESP32_CAM/config/end_hour")) {
        endH = fbDO.intData();
    }
    // Serial.printf("Config Updated: %d - %d\n", startH, endH);
}

// ================= SETUP (CHẠY 1 LẦN) =================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 
  Serial.begin(115200);

  pinMode(PIR_PIN, INPUT); 
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(FLASH_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  
  digitalWrite(RELAY_PIN, RELAY_OFF);
  digitalWrite(FLASH_PIN, LOW);
  digitalWrite(LED_PIN, HIGH); 

  // WARM-UP (Chỉ chạy 1 lần khi cắm điện)
  Serial.println(">>> POWER ON. WARMING UP...");
  while (digitalRead(PIR_PIN) == HIGH) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(200);
  }
  Serial.println("System Ready.");
  digitalWrite(LED_PIN, HIGH);

  initCamera();
  initSDCard();

  // Kết nối WiFi (Chờ đến khi được thì thôi)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN)); 
    delay(500); 
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  digitalWrite(LED_PIN, LOW); // Đèn sáng báo có mạng

  // Init Thời gian & Firebase
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.timeout.serverResponse = 1000;
  Firebase.signUp(&config, &auth, "", "");
  Firebase.begin(&config, &auth);
  
  // Lấy cấu hình lần đầu
  updateConfig();
}

// ================= LOOP (CHẠY LIÊN TỤC) =================
void loop() {
    
    // 1. Tự động kiểm tra và nối lại WiFi
    if (WiFi.status() != WL_CONNECTED) {
        if(millis() - lastWifiCheck > 5000) { // Thử nối lại mỗi 5s
            lastWifiCheck = millis();
            Serial.println("WiFi Lost! Reconnecting...");
            WiFi.disconnect();
            WiFi.reconnect();
            digitalWrite(LED_PIN, HIGH); // Tắt đèn báo mất mạng
        }
    } else {
        digitalWrite(LED_PIN, LOW); // Đèn sáng báo có mạng
    }

    // 2. Cập nhật cấu hình định kỳ (Mỗi 10 giây)
    if (WiFi.status() == WL_CONNECTED && millis() - lastConfigUpdate > 10000) {
        lastConfigUpdate = millis();
        updateConfig();
    }

    // 3. QUAN TRỌNG: QUÉT CẢM BIẾN PIR
    if (digitalRead(PIR_PIN) == HIGH) {
        Serial.println("!!! MOTION DETECTED !!!");
        
        // Phân nhánh Online / Offline
        if (WiFi.status() == WL_CONNECTED) {
            // Có mạng -> Check giờ
            int currentH = getCurrentHour();
            bool isDangerTime = false;
            
            if (currentH != -1) {
                if (startH > endH) { // VD: 22h -> 6h
                    if (currentH >= startH || currentH < endH) isDangerTime = true;
                } else { // VD: 8h -> 17h
                    if (currentH >= startH && currentH < endH) isDangerTime = true;
                }
            } else {
                isDangerTime = true; // Lỗi giờ thì cứ báo động cho an toàn
            }

            if (isDangerTime) {
                handleOnlineAlert();
            } else {
                Serial.println("Safe Time. Ignored.");
            }
        } 
        else {
            // Mất mạng -> Báo động Offline luôn
            handleOfflineAlert();
        }

        // Chờ PIR về LOW để tránh kích hoạt liên tục (Debounce)
        Serial.println("Waiting for PIR clear...");
        while(digitalRead(PIR_PIN) == HIGH) delay(100);
        Serial.println("Clear. Monitoring...");
    }

    delay(100); // Giảm tải CPU
}