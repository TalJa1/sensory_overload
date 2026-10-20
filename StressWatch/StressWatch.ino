#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <ESP_I2S.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>

// ==========================================
// CẤU HÌNH WIFI & SUPABASE CLOUD
// ==========================================
const char* DEFAULT_WIFI_SSID = "American Study";
const char* DEFAULT_WIFI_PASS = "66668888";
const char* AP_SSID = "SensoryWatch";

String activeWifiSSID = "";
String activeWifiPass = "";

Preferences preferences;
DNSServer dnsServer;
WebServer server(80);

const char* SUPABASE_REST_URL = "https://nfczwxhmyijdbvceeygi.supabase.co/rest/v1/sensory_logs";
const char* SUPABASE_ANON_KEY = "sb_publishable_f_a5LR75HDlrZ10ulnX4Mg_FPIdwXwT";

// ==========================================
// CẤU HÌNH CHÂN PHẦN CỨNG
// ==========================================
#define I2C_SDA_PIN   21
#define I2C_SCL_PIN   22

#define I2S_SCK       26
#define I2S_WS        25
#define I2S_SD        33

#define GSR_PIN       34

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
MAX30105 particleSensor;
I2SClass i2s;

// ==========================================
// BIẾN CHIA SẺ ĐA LUỒNG
// ==========================================
volatile long globalMicVol = 0;
volatile long peakMicVol = 0;
volatile int sharedBPM = 0;
volatile int sharedGSR = 0;
volatile long sharedMic = 0;
volatile int sharedStress = 0;
volatile bool sharedIsWorn = false; // Trạng thái phát hiện đeo
volatile bool wifiConnected = false;
volatile bool beatDetected = false;

// Biến tính nhịp tim
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float bpm = 0;
int avgBPM = 0;

// Trạng thái luân chuyển màn hình OLED
int activeScreen = 0; // 0: Tâm Trạng Kawaii, 1: Thước Đo Giác Quan
unsigned long lastScreenSwitch = 0;

int lastMicScore = 0;
int lastGsrScore = 0;
int lastBpmScore = 0;

// Biến theo dõi tiếng ồn kéo dài (chỉ cảnh báo khi ồn lớn liên tục >= 5s)
unsigned long loudNoiseStartTime = 0;
unsigned long lastLoudNoiseTime = 0;
unsigned long loudNoiseDuration = 0;

// ==========================================
// QUẢN LÝ TIẾT KIỆM NĂNG LƯỢNG THÔNG MINH (SMART POWER MANAGEMENT)
// ==========================================
enum PowerState {
  PWR_ACTIVE = 0,         // Màn hình bật, gửi Cloud bình thường (4s)
  PWR_STANDBY,            // Đang đeo, êm ả > 1p -> Tắt màn hình, gửi Cloud giãn ra (15s)
  PWR_UNWORN_SLEEP        // Tháo đồng hồ > 1p -> Tắt màn hình, tạm dừng gửi Cloud
};

volatile PowerState currentPowerState = PWR_ACTIVE;
bool isDisplaySleeping = false;
unsigned long lastActivityTime = 0;   // Mốc thời gian kích động/hoạt động gần nhất
unsigned long unwornStartTime = 0;    // Mốc thời gian bắt đầu tháo đồng hồ
const unsigned long TIMEOUT_SLEEP_MS = 60000; // 60 giây (1 phút)

