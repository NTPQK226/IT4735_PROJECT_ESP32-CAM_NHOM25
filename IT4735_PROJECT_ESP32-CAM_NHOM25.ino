// IT4735_Thay_Le_Ba_Vui_Nhom25
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

// ================= CẤU HÌNH WIFI =================
#define WIFI_SSID "P may die"
#define WIFI_PASSWORD "19191919"

// ================= CẤU HÌNH FIREBASE =================
#define API_KEY "AIzaSyB3rRXDoiSRQITjVAre7nrGKnqntQOR5DA"
#define DATABASE_URL "https://iot-theft-detection-default-rtdb.firebaseio.com"
#define STORAGE_BUCKET "iot-theft-detection.firebasestorage.app"

// ================= CẤU HÌNH PHẦN CỨNG =================
#define PIR_PIN    13
#define RELAY_PIN  12
#define FLASH_PIN  4
#define LED_PIN    33

// Cấu hình mức kích Relay
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ================= CAMERA PINOUT =================
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

// Server lấy giờ
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;
const int   daylightOffset_sec = 0;

// Biến lưu cấu hình giờ
int config_start_hour = 23; // Mặc định 23h
int config_end_hour = 6;    // Mặc định 6h sáng

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
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }
  esp_camera_init(&config);
}

void initSDCard() {
  if(!SD_MMC.begin("/sdcard", true)){
    Serial.println("SD Card Mount Failed");
    return;
  }
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

// === HÀM MỚI: KIỂM TRA GIỜ CÓ ĐƯỢC PHÉP BÁO ĐỘNG KHÔNG ===
bool checkAllowedTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return true; // Không lấy được giờ thì mặc định cho phép (An toàn)
  }
  
  int current_h = timeinfo.tm_hour;
  Serial.printf("Current: %d | Start: %d | End: %d\n", current_h, config_start_hour, config_end_hour);

  // Logic so sánh giờ
  if (config_start_hour < config_end_hour) {
    if (current_h >= config_start_hour && current_h < config_end_hour) return true;
  } else {
    if (current_h >= config_start_hour || current_h < config_end_hour) return true;
  }

  return false;
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(FLASH_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  
  // Đảm bảo tắt hết khi mới khởi động
  digitalWrite(RELAY_PIN, RELAY_OFF);
  digitalWrite(FLASH_PIN, LOW);

  // Khởi tạo Camera & SD
  initCamera();
  initSDCard();

  // Chụp ảnh trước để sẵn sàng buffer
  camera_fb_t * fb = NULL;
  fb = esp_camera_fb_get();
  if(!fb) { Serial.println("Capture failed"); esp_deep_sleep_start(); }

  // Kết nối WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) { // Tăng retry lên chút
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(200); // Nháy nhanh hơn chút
    Serial.print(".");
    retry++;
  }

  // ===========================================
  // TRƯỜNG HỢP 1: CÓ MẠNG (ONLINE)
  // ===========================================
  if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi Connected! Checking Config...");
      digitalWrite(LED_PIN, LOW); 

      // 1. Lấy giờ chuẩn
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      
      // 2. Kết nối Firebase lấy Config
      config.api_key = API_KEY;
      config.database_url = DATABASE_URL;
      Firebase.signUp(&config, &auth, "", "");
      Firebase.begin(&config, &auth);

      // Lấy start_hour
      if (Firebase.RTDB.getInt(&fbDO, "/devices/ESP32_CAM/config/start_hour")) {
         if (fbDO.dataType() == "int") config_start_hour = fbDO.intData();
      }
      // Lấy end_hour
      if (Firebase.RTDB.getInt(&fbDO, "/devices/ESP32_CAM/config/end_hour")) {
         if (fbDO.dataType() == "int") config_end_hour = fbDO.intData();
      }

      // 3. KIỂM TRA XEM CÓ ĐƯỢC PHÉP BÁO ĐỘNG KHÔNG?
      if (checkAllowedTime()) {
           Serial.println(">>> TIME ALLOWED! ALARM ACTIVE <<<");
           
           // A. BẬT BÁO ĐỘNG NGAY
           digitalWrite(RELAY_PIN, RELAY_ON);
           digitalWrite(FLASH_PIN, HIGH);
           
           // B. UPLOAD ẢNH
           String timeStr = getDateTimeString();
           String filename = "/capture_" + timeStr + ".jpg";
           String path = "camera_captures" + filename;

           if (Firebase.Storage.upload(&fbDO, STORAGE_BUCKET, fb->buf, fb->len, path, "image/jpeg")) {
                FirebaseJson json;
                json.set("timestamp", timeStr);
                json.set("device_id", "ESP32_CAM");
                json.set("image_name", filename);
                json.set("message", "Motion Detected!");
                Firebase.RTDB.pushJSON(&fbDO, "/logs", &json);
                
                Firebase.RTDB.setBool(&fbDO, "/system_status/alarm_active", true);

                // C. VÒNG LẶP HÚ CÒI (Cho đến khi hết giờ hoặc App tắt)
                bool alarmState = true; 
                unsigned long startAlarm = millis();
                while(millis() - startAlarm < 60000) { // Max 1 phút
                    // Nháy đèn/Còi
                    if ((millis() / 250) % 2 == 0) {
                        if(!alarmState) { digitalWrite(RELAY_PIN, RELAY_ON); digitalWrite(FLASH_PIN, HIGH); alarmState = true; }
                    } else {
                        if(alarmState) { digitalWrite(RELAY_PIN, RELAY_OFF); digitalWrite(FLASH_PIN, LOW); alarmState = false; }
                    }
                    
                    // Check lệnh tắt từ App
                    if ((millis() / 2000) % 2 == 0) {
                        if (Firebase.RTDB.getBool(&fbDO, "/system_status/alarm_active")) {
                            if (fbDO.boolData() == false) break; // Thoát nếu App tắt
                        }
                    }
                    delay(10);
                }
           }
      } else {
           Serial.println(">>> MOTION DETECTED BUT IGNORED DUE TO SCHEDULE <<<");
           // Có thể log nhẹ vào Firebase là "Phát hiện chuyển động ngoài giờ" nếu muốn
      }
  }
  // ===========================================
  // TRƯỜNG HỢP 2: MẤT MẠNG (OFFLINE)
  // ===========================================
  else {
      Serial.println("\nWiFi Failed! Mode: OFFLINE");
      
      // Ở chế độ Offline, ta KHÔNG biết giờ, nên buộc phải báo động để an toàn
      // Hoặc nếu bạn muốn Offline thì im lặng luôn thì xóa đoạn dưới đi.
      // Ở đây tôi giữ nguyên logic: Offline = Báo động luôn.

      String filename = getNextOfflineFileName();
      fs::FS &fs = SD_MMC;
      File file = fs.open(filename, FILE_WRITE);
      if(file){
        file.write(fb->buf, fb->len);
        file.close();
      }
      
      unsigned long startOfflineAlarm = millis();
      bool alarmState = false;
      while (digitalRead(PIR_PIN) == HIGH || millis() - startOfflineAlarm < 5000) {
          if (millis() - startOfflineAlarm > 60000) break;
          if ((millis() / 250) % 2 == 0) { 
               if(!alarmState) { digitalWrite(RELAY_PIN, RELAY_ON); digitalWrite(FLASH_PIN, HIGH); alarmState = true; }
           } else {
               if(alarmState) { digitalWrite(RELAY_PIN, RELAY_OFF); digitalWrite(FLASH_PIN, LOW); alarmState = false; }
           }
           delay(10);
      }
  }

  esp_camera_fb_return(fb);

  // Tắt hết
  digitalWrite(RELAY_PIN, RELAY_OFF);
  digitalWrite(FLASH_PIN, LOW);
  digitalWrite(LED_PIN, HIGH);

  Serial.println("Going to sleep...");
  rtc_gpio_hold_dis(GPIO_NUM_12);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_13, 1);
  esp_deep_sleep_start();
}

void loop() {}