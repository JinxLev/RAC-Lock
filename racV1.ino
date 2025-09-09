/*
  RAC Smart Lock — ESP32 + RFID + Servo + I2C LCD
  Firmware FSM v0.1 — non‑blocking, Firebase‑driven, event‑logged

  Fixes in this build
  - Added lcdInit() implementation
  - Removed missing lock_state.h and inlined Mode + modeName()
  - Fixed legacy token name (FIREBASE_AUTH)
  - Safer LCD printing/padding; Wire.begin(SDA,SCL)
  - Tidied Firebase stream parsing
*/

#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <ESP32Servo.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoOTA.h>
#include <time.h>

// ======= FSM (inlined replacement for lock_state.h) =======
enum class Mode { LOCKED, UNLOCKED, UNLOCKING, LOCKING, ERROR };
static const char* modeName(Mode m){
  switch(m){
    case Mode::LOCKED:     return "LOCKED";
    case Mode::UNLOCKED:   return "UNLOCKED";
    case Mode::UNLOCKING:  return "UNLOCKING";
    case Mode::LOCKING:    return "LOCKING";
    default:               return "ERROR";
  }
}
static Mode mode_ = Mode::LOCKED;
bool showingwelcome_ = false;

// ======= USER FILL (do NOT commit secrets) =======
#define WIFI_SSID     "abc"
#define WIFI_PASSWORD "linuxabc"
#define FIREBASE_HOST "https://rac-app-63e7f-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "b9xaDAbvYoGS05DnqaE2t97LaK9vu2gyDbLLh44f" // legacy DB secret
// ================================================

// ======= NTP Configuration =======
#define NTP_SERVER "pool.ntp.org" // Or your preferred NTP server
#define GMT_OFFSET_SEC 0          // Adjust for your timezone in seconds (e.g., GMT+7 = 7*3600 = 25200)
#define DAYLIGHT_OFFSET_SEC 0     // Adjust for daylight saving if needed (e.g., 3600)
// ================================

// Firebase globals
FirebaseConfig fbConfig;
FirebaseAuth   fbAuth;
FirebaseData   fbdo;            // general ops
FirebaseData   fbStreamConfig;  // /config stream
FirebaseData   fbStreamActions; // /actions stream