// ==========================================
// THUẬT TOÁN TÍNH ĐIỂM QUÁ TẢI (SENSORY OVERLOAD)
// ==========================================
int calculateStressScore(int bpmVal, int gsrVal, long micVal, bool isWorn) {
  // CÁC NGƯỠNG TIẾNG ỒN:
  // - Khi ĐANG ĐEO: Cảnh báo quá tải khi ồn kéo dài >= 1.500.000 trong 5s
  // - Khi CHƯA ĐEO: Cảnh báo môi trường khi ồn cực lớn >= 2.500.000 (tiếng hét, đổ vỡ, tiếng động lớn trong phòng)
  const long WORN_LOUD_THRESHOLD = 1500000;
  const long UNWORN_LOUD_THRESHOLD = 2500000;
  const unsigned long SUSTAIN_REQUIRED_MS = 5000; // 5 giây khi đang đeo
  const unsigned long PAUSE_GRACE_MS = 1000;      // 1 giây ân hạn nếu ngắt quãng ngắn để lấy hơi

  unsigned long now = millis();

  // NẾU BÉ CHƯA ĐEO ĐỒNG HỒ:
  if (!isWorn) {
    lastGsrScore = 0;
    lastBpmScore = 0;
    loudNoiseStartTime = 0;
    loudNoiseDuration = 0;

    // Khi chưa đeo: Nếu tiếng ồn môi trường vượt ngưỡng cực đại 2.500.000 -> BẬT CẢNH BÁO MÔI TRƯỜNG!
    if (micVal >= UNWORN_LOUD_THRESHOLD) {
      lastMicScore = 80;
      return 80; // Trả về mức cảnh báo 80% (>= 70%) để OLED bật sáng cảnh báo
    } else {
      lastMicScore = 0;
      return 0; // Yên tĩnh khi chưa đeo
    }
  }

  // 1. Nhịp Tim (BPM): 75-85 bpm là bình thường. > 115 bpm là hoảng loạn
  int bpmScore = 0;
  if (bpmVal > 0) {
    if (bpmVal <= 80) bpmScore = 0;
    else if (bpmVal >= 125) bpmScore = 100;
    else bpmScore = map(bpmVal, 80, 125, 0, 100);
  }

  // 2. Điện Trở Da (GSR): Bình thường 320-420. Kích động > 650
  int gsrScore = 0;
  if (gsrVal > 250) {
    if (gsrVal <= 420) gsrScore = 0;
    else if (gsrVal >= 750) gsrScore = 100;
    else gsrScore = map(gsrVal, 420, 750, 0, 100);
  }

  // 3. Tiếng Ồn (Mic INMP441) - Khi đang đeo:
  if (micVal >= WORN_LOUD_THRESHOLD) {
    lastLoudNoiseTime = now;
    if (loudNoiseStartTime == 0) {
      loudNoiseStartTime = now;
    }
    loudNoiseDuration = now - loudNoiseStartTime;
  } else {
    // Nếu âm thanh dịu xuống quá 1 giây thì reset bộ đếm thời gian
    if (now - lastLoudNoiseTime > PAUSE_GRACE_MS) {
      loudNoiseStartTime = 0;
      loudNoiseDuration = 0;
    }
  }

  int micScore = 0;
  if (micVal <= 50000) {
    micScore = 0;
  } else if (micVal < WORN_LOUD_THRESHOLD) {
    micScore = map(micVal, 50000, WORN_LOUD_THRESHOLD, 0, 30);
  } else {
    // micVal >= 1.500.000 (Tiếng hét / ồn lớn khi đeo)
    if (loudNoiseDuration < SUSTAIN_REQUIRED_MS) {
      // Chưa đủ 5 giây -> Tối đa 40%, không thể kích hoạt cảnh báo quá tải
      micScore = map(loudNoiseDuration, 0, SUSTAIN_REQUIRED_MS, 20, 40);
    } else {
      // ĐÃ KÉO DÀI ĐỦ >= 5 GIÂY -> KÍCH HOẠT CẢNH BÁO QUÁ TẢI (Điểm 75 - 100%)
      micScore = map(constrain(micVal, WORN_LOUD_THRESHOLD, 3000000), WORN_LOUD_THRESHOLD, 3000000, 75, 100);
    }
  }

  lastMicScore = micScore;
  lastGsrScore = gsrScore;
  lastBpmScore = bpmScore;

  // NGUYÊN TẮC: BẤT KỲ KÍCH THÍCH NÀO ĐẠT NGƯỠNG NGUY HIỂM -> BÁO ĐỘNG NGAY!
  int maxSingle = micScore;
  if (gsrScore > maxSingle) maxSingle = gsrScore;
  if (bpmScore > maxSingle) maxSingle = bpmScore;

  // Điểm cộng hưởng từ các kênh phụ
  int compoundBonus = 0;
  if (maxSingle == micScore) {
    compoundBonus = (gsrScore + bpmScore) / 4;
  } else if (maxSingle == gsrScore) {
    compoundBonus = (micScore + bpmScore) / 4;
  } else {
    compoundBonus = (micScore + gsrScore) / 4;
  }

  int totalScore = maxSingle + compoundBonus;
  return constrain(totalScore, 0, 100);
}

// ==========================================
// TASK 1: ĐỌC MICRO INMP441 & KHỬ NHIỄU NỀN DC (CORE 0)
// ==========================================
void TaskMic(void *pvParameters) {
  i2s.setPins(I2S_SCK, I2S_WS, -1, I2S_SD, -1);
  if (!i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("Loi I2S!");
  }

  int32_t samples[64];
  unsigned long lastDecayTime = 0;
  int32_t dcOffset = 0;

  for (;;) {
    size_t bytes_read = i2s.readBytes((char*)samples, sizeof(samples));
    if (bytes_read > 0) {
      int samples_read = bytes_read / sizeof(int32_t);
      int64_t sum = 0;
      for (int i = 0; i < samples_read; i++) {
        int32_t sample = samples[i] >> 8;
        // Khử trôi DC Offset
        dcOffset = (dcOffset * 63 + sample) / 64;
        int32_t acSample = sample - dcOffset;
        sum += abs((int)acSample);
      }
      
      if (samples_read > 0) {
        long currentAmp = sum / samples_read;
        // Bắt đỉnh tức thời cực nhạy
        if (currentAmp > peakMicVol) {
          peakMicVol = currentAmp;
        }
      }
    }

    // Mỗi 100ms, đỉnh âm thanh xả 25% (Hết ồn thì sau 1.2s màn hình trở lại bình thường)
    if (millis() - lastDecayTime >= 100) {
      lastDecayTime = millis();
      peakMicVol = (peakMicVol * 75) / 100;
      globalMicVol = peakMicVol;
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ==========================================
// CÁC MÀN HÌNH OLED CHO QUÁ TRÌNH KẾT NỐI WIFI
// ==========================================
void showConnectingScreen(const char* targetSSID, int attempt, int maxAttempts) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  // Header
  display.setTextSize(1);
  display.setCursor(20, 2);
  display.print("SENSORY WATCH");
  display.drawFastHLine(0, 12, 128, SH110X_WHITE);

  // Thông tin kết nối
  display.setCursor(8, 17);
  display.print("Dang ket noi WiFi:");

  display.drawRoundRect(4, 28, 120, 18, 4, SH110X_WHITE);
  display.setCursor(8, 33);
  char buf[20];
  snprintf(buf, sizeof(buf), "%.16s", targetSSID);
  display.print(buf);

  // Thanh tiến trình
  display.drawRoundRect(14, 50, 100, 10, 3, SH110X_WHITE);
  int barW = map(attempt, 0, maxAttempts, 2, 96);
  if (barW > 0) display.fillRoundRect(16, 52, barW, 6, 2, SH110X_WHITE);

  display.display();
}

void showConnectedScreen(const char* connectedSSID, IPAddress ip) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  display.drawRoundRect(4, 4, 120, 56, 6, SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(14, 10);
  display.print("DA KET NOI WIFI!");
  display.drawFastHLine(10, 22, 108, SH110X_WHITE);

  display.setCursor(10, 26);
  char buf[22];
  snprintf(buf, sizeof(buf), "Net: %.13s", connectedSSID);
  display.print(buf);

  display.setCursor(10, 37);
  display.print("IP: ");
  display.print(ip);

  display.setCursor(44, 47);
  display.print("(^ _ ^)");

  display.display();
}

void showAPScreen(unsigned long uptime) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  // Header
  display.setTextSize(1);
  display.setCursor(14, 2);
  display.print("CAU HINH WIFI AP");
  display.drawFastHLine(0, 12, 128, SH110X_WHITE);

  // Khung thông tin phát AP
  display.drawRoundRect(4, 15, 120, 34, 4, SH110X_WHITE);
  display.setCursor(8, 20);
  display.print("WiFi: SensoryWatch");
  display.setCursor(8, 30);
  display.print("Pass: (Khong co)");
  display.setCursor(8, 40);
  display.print("Web:  192.168.4.1");

  // Footer hoạt ảnh chờ
  display.drawFastHLine(0, 51, 128, SH110X_WHITE);
  display.setCursor(8, 54);
  int dots = (uptime / 400) % 4;
  display.print("Dang cho dien thoai");
  for (int i = 0; i < dots; i++) display.print(".");

  display.display();
}

void showReceivingScreen(const char* newSsid) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  display.drawRoundRect(4, 8, 120, 48, 6, SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(14, 14);
  display.print("DA LUU THONG TIN!");
  display.drawFastHLine(10, 25, 108, SH110X_WHITE);
  display.setCursor(10, 30);
  char buf[22];
  snprintf(buf, sizeof(buf), "Thu: %.14s", newSsid);
  display.print(buf);
  display.setCursor(16, 42);
  display.print("Dang ket noi lai...");

  display.display();
}

// ==========================================
// XỬ LÝ WEB SERVER CẤU HÌNH WIFI (CAPTIVE PORTAL)
// ==========================================
bool newConfigReceived = false;
String pendingSSID = "";
String pendingPASS = "";

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Sensory Watch - Cài Đặt WiFi</title>"
    "<style>"
    "body{font-family:-apple-system,sans-serif;background:#f0f9ff;margin:0;padding:20px;display:flex;justify-content:center;}"
    ".c{background:#fff;max-width:360px;width:100%;padding:24px;border-radius:24px;box-shadow:0 10px 25px rgba(0,0,0,0.06);}"
    "h2{color:#0284c7;margin:0 0 6px;text-align:center;font-size:20px;}"
    "p{color:#64748b;font-size:13px;text-align:center;margin:0 0 16px;line-height:1.4;}"
    "label{font-size:13px;font-weight:600;color:#334155;margin-bottom:6px;display:block;}"
    "select,input[type='text'],input[type='password']{width:100%;padding:12px;margin-bottom:12px;border:1.5px solid #cbd5e1;border-radius:12px;font-size:14px;box-sizing:border-box;}"
    "select:focus,input:focus{border-color:#0284c7;outline:none;}"
    ".toggle-label{font-size:13px;font-weight:normal;color:#475569;display:flex;align-items:center;margin-top:-4px;margin-bottom:14px;cursor:pointer;}"
    ".toggle-label input{width:18px;height:18px;margin:0 8px 0 0;cursor:pointer;}"
    ".notice{background:#fff1f2;border:1px solid #fecdd3;padding:10px 12px;border-radius:10px;font-size:12px;color:#be123c;margin-bottom:16px;line-height:1.4;}"
    "button{width:100%;background:linear-gradient(135deg,#0284c7,#6366f1);color:#fff;border:none;padding:14px;border-radius:12px;font-size:15px;font-weight:bold;cursor:pointer;box-shadow:0 4px 12px rgba(2,132,199,0.25);}"
    "button:active{transform:scale(0.98);}"
    ".f{font-size:11px;text-align:center;color:#94a3b8;margin-top:16px;}"
    "</style></head><body><div class='c'>"
    "<h2>🧸 Sensory Watch</h2>"
    "<p>Chọn mạng WiFi để đồng hồ đồng bộ dữ liệu của bé lên hệ thống.</p>"
    "<form method='POST' action='/save'>"
    "<label>Mạng WiFi xung quanh (2.4GHz):</label>"
    "<select name='ssid' id='ssid' onchange='toggleCustom(this)'>";

  int n = WiFi.scanNetworks();
  if (n <= 0) {
    html += "<option value=''>Không tìm thấy mạng nào (hoặc đang quét)</option>";
  } else {
    for (int i = 0; i < n; ++i) {
      String s = WiFi.SSID(i);
      if (s.length() > 0) {
        html += "<option value='" + s + "'>" + s + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
      }
    }
    WiFi.scanDelete();
  }

  html += "<option value='__custom__'>-- Tự nhập tên mạng khác --</option>"
    "</select>"
    "<input type='text' name='custom_ssid' id='custom_ssid' placeholder='Nhập tên WiFi...' style='display:none;' autocapitalize='none' autocorrect='off' spellcheck='false'>"
    "<label>Mật khẩu WiFi:</label>"
    "<input type='password' name='password' id='pwd' placeholder='Nhập mật khẩu WiFi...' autocapitalize='none' autocorrect='off' spellcheck='false'>"
    "<label class='toggle-label'>"
    "<input type='checkbox' onclick='togglePwd()'> Hiện mật khẩu để kiểm tra"
    "</label>"
    "<div class='notice'>"
    "⚠️ <b>Lưu ý:</b> ESP32 chỉ hỗ trợ WiFi băng tần <b>2.4GHz</b> (không hỗ trợ 5GHz). Hãy chắc chắn bạn chọn đúng mạng 2.4GHz."
    "</div>"
    "<button type='submit'>Lưu & Kết Nối Ngay ✨</button>"
    "</form><div class='f'>Sensory Overload Monitoring System</div></div>"
    "<script>"
    "function toggleCustom(sel){var el=document.getElementById('custom_ssid');if(sel.value==='__custom__'){el.style.display='block';el.required=true;}else{el.style.display='none';el.required=false;}}"
    "function togglePwd(){var p=document.getElementById('pwd');p.type=(p.type==='password')?'text':'password';}"
    "</script>"
    "</body></html>";

  server.send(200, "text/html", html);
}