// UID helper
static String uidToString(const MFRC522::Uid &u) {
  String s; s.reserve(u.size * 2);
  for (byte i = 0; i < u.size; i++) {
    if (u.uidByte[i] < 0x10) s += '0';
    s += String(u.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// ======= Config (cloud‑driven) =======
struct ConfigServo { int pin=22, min_us=500, max_us=2400, unlock_angle=0, lock_angle=90; };
struct ConfigLock  { unsigned long auto_lock_ms=8000; bool rfid_enable=true, remote_enable=true; };
struct ConfigLCD   { bool enable=true; int sda=27, scl=26, address=0x27, cols=16, rows=2; };
struct ConfigApp   { unsigned long poll_ms=250; unsigned long servo_timeout_ms=2000; };
struct ConfigRoot  { ConfigServo servo; ConfigLock lock; ConfigLCD lcd; ConfigApp app; } CFG;

// ======= Hardware objects (late‑bound) =======
LiquidCrystal_I2C *lcd = nullptr;
MFRC522 *rfid = nullptr;  // pins baked at boot
Servo servo;

int  servoAngle_ = 90;           // mirror
unsigned long lastUnlockMs_ = 0; // for auto‑lock
unsigned long moveStartMs_ = 0;  // stall detection
unsigned long lastActionTs_ = 0; // last processed action ts

// ======= Utils: Firebase RTDB helpers =======
static bool ensure(const String &path, FirebaseJson &json){ return Firebase.RTDB.updateNode(&fbdo, path, &json); }
static bool setStr (const String &p, const String &v){ return Firebase.RTDB.setString(&fbdo, p, v); }
static bool setInt (const String &p, int v){ return Firebase.RTDB.setInt(&fbdo, p, v); }
static bool setBool(const String &p, bool v){ return Firebase.RTDB.setBool(&fbdo, p, v); }
static bool setJson(const String &p, FirebaseJson &j){ return Firebase.RTDB.setJSON(&fbdo, p, &j); }
static bool pushJson(const String &p, FirebaseJson &j){ return Firebase.RTDB.pushJSON(&fbdo, p, &j); }

static bool getInt(const String &p, int &out){ if(!Firebase.RTDB.getInt(&fbdo, p)) return false; out=fbdo.intData(); return true; }
static bool getUL (const String &p, unsigned long &out){ if(!Firebase.RTDB.getInt(&fbdo, p)) return false; out=(unsigned long)fbdo.intData(); return true; }
static bool getBool(const String &p, bool &out){ if(!Firebase.RTDB.getBool(&fbdo, p)) return false; out=fbdo.boolData(); return true; }

// ======= LCD helpers =======
static void lcdInit(){
  if(lcd){ delete lcd; lcd=nullptr; }
  if(!CFG.lcd.enable) return;
  Wire.begin(CFG.lcd.sda, CFG.lcd.scl);
  lcd = new LiquidCrystal_I2C(CFG.lcd.address, CFG.lcd.cols, CFG.lcd.rows);
  lcd->init();
  lcd->backlight();
}

static void lcdLine(int row, const String &msg) {
  if (!lcd) return;
  const int cols = CFG.lcd.cols;
  lcd->setCursor(0, row);
  int n = msg.length(); if (n > cols) n = cols;
  for (int i = 0; i < n; ++i) lcd->print(msg[i]); // print trimmed
  for (int i = n; i < cols; ++i) lcd->print(' '); // pad out the line
}

// ======= Events & State =======
// static void pushEvent(const String &type, const String &by="", const String &uid="", const String &user="", const String &reason="", int duration_ms=-1){
//   FirebaseJson j; unsigned long now = millis();
//   j.set("ts", (int)now);
//   j.set("type", type);
//   if(by.length()) j.set("by", by);
//   if(uid.length()) j.set("uid", uid);
//   if(user.length()) j.set("user", user);
//   if(reason.length()) j.set("reason", reason);
//   if(duration_ms>=0) j.set("duration_ms", duration_ms);
//   pushJson("/events", j);
// }

static void pushEvent(const String &type, const String &by="", const String &uid="", const String &user="", const String &reason="", int duration_ms=-1){
  FirebaseJson j;
  
  // ======= Get current time using NTP =======
  time_t now_seconds = time(nullptr); // Get current time in seconds since epoch
  // Check if time is likely synced (e.g., after Jan 1, 2023)
  // This helps avoid sending the initial 1970 timestamp if NTP hasn't synced yet
  if (now_seconds < 1672531200) { // 1672531200 is Unix timestamp for 2023-01-01 00:00:00 UTC
      Serial.println("Warning: Time not synced, using millis() for event timestamp.");
      // Fallback to millis if time not synced (results in 1970 issue until sync)
      // You might choose to discard the event or handle differently
      now_seconds = millis() / 1000; // Approximate seconds since boot as fallback
      // Or just return; to skip the event if time is critical
  }
  long long now_millis = (long long)now_seconds * 1000LL; // Convert to milliseconds
  // =========================================

  j.set("ts", (long long)now_millis); // Store correct timestamp in milliseconds
  j.set("type", type);
  if(by.length()) j.set("by", by);
  if(uid.length()) j.set("uid", uid);
  if(user.length()) j.set("user", user);
  if(reason.length()) j.set("reason", reason);
  if(duration_ms>=0) j.set("duration_ms", duration_ms);
  pushJson("/events", j);
}

static void publishState(){
  FirebaseJson j;
  j.set("ip", WiFi.localIP().toString());
  j.set("rssi", WiFi.RSSI());
  j.set("mode", modeName(mode_));
  j.set("servo_angle", servoAngle_);
  j.set("needs_reboot", false);
  j.set("version", "0.1.0");
  j.set("uptime_ms", (int)millis());
  setJson("/state", j);
}

// ======= Servo control =======
static void servoAttach(){ if(servo.attached()) servo.detach(); servo.attach(CFG.servo.pin, CFG.servo.min_us, CFG.servo.max_us); }
static void writeServoAngle(int a){ a = constrain(a, 0, 180); servo.write(a); servoAngle_ = a; setInt("/state/servo_angle", servoAngle_); }
// static void setMode(Mode m){ mode_=m; setStr("/state/mode", modeName(mode_)); if(lcd){ lcdLine(1, String("Mode:") + modeName(mode_)); } }
static void setMode(Mode m){ 
  mode_ = m; 
  setStr("/state/mode", modeName(mode_)); 

  if(lcd && !showingwelcome_){  // ✅ Only update LCD if NOT showing welcome
    if(m == Mode::LOCKED || m == Mode::UNLOCKED){
      lcdLine(0, "RAC Smart Lock");
      lcdLine(1, String("Mode:") + modeName(mode_));
    } else {
      lcdLine(1, String("Mode:") + modeName(mode_));
    }
  }

  // ✅ Clear flag when entering steady state
  if(m == Mode::UNLOCKED || m == Mode::LOCKED){
    showingwelcome_ = false;
  }
}
static void doLock(){ setMode(Mode::LOCKING); moveStartMs_ = millis(); writeServoAngle(CFG.servo.lock_angle); }
static void doUnlock(const String &by){ setMode(Mode::UNLOCKING); moveStartMs_ = millis(); writeServoAngle(CFG.servo.unlock_angle); lastUnlockMs_ = millis(); pushEvent("UNLOCK", by); }
// static void doUnlock(const String &by){
//   setMode(Mode::UNLOCKING);
//   moveStartMs_ = millis();
//   writeServoAngle(CFG.servo.unlock_angle);
//   lastUnlockMs_ = millis();
//   pushEvent("UNLOCK", by);

//   // ✅ Show user name on LCD during unlock
//   if(lcd){
//     showingwelcome_ = true;
//     lcdLine(0, "Hello");
//     lcdLine(1, by); // Show user name
//   }
// }

// ======= Config seeding & loading =======
static void seedDefaults(){
  FirebaseJson j;
  FirebaseJson jServo; jServo.set("pin", CFG.servo.pin); jServo.set("min_us", CFG.servo.min_us); jServo.set("max_us", CFG.servo.max_us); jServo.set("unlock_angle", CFG.servo.unlock_angle); jServo.set("lock_angle", CFG.servo.lock_angle);
  FirebaseJson jLock;  jLock.set("auto_lock_ms", (int)CFG.lock.auto_lock_ms); jLock.set("rfid_enable", CFG.lock.rfid_enable); jLock.set("remote_enable", CFG.lock.remote_enable);
  FirebaseJson jLCD;   jLCD.set("enable", CFG.lcd.enable); jLCD.set("sda", CFG.lcd.sda); jLCD.set("scl", CFG.lcd.scl); jLCD.set("address", CFG.lcd.address); jLCD.set("cols", CFG.lcd.cols); jLCD.set("rows", CFG.lcd.rows);
  FirebaseJson jApp;   jApp.set("poll_ms", (int)CFG.app.poll_ms); jApp.set("servo_timeout_ms", (int)CFG.app.servo_timeout_ms);
  j.set("servo", jServo); j.set("lock", jLock); j.set("lcd", jLCD); j.set("app", jApp);
  ensure("/config", j);
}

static void loadConfigFromRTDB(){
  int v; unsigned long ul; bool b;
  // Servo
  if(getInt("/config/servo/pin", v)) CFG.servo.pin=v;
  if(getInt("/config/servo/min_us", v)) CFG.servo.min_us=v;
  if(getInt("/config/servo/max_us", v)) CFG.servo.max_us=v;
  if(getInt("/config/servo/unlock_angle", v)) CFG.servo.unlock_angle=v;
  if(getInt("/config/servo/lock_angle", v)) CFG.servo.lock_angle=v;
  // Lock
  if(getUL("/config/lock/auto_lock_ms", ul)) CFG.lock.auto_lock_ms=ul;
  if(getBool("/config/lock/rfid_enable", b)) CFG.lock.rfid_enable=b;
  if(getBool("/config/lock/remote_enable", b)) CFG.lock.remote_enable=b;
  // LCD
  if(getBool("/config/lcd/enable", b)) CFG.lcd.enable=b;
  if(getInt("/config/lcd/sda", v)) CFG.lcd.sda=v;
  if(getInt("/config/lcd/scl", v)) CFG.lcd.scl=v;
  if(getInt("/config/lcd/address", v)) CFG.lcd.address=v;
  if(getInt("/config/lcd/cols", v)) CFG.lcd.cols=v;
  if(getInt("/config/lcd/rows", v)) CFG.lcd.rows=v;
  // App
  if(getUL("/config/app/poll_ms", ul)) CFG.app.poll_ms=ul;
  if(getUL("/config/app/servo_timeout_ms", ul)) CFG.app.servo_timeout_ms=ul;
}

static void applyConfig(bool first){
  lcdInit();          // LCD hot‑apply
  servoAttach();      // Servo hot‑bind
  writeServoAngle(CFG.servo.lock_angle);
  setMode(Mode::LOCKED);
  if(first){
    if(rfid){ delete rfid; rfid=nullptr; }
    SPI.begin();
    // Default ESP32 MFRC522 pins: SS=5, RST=21 (adjust in code if needed)
    rfid = new MFRC522(5, 21);
    rfid->PCD_Init();
  }
}

// ======= Streams =======
static void onConfigStream(FirebaseStream data){
  Serial.println("[Stream:/config] change");
  loadConfigFromRTDB();
  applyConfig(false);
}
static void onActionsStream(FirebaseStream data){
  Serial.println("[Stream:/actions] event");
  if(data.dataType() != "json") return;
  FirebaseJson *js = data.to<FirebaseJson*>();
  FirebaseJsonData jd;
  String cmd, by; int ts=0, ms=0;
  if(js->get(jd, "cmd")) cmd = jd.to<String>();
  if(js->get(jd, "by")) by = jd.to<String>();
  if(js->get(jd, "ts")) ts = jd.to<int>();
  if(js->get(jd, "ms")) ms = jd.to<int>();
  if(ts <= 0 || (unsigned long)ts <= lastActionTs_) return; // ignore stale
  lastActionTs_ = ts; setInt("/state/last_action_ts", lastActionTs_);
  if(!CFG.lock.remote_enable) { pushEvent("REMOTE", by, "", "", "remote_disabled"); return; }
  if(cmd == "unlock") {
    doUnlock(by);
  } else if(cmd == "lock") {
    doLock(); pushEvent("LOCK", by);
  } else if(cmd == "pulse") {
    unsigned long dur = (ms>0)? (unsigned long)ms : 3000;
    doUnlock(by);
    lastUnlockMs_ = millis() - (CFG.lock.auto_lock_ms - dur); // pulse window
  } else {
    pushEvent("ERROR", by, "", "", String("unknown_cmd:") + cmd);
  }
}
static void onStreamTimeout(bool which){ Serial.printf("[Stream:%s] timeout\n", which?"config":"actions"); }

// ======= RFID authorize =======
static bool isAuthorizedUID(const String &uid, String &userOut){
  // users/<UID> can be string (name) or {name, enabled}
  String path = String("/users/") + uid;
  if(Firebase.RTDB.get(&fbdo, path)){
    if(fbdo.dataType() == "string"){ userOut = fbdo.to<String>(); return userOut.length()>0; }
    if(fbdo.dataType() == "json"){
      FirebaseJson *j = fbdo.to<FirebaseJson*>();
      FirebaseJsonData jd;
      String name; bool enabled=true;
      if(j->get(jd, "name")) name = jd.to<String>();
      if(j->get(jd, "enabled")) enabled = jd.to<bool>();
      if(name.length()>0 && enabled){ userOut=name; return true; }
    }
  }
  return false;
}

// ======= Setup / Loop =======
void setup(){
  Serial.begin(115200);

  // Wi‑Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  while(WiFi.status() != WL_CONNECTED){ delay(100); Serial.print('.'); }
  Serial.println(); Serial.println(WiFi.localIP());

  // ======= Add NTP Initialization Here =======
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  // Optional: Wait a bit for initial sync or check time
  // delay(1000); // Give NTP a moment (optional)

  // Firebase
  fbConfig.database_url = FIREBASE_HOST;
  fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH; // FIXED
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);

  // OTA (optional; change hostname)
  ArduinoOTA.setHostname("rac-smart-lock");
  ArduinoOTA.begin();

  // Seed + load + apply
  seedDefaults();
  loadConfigFromRTDB();
  applyConfig(true);

  // Publish initial state
  publishState();
  pushEvent("BOOT");

  // Streams
  if(Firebase.RTDB.beginStream(&fbStreamConfig, "/config")){
    Firebase.RTDB.setStreamCallback(&fbStreamConfig,
      [](FirebaseStream data){ onConfigStream(data); },
      [](bool /*timeout*/){ onStreamTimeout(true); }
    );
  } else {
    Serial.printf("/config stream err: %s\n", fbStreamConfig.errorReason().c_str());
  }
  if(Firebase.RTDB.beginStream(&fbStreamActions, "/actions")){
    Firebase.RTDB.setStreamCallback(&fbStreamActions,
      [](FirebaseStream data){ onActionsStream(data); },
      [](bool /*timeout*/){ onStreamTimeout(false); }
    );
  } else {
    Serial.printf("/actions stream err: %s\n", fbStreamActions.errorReason().c_str());
  }

  lcdLine(0, "RAC Smart Lock");
  lcdLine(1, String("Mode:") + modeName(mode_));
}

void loop(){
  ArduinoOTA.handle(); // non‑blocking OTA

  unsigned long now = millis();

  // Auto‑lock logic
  if(mode_ == Mode::UNLOCKED){
    if(CFG.lock.auto_lock_ms > 0 && (now - lastUnlockMs_) >= CFG.lock.auto_lock_ms){
      doLock(); pushEvent("LOCK", "auto");
    }
  }

  // Servo stall detection => treat move as complete when timeout hits
  if((mode_ == Mode::UNLOCKING || mode_ == Mode::LOCKING) && (now - moveStartMs_) > CFG.app.servo_timeout_ms){
    if(mode_ == Mode::UNLOCKING){ setMode(Mode::UNLOCKED); }
    else { setMode(Mode::LOCKED); }
  }

  // RFID check (non‑blocking)
  if(CFG.lock.rfid_enable && rfid){
    if(rfid->PICC_IsNewCardPresent() && rfid->PICC_ReadCardSerial()){
      String uid = uidToString(rfid->uid);
      String user; bool ok = isAuthorizedUID(uid, user);
      if(ok){ doUnlock(user); setStr("/state/last_uid", uid); setStr("/state/last_user", user); }
      else { pushEvent("RFID_DENY", "", uid, "", "unauthorized"); }
      rfid->PICC_HaltA();
      rfid->PCD_StopCrypto1();
    }
  }

  // Light periodic state publish
  static unsigned long lastStatePush=0;
  if(now - lastStatePush >= 2000){ lastStatePush = now; publishState(); }
}