void handleSave() {
  String s = server.arg("ssid");
  if (s == "__custom__") {
    s = server.arg("custom_ssid");
  }
  s.trim();
  pendingSSID = s;
  pendingPASS = server.arg("password");
  newConfigReceived = true;

  String res = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Đã Nhận Cấu Hình</title>"
    "<style>body{font-family:-apple-system,sans-serif;text-align:center;padding:40px 20px;background:#f0fdf4;color:#1e293b;}"
    ".box{max-width:320px;margin:0 auto;background:#fff;padding:24px;border-radius:20px;box-shadow:0 8px 20px rgba(0,0,0,0.06);}"
    "</style></head><body><div class='box'>"
    "<h2>🧸 Đã Lưu Thông Tin!</h2>"
    "<p style='color:#059669;font-weight:600;'>Đang thử kết nối vào: " + pendingSSID + "</p>"
    "<p style='color:#64748b;font-size:13px;'>Đồng hồ đang chuyển sang kết nối trực tiếp với router. Vui lòng theo dõi màn hình OLED trên đồng hồ nhé!</p>"
    "</div></body></html>";

  server.send(200, "text/html", res);
}

void handleNotFound() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void initAndConnectWiFi() {
  // 1. Đọc WiFi lưu trong bộ nhớ NVS Preferences
  preferences.begin("sensory_wifi", false);
  activeWifiSSID = preferences.getString("ssid", DEFAULT_WIFI_SSID);
  activeWifiPass = preferences.getString("pass", DEFAULT_WIFI_PASS);

  Serial.printf("[WiFi] Thu ket noi toi: %s\n", activeWifiSSID.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Giữ radio ổn định, chống timeout bắt tay WPA2

  if (activeWifiSSID.length() > 0) {
    if (activeWifiPass.length() == 0) {
      WiFi.begin(activeWifiSSID.c_str());
    } else {
      WiFi.begin(activeWifiSSID.c_str(), activeWifiPass.c_str());
    }

    bool connected = false;
    const int MAX_ATTEMPTS = 25; // 25 * 500ms = 12.5 giây
    for (int i = 0; i <= MAX_ATTEMPTS; i++) {
      showConnectingScreen(activeWifiSSID.c_str(), i, MAX_ATTEMPTS);
      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        break;
      }
      delay(500);
    }

    if (connected) {
      wifiConnected = true;
      Serial.printf("[WiFi] Ket noi thanh cong! Web: sensory.hen.id.vn\n");
      showConnectedScreen(activeWifiSSID.c_str(), WiFi.localIP());
      delay(1800);
      return;
    }
  }

  // 2. Nếu thất bại -> Bật chế độ AP phát WiFi để cấu hình
  Serial.println("[WiFi] Khong the ket noi. Bat che do AP de cau hinh...");
  WiFi.disconnect(true);
  delay(100);

  WiFi.mode(WIFI_AP_STA);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID);
  dnsServer.start(53, "*", apIP);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  unsigned long lastOledRefresh = 0;
  newConfigReceived = false;
  bool connected = false;

  while (!connected) {
    dnsServer.processNextRequest();
    server.handleClient();

    if (millis() - lastOledRefresh >= 300) {
      lastOledRefresh = millis();
      showAPScreen(millis());
    }

    if (newConfigReceived) {
      newConfigReceived = false;
      showReceivingScreen(pendingSSID.c_str());

      // Chờ 1.5 giây để điện thoại nhận trọn vẹn trang web phản hồi trước khi tắt AP
      delay(1500);

      Serial.printf("[WiFi] Thu ket noi WiFi moi: '%s' (Do dai pass: %d)\n", pendingSSID.c_str(), pendingPASS.length());

      // =========================================================================
      // CỰC KỲ QUAN TRỌNG: TẮT SOFTAP ĐỂ TRÁNH XUNG ĐỘT KÊNH (CHANNEL CONFLICT)
      // ESP32 chỉ có 1 phần cứng Radio WiFi duy nhất. Nếu SoftAP đang chạy ở Kênh 1,
      // mà router nhà bạn phát ở Kênh khác (như kênh 6 hay 11), ESP32 sẽ bị rớt gói tin
      // bắt tay WPA2 (4-Way Handshake) do nhảy kênh liên tục, dẫn đến báo SAI MẬT KHẨU dù nhập đúng!
      // Vì vậy ta tạm ngắt SoftAP và chuyển sang thuần WIFI_STA để kết nối router ổn định 100%.
      // =========================================================================
      WiFi.softAPdisconnect(true);
      delay(100);
      WiFi.mode(WIFI_STA);
      WiFi.disconnect(true);
      delay(150);

      WiFi.setSleep(false);
      if (pendingPASS.length() == 0) {
        WiFi.begin(pendingSSID.c_str());
      } else {
        WiFi.begin(pendingSSID.c_str(), pendingPASS.c_str());
      }

      const int NEW_ATTEMPTS = 30; // 30 * 500ms = 15 giây (đủ thời gian router cấp DHCP)
      for (int i = 0; i <= NEW_ATTEMPTS; i++) {
        showConnectingScreen(pendingSSID.c_str(), i, NEW_ATTEMPTS);
        if (WiFi.status() == WL_CONNECTED) {
          connected = true;
          break;
        }
        delay(500);
      }

      if (connected) {
        wifiConnected = true;
        preferences.putString("ssid", pendingSSID);
        preferences.putString("pass", pendingPASS);
        activeWifiSSID = pendingSSID;
        activeWifiPass = pendingPASS;
        Serial.println("[WiFi] Da ket noi va luu cau hinh moi vao Preferences!");

        showConnectedScreen(activeWifiSSID.c_str(), WiFi.localIP());
        delay(2000);

        server.stop();
        dnsServer.stop();
        break;
      } else {
        Serial.println("[WiFi] Ket noi that bai voi thong tin moi. Khoi dong lai che do AP...");
        display.clearDisplay();
        display.drawRoundRect(4, 10, 120, 44, 4, SH110X_WHITE);
        display.setCursor(14, 18);
        display.print("SAI MK HOAC LOI!");
        display.setCursor(14, 32);
        display.print("Thu nhap lai nhe!");
        display.display();
        delay(2500);

        // Bật lại SoftAP để người dùng có thể kết nối lại và nhập lại thông tin
        WiFi.disconnect(true);
        delay(100);
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        WiFi.softAP(AP_SSID);
        delay(100);
        dnsServer.start(53, "*", apIP);
      }
    }

    delay(10);
  }
}

// ==========================================
// TASK 2: GỬI CLOUD SUPABASE (CORE 0)
// ==========================================
void TaskNetwork(void *pvParameters) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(4000);

  for (;;) {
    // 1. Nếu đồng hồ bị tháo ra quá 1 phút và KHÔNG có tiếng ồn nguy hiểm -> Tạm dừng gửi Cloud
    if (currentPowerState == PWR_UNWORN_SLEEP && sharedStress < 70) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    // 2. Chu kỳ gửi: 4s khi Active / Stress, giãn ra 15s khi Standby êm ả
    int cloudIntervalMs = (currentPowerState == PWR_STANDBY) ? 15000 : 4000;

    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;

      HTTPClient https;
      if (https.begin(client, SUPABASE_REST_URL)) {
        https.addHeader("Content-Type", "application/json");
        https.addHeader("apikey", SUPABASE_ANON_KEY);
        https.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
        https.addHeader("Prefer", "return=minimal");

        const char* statusStr = "CHƯA ĐEO ĐỒNG HỒ";
        if (sharedIsWorn) {
          if (sharedStress >= 70) statusStr = "QUÁ TẢI CẢM GIÁC (OVERLOAD)";
          else if (sharedStress >= 40) statusStr = "KÍCH THÍCH CAO";
          else statusStr = "BÌNH THƯỜNG";
        } else {
          if (sharedStress >= 70) statusStr = "MÔI TRƯỜNG QUÁ ỒN (CHƯA ĐEO)";
        }

        char jsonPayload[300];
        snprintf(jsonPayload, sizeof(jsonPayload),
          "{\"device_id\":\"SOM_Watch_01\",\"bpm\":%d,\"gsr\":%d,\"mic\":%ld,\"stress_score\":%d,\"overload_status\":\"%s\"}",
          sharedBPM, sharedGSR, sharedMic, sharedStress, statusStr
        );

        int httpCode = https.POST(jsonPayload);
        if (httpCode > 0) {
          Serial.printf("[Cloud] Gui thanh cong (code %d)\n", httpCode);
        } else {
          Serial.printf("[Cloud] Loi POST: %s\n", https.errorToString(httpCode).c_str());
        }
        https.end();
      }
    } else {
      wifiConnected = false;
      static unsigned long lastReconnect = 0;
      if (millis() - lastReconnect >= 10000) {
        lastReconnect = millis();
        Serial.println("[WiFi] Dang ket noi lai WiFi...");
        WiFi.reconnect();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(cloudIntervalMs));
  }
}

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(GSR_PIN, INPUT);

  // 1. I2C Bus
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);

  // 2. Màn hình OLED SH1106
  if (display.begin(0x3C, true)) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    
    // Logo chào mừng
    display.drawRoundRect(12, 10, 104, 44, 8, SH110X_WHITE);
    display.setTextSize(1);
    display.setCursor(24, 20);
    display.println("SENSORY WATCH");
    display.setCursor(30, 36);
    display.println("Be An Tam");
    display.display();
    delay(1200);
  }

  // 3. Khởi động và kiểm tra kết nối WiFi (có màn hình hiển thị & AP Portal)
  initAndConnectWiFi();

  // 4. MAX30102
  if (particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    particleSensor.setup(0x1F, 4, 2, 400, 411, 4096);
    particleSensor.setPulseAmplitudeRed(0x0A);
    particleSensor.setPulseAmplitudeIR(0x1F);
  }

  // 5. Khởi tạo 2 Task trên Core 0
  xTaskCreatePinnedToCore(TaskMic, "TaskMic", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskNetwork, "TaskNetwork", 8192, NULL, 1, NULL, 0);

  lastScreenSwitch = millis();
  lastActivityTime = millis();
}

// ==========================================
// LOOP CHÍNH: ĐO TIM & RENDER GIAO DIỆN (CORE 1)
// ==========================================
void loop() {
  // 1. Đọc MAX30102
  long irValue = particleSensor.getIR();
  bool fingerAttached = (irValue > 35000);

  // Chỉ nghỉ nhẹ khi đang tháo tay ngủ chờ VÀ cảm biến hở hoàn toàn ngoài không khí
  // Khi có vật thể/cổ tay chạm vào, CPU chạy full tốc độ để bắt xung nhịp tim tức thì
  if (currentPowerState == PWR_UNWORN_SLEEP && irValue < 30000) {
    delay(50);
  }

  if (fingerAttached) {
    if (checkForBeat(irValue)) {
      long delta = millis() - lastBeat;
      lastBeat = millis();

      bpm = 60.0 / (delta / 1000.0);

      if (bpm > 45 && bpm < 185) {
        rates[rateSpot++] = (byte)bpm;
        rateSpot %= RATE_SIZE;

        int total = 0;
        for (byte i = 0; i < RATE_SIZE; i++) total += rates[i];
        avgBPM = total / RATE_SIZE;
        beatDetected = true;
      }
    } else {
      // Nếu có vật chạm quang (như mặt bàn, vải) nhưng quá 4.5 giây không có xung tim
      // -> Xác định không có nhịp tim, reset avgBPM về 0
      if (lastBeat > 0 && (millis() - lastBeat > 4500)) {
        avgBPM = 0;
      }
    }
  } else {
    avgBPM = 0;
    rateSpot = 0;
    lastBeat = 0;
  }

  // 2. Chuyển đổi màn hình tự động mỗi 10 giây khi bình thường
  if (millis() - lastScreenSwitch >= 10000) {
    lastScreenSwitch = millis();
    activeScreen = (activeScreen + 1) % 2;
  }

  // 3. Render OLED mỗi 200ms
  static unsigned long lastRender = 0;
  if (millis() - lastRender >= 200) {
    lastRender = millis();

    int gsrRaw = analogRead(GSR_PIN);
    // ESP32 ADC là 12-bit (0 - 4095), quy đổi về chuẩn 10-bit (0 - 1023) để khớp hệ thống Web & Cloud
    int gsrVal = gsrRaw / 4;
    long micVol = globalMicVol;

    // ========================================================
    // PHÁT HIỆN ĐEO ĐỒNG HỒ (ON-WRIST DETECTION):
    // Nguyên tắc: Phải đo được nhịp tim sinh học thực tế đang đập!
    // Nếu hoàn toàn không đo được nhịp tim (avgBPM == 0 hoặc mất nhịp quá 4.5s),
    // kể cả cảm biến quang bị phản xạ (đặt trên bàn) hay GSR có chạm tĩnh điện,
    // thì vẫn xác định 100% là KHÔNG ĐEO ĐỒNG HỒ.
    // ========================================================
    bool hasHeartbeat = (avgBPM > 0 && lastBeat > 0 && (millis() - lastBeat <= 4500));
    bool isWorn = fingerAttached && hasHeartbeat;

    // Tính điểm quá tải
    int currentStress = calculateStressScore(avgBPM, gsrVal, micVol, isWorn);

    sharedBPM = avgBPM;
    sharedGSR = gsrVal;
    sharedMic = micVol;
    sharedStress = currentStress;
    sharedIsWorn = isWorn;

    // ========================================================
    // QUẢN LÝ TIẾT KIỆM NĂNG LƯỢNG THÔNG MINH (SMART POWER)
    // ========================================================
    unsigned long now = millis();

    // Phát hiện thao tác chạm nhẹ cảm biến GSR để đánh thức màn hình
    static int prevGsrForWake = 0;
    if (abs(gsrVal - prevGsrForWake) > 60) {
      lastActivityTime = now;
    }
    prevGsrForWake = gsrVal;

    // Đánh thức ngay khi có dấu hiệu bất thường:
    // - Điểm stress >= 40% (bé kích động hoặc quá tải)
    // - Có tiếng ồn lớn (micVol >= 85000 khi đeo, hoặc >= 2500000 khi không đeo)
    // - Tim đập nhanh bất thường (avgBPM >= 100)
    if (currentStress >= 40 || micVol >= 2500000 || (isWorn && (lastMicScore >= 40 || micVol >= 85000 || avgBPM >= 100))) {
      lastActivityTime = now;
    }

    if (!isWorn) {
      // Nếu có tiếng ồn cực lớn khi chưa đeo (>= 2.500.000) -> Bật sáng màn hình cảnh báo ngay!
      if (currentStress >= 70 || micVol >= 2500000) {
        currentPowerState = PWR_ACTIVE;
        unwornStartTime = now; // Reset bộ đếm ngủ khi đang có cảnh báo ồn
        if (isDisplaySleeping) {
          display.oled_command(SH110X_DISPLAYON);
          isDisplaySleeping = false;
          Serial.println("[Power] Chua deo nhung TIENG ON QUA LON (>= 2.500.000) -> Bat sang man hinh bao dong!");
        }
      } else {
        // TRƯỜNG HỢP A: KHÔNG ĐEO & YÊN TĨNH
        if (unwornStartTime == 0) {
          unwornStartTime = now;
        }
        if (now - unwornStartTime >= TIMEOUT_SLEEP_MS) {
          // Tháo ra quá 1 phút -> Tắt màn hình, tạm dừng Cloud để tiết kiệm pin
          currentPowerState = PWR_UNWORN_SLEEP;
          if (!isDisplaySleeping) {
            display.oled_command(SH110X_DISPLAYOFF);
            isDisplaySleeping = true;
            Serial.println("[Power] Thao dong ho > 1p -> Tat man hinh & tam dung Cloud!");
          }
        } else {
          // Mới tháo ra chưa tới 1 phút -> Giữ màn hình nhắc nhở
          currentPowerState = PWR_ACTIVE;
          if (isDisplaySleeping) {
            display.oled_command(SH110X_DISPLAYON);
            isDisplaySleeping = false;
          }
        }
      }
    } else {
      // TRƯỜNG HỢP B: ĐANG ĐEO ĐỒNG HỒ
      // Nếu vừa mới đeo lại từ lúc tháo ra -> Đánh thức tức thì!
      if (unwornStartTime != 0 || currentPowerState == PWR_UNWORN_SLEEP) {
        unwornStartTime = 0;
        lastActivityTime = now;
        currentPowerState = PWR_ACTIVE;
        if (isDisplaySleeping) {
          display.oled_command(SH110X_DISPLAYON);
          isDisplaySleeping = false;
          Serial.println("[Power] Phat hien da deo lai -> Bat sang man hinh tuc thi!");
        }
      }

      // Đang đeo: Kiểm tra thời gian bình yên
      if (now - lastActivityTime >= TIMEOUT_SLEEP_MS) {
        // Bình thường > 1 phút -> Tắt màn hình, các cảm biến vẫn đo ngầm
        currentPowerState = PWR_STANDBY;
        if (!isDisplaySleeping) {
          display.oled_command(SH110X_DISPLAYOFF);
          isDisplaySleeping = true;
          Serial.println("[Power] Binh thuong > 1p -> Tat man hinh (Standby), cam bien van do ngam 24/7!");
        }
      } else {
        // Đang hoạt động / cảnh báo -> Màn hình sáng
        currentPowerState = PWR_ACTIVE;
        if (isDisplaySleeping) {
          display.oled_command(SH110X_DISPLAYON);
          isDisplaySleeping = false;
          Serial.println("[Power] Co tin hieu bat thuong -> Bat sang man hinh bao dong ngay!");
        }
      }
    }

    // Nếu màn hình đang ngủ -> Bỏ qua phần render để tiết kiệm CPU và I2C bus!
    if (isDisplaySleeping) {
      beatDetected = false;
      return;
    }

    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);

    // ========================================================
    // CHẾ ĐỘ CẢNH BÁO QUÁ TẢI / TIẾNG ỒN NGUY HIỂM (STRESS >= 70)
    // CẢ KHI ĐEO (QUÁ TẢI CẢM GIÁC) HOẶC CHƯA ĐEO (PHÒNG QUÁ ỒN)
    // ========================================================
    if (currentStress >= 70) {
      display.fillRoundRect(0, 0, 128, 14, 3, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
      display.setTextSize(1);
      display.setCursor(14, 3);
      if (!isWorn) {
        display.print("! PHONG QUA ON !");
      } else {
        display.print("! CAN VO VE BE !");
      }
      display.setTextColor(SH110X_WHITE);

      display.setTextSize(2);
      display.setCursor(24, 18);
      display.print("( > _ < )");

      // Chỉ rõ nguyên nhân chính xác
      display.setTextSize(1);
      display.setCursor(4, 36);
      if (!isWorn) {
        display.print("ON MOI TRUONG: ");
        display.printf("%2d%%", currentStress);
      } else if (lastMicScore >= 70) {
        display.print("DO ON QUA MUC: ");
        display.printf("%2d%%", currentStress);
      } else if (lastBpmScore >= 70) {
        display.print("TIM DAP NHANH: ");
        display.printf("%2d%%", currentStress);
      } else if (lastGsrScore >= 70) {
        display.print("CANG THANG: ");
        display.printf("%2d%%", currentStress);
      } else {
        display.print("QUA TAI: ");
        display.printf("%2d%%", currentStress);
      }

      // Khung hành động can thiệp
      display.drawRoundRect(4, 48, 120, 15, 4, SH110X_WHITE);
      display.setCursor(10, 52);
      if (!isWorn) {
        display.print("KIEM TRA PHONG BE");
      } else if (lastMicScore >= 70) {
        display.print("DEO TAI NGHE");
      } else if (lastBpmScore >= 70) {
        display.print("HIT THO SAU");
      } else {
        display.print("OM AP & VO VE");
      }
    }

    // ========================================================
    // CHẾ ĐỘ A: BÉ CHƯA ĐEO ĐỒNG HỒ (BÌNH THƯỜNG KHÔNG ỒN LỚN)
    // ========================================================
    else if (!isWorn) {
      // Header
      display.setTextSize(1);
      display.setCursor(2, 2);
      display.print("SENSORY WATCH");

      display.setCursor(82, 2);
      if (wifiConnected) display.print("[WF:OK]");
      else display.print("[WF:..]");
      display.drawFastHLine(0, 12, 128, SH110X_WHITE);

      // Khung thông báo trung tâm bo tròn
      display.drawRoundRect(10, 16, 108, 32, 6, SH110X_WHITE);
      display.setTextSize(1);
      display.setCursor(18, 22);
      display.print("CHUA DEO DONG HO");

      display.setCursor(44, 34);
      display.print("( . _ . )");

      // Footer nhắc nhở vỗ về
      display.drawFastHLine(0, 50, 128, SH110X_WHITE);
      display.setCursor(10, 54);
      display.print("HAY DEO CHO BE");
    }

    // ========================================================
    // CHẾ ĐỘ C: TÂM TRẠNG BÉ & THANH NĂNG LƯỢNG (SCREEN 0)
    // ========================================================
    else if (activeScreen == 0) {
      display.setTextSize(1);
      display.setCursor(2, 2);
      display.print("BE KHANG");

      display.setCursor(82, 2);
      if (wifiConnected) display.print("[WF:OK]");
      else display.print("[WF:..]");
      display.drawFastHLine(0, 12, 128, SH110X_WHITE);

      // Khung cảm xúc Kawaii
      display.drawRoundRect(14, 15, 100, 31, 6, SH110X_WHITE);

      if (currentStress < 40) {
        display.setTextSize(2);
        display.setCursor(26, 18);
        display.print("(^_^)");

        display.setTextSize(1);
        display.setCursor(34, 35);
        display.print("BE AN TAM");
      } else {
        display.setTextSize(2);
        display.setCursor(26, 18);
        display.print("(o_o)");

        display.setTextSize(1);
        display.setCursor(26, 35);
        display.print("HOI KICH DONG");
      }

      // Thanh Gauge cảm giác bo tròn
      display.drawRoundRect(8, 49, 82, 12, 4, SH110X_WHITE);
      int barWidth = map(constrain(currentStress, 0, 100), 0, 100, 0, 76);
      if (barWidth > 0) {
        display.fillRoundRect(11, 52, barWidth, 6, 2, SH110X_WHITE);
      }

      display.setTextSize(1);
      display.setCursor(96, 51);
      display.printf("%2d%%", currentStress);
    }

    // ========================================================
    // CHẾ ĐỘ D: THƯỚC ĐO GIÁC QUAN (SCREEN 1)
    // Căn chỉnh tuyệt đối các cột: không tràn, không đè chữ!
    // ========================================================
    else {
      display.setTextSize(1);
      display.setCursor(2, 1);
      display.print("GIAC QUAN");

      display.setCursor(80, 1);
      display.printf("DO:%2d%%", currentStress);
      display.drawFastHLine(0, 11, 128, SH110X_WHITE);

      // 1. Nhịp tim (TIM) - Cột X = 2, 24, 40, Thước đo X = 86 -> 124
      display.setCursor(2, 14);
      display.print("TIM");
      display.setCursor(24, 14);
      display.print(beatDetected ? "<3" : "  ");
      display.setCursor(40, 14);
      if (avgBPM > 0) {
        display.printf("%3dbpm", avgBPM);
      } else {
        display.print("Dang do");
      }
      display.drawRoundRect(86, 14, 40, 8, 3, SH110X_WHITE);
      int bpmBar = (avgBPM > 0) ? map(constrain(avgBPM, 65, 125), 65, 125, 2, 36) : 0;
      if (bpmBar > 0) display.fillRoundRect(88, 16, bpmBar, 4, 1, SH110X_WHITE);

      // 2. Phản xạ da (DA)
      display.setCursor(2, 27);
      display.print("DA ");
      display.setCursor(24, 27);
      if (gsrVal < 420) {
        display.print("Diu em ");
      } else if (gsrVal < 650) {
        display.print("Hoi am ");
      } else {
        display.print("Mo hoi!");
      }
      display.drawRoundRect(86, 27, 40, 8, 3, SH110X_WHITE);
      int gsrBar = (gsrVal > 300) ? map(constrain(gsrVal, 300, 750), 300, 750, 2, 36) : 0;
      if (gsrBar > 0) display.fillRoundRect(88, 29, gsrBar, 4, 1, SH110X_WHITE);

      // 3. Tiếng ồn môi trường (ON)
      display.setCursor(2, 40);
      display.print("ON ");
      display.setCursor(24, 40);
      if (micVol < 50000) {
        display.print("Yen ang");
      } else if (micVol < 85000) {
        display.print("Vua phai");
      } else {
        if (loudNoiseDuration >= 5000) {
          display.print("On ao!");
        } else {
          int sec = (loudNoiseDuration / 1000) + 1;
          if (sec > 5) sec = 5;
          display.printf("On:%ds  ", sec);
        }
      }
      display.drawRoundRect(86, 40, 40, 8, 3, SH110X_WHITE);
      int micBar = (micVol > 45000) ? map(constrain(micVol, 45000, 140000), 45000, 140000, 2, 36) : 0;
      if (micBar > 0) display.fillRoundRect(88, 42, micBar, 4, 1, SH110X_WHITE);

      // Footer
      display.drawFastHLine(0, 51, 128, SH110X_WHITE);
      display.setCursor(10, 54);
      if (currentStress < 40) {
        display.print("[ BE DANG ON ]");
      } else {
        display.print("[ BE CANG THANG ]");
      }
    }

    display.display();
    beatDetected = false;
  }
}
