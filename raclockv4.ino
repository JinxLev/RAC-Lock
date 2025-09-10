/*
  RAC Smart Lock — ESP32 + RFID + Servo + I2C LCD
  Firmware FSM v0.2.2 — friendlier LCD + lean RTDB state
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

// ======= FSM =======
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

// UI flags
static bool showingWelcome_ = false;     // currently showing welcome message
static String currentUser_ = "";         // last authorized user for UI

// ======= USER FILL (do NOT commit secrets) =======
#define WIFI_SSID     "TEC"
#define WIFI_PASSWORD "12345679"
#define FIREBASE_HOST "https://rac-app-63e7f-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "b9xaDAbvYoGS05DnqaE2t97LaK9vu2gyDbLLh44f"
// ================================================

// ======= NTP =======
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 0
#define DAYLIGHT_OFFSET_SEC 0

// ======= Firebase =======
FirebaseConfig fbConfig;
FirebaseAuth   fbAuth;
FirebaseData   fbdo;
FirebaseData   fbStreamConfig;
FirebaseData   fbStreamActions;

//BUZZER
#define BUZZER_PIN 25




// ======= UID helper =======
static String uidToString(const MFRC522::Uid &u) {
  String s; s.reserve(u.size * 2);
  for (byte i = 0; i < u.size; i++) {
    if (u.uidByte[i] < 0x10) s += '0';
    s += String(u.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// ======= Config =======
struct ConfigServo { int pin=22, min_us=500, max_us=2400, unlock_angle=0, lock_angle=180; };
struct ConfigLock  { unsigned long auto_lock_ms=8000; bool rfid_enable=true, remote_enable=true; };
struct ConfigLCD   { bool enable=true; int sda=27, scl=26, address=0x27, cols=16, rows=2; };
struct ConfigApp   { unsigned long poll_ms=250; unsigned long servo_timeout_ms=2000; };
struct ConfigRoot  { ConfigServo servo; ConfigLock lock; ConfigLCD lcd; ConfigApp app; } CFG;

// ======= Hardware =======
LiquidCrystal_I2C *uiLcd = nullptr;   // renamed to avoid conflict
Servo servo;

static const int MFRC_SS_PIN  = 5;
static const int MFRC_RST_PIN = 21;
MFRC522 mfrc(MFRC_SS_PIN, MFRC_RST_PIN);

int  servoAngle_ = 90;
unsigned long lastUnlockMs_ = 0;
unsigned long moveStartMs_ = 0;
unsigned long lastActionTs_ = 0;

// ======= Firebase helpers =======
static bool ensure(const String &path, FirebaseJson &json){ return Firebase.RTDB.updateNode(&fbdo, path, &json); }
static bool setStr (const String &p, const String &v){ return Firebase.RTDB.setString(&fbdo, p, v); }
static bool setInt (const String &p, int v){ return Firebase.RTDB.setInt(&fbdo, p, v); }
static bool setBool(const String &p, bool v){ return Firebase.RTDB.setBool(&fbdo, p, v); }
static bool setJson(const String &p, FirebaseJson &j){ return Firebase.RTDB.setJSON(&fbdo, p, &j); }
static bool pushJson(const String &p, FirebaseJson &j){ return Firebase.RTDB.pushJSON(&fbdo, p, &j); }

static bool getInt(const String &p, int &out){ if(!Firebase.RTDB.getInt(&fbdo, p)) return false; out=fbdo.intData(); return true; }
static bool getUL (const String &p, unsigned long &out){ if(!Firebase.RTDB.getInt(&fbdo, p)) return false; out=(unsigned long)fbdo.intData(); return true; }
static bool getBool(const String &p, bool &out){ if(!Firebase.RTDB.getBool(&fbdo, p)) return false; out=fbdo.boolData(); return true; }

// ======= LCD helpers (friendlier UX) =======
static void lcdInit(){
  if(uiLcd){ delete uiLcd; uiLcd=nullptr; }
  if(!CFG.lcd.enable) return;
  Wire.begin(CFG.lcd.sda, CFG.lcd.scl);
  uiLcd = new LiquidCrystal_I2C(CFG.lcd.address, CFG.lcd.cols, CFG.lcd.rows);
  uiLcd->init();
  uiLcd->backlight();
}

static void lcdLine(int row, const String &msg) {
  if (!uiLcd) return;
  const int cols = CFG.lcd.cols;
  uiLcd->setCursor(0, row);
  int n = msg.length(); if (n > cols) n = cols;
  for (int i = 0; i < n; ++i) uiLcd->print(msg[i]);
  for (int i = n; i < cols; ++i) uiLcd->print(' ');
}

// Friendly screens
static void showIdle(){
  if(!uiLcd) return;
  showingWelcome_ = false;
  currentUser_ = "";
  lcdLine(0, "RAC Smart Lock");
  lcdLine(1, "Ready. Scan card.");
  digitalWrite(BUZZER_PIN, LOW);   // buzzer ON while welcome is displayed

}

static void showWelcome(const String& user){
  if(!uiLcd) return;
  showingWelcome_ = true;
  currentUser_ = user;
  lcdLine(0, "Welcome,");
  lcdLine(1, user);
  digitalWrite(BUZZER_PIN, HIGH);   // buzzer ON while welcome is displayed

}

static void showModeIfNotWelcome(){
  if(!uiLcd) return;
  if(showingWelcome_) return; // don't override welcome screen
  // For brief status updates without spamming users
  lcdLine(0, "RAC Smart Lock");
  lcdLine(1, String("Mode: ") + modeName(mode_));
}

// ======= Events & State =======
static void pushEvent(const String &type, const String &by="", const String &uid="", const String &user="", const String &reason="", int duration_ms=-1){
  FirebaseJson j;
  time_t now_seconds = time(nullptr);
  if (now_seconds < 1672531200) { // before 2023-01-01 => not synced
    Serial.println("Warning: Time not synced, using millis()");
    now_seconds = millis() / 1000;
  }
  int64_t now_millis = (int64_t)now_seconds * 1000;
  j.set("ts", now_millis);
  j.set("type", type);
  if(by.length()) j.set("by", by);
  if(uid.length()) j.set("uid", uid);
  if(user.length()) j.set("user", user);
  if(reason.length()) j.set("reason", reason);
  if(duration_ms>=0) j.set("duration_ms", duration_ms);
  pushJson("/events", j);
}

// Minimal /state payload: keep only what the web UI reasonably needs.
static void publishState(){
  FirebaseJson j;
  j.set("mode", modeName(mode_));
  j.set("servo_angle", servoAngle_);
  if(currentUser_.length()) j.set("last_user", currentUser_);
  // last_uid is written elsewhere when applicable
  j.set("version", "0.2.2");
  setJson("/state", j);
}

// ======= Servo control =======
static void servoAttach(){ if(servo.attached()) servo.detach(); servo.attach(CFG.servo.pin, CFG.servo.min_us, CFG.servo.max_us); }
static void writeServoAngle(int a){ a = constrain(a, 0, 180); servo.write(a); servoAngle_ = a; setInt("/state/servo_angle", servoAngle_); }

static void setMode(Mode m){
  mode_ = m;
  setStr("/state/mode", modeName(mode_));
  // Update LCD only if we are NOT showing the persistent welcome screen.
  showModeIfNotWelcome();
  // When we settle into LOCKED, always revert to idle screen.
  if(m == Mode::LOCKED) showIdle();
}

static void doLock(){
  setMode(Mode::LOCKING);
  moveStartMs_ = millis();
  writeServoAngle(CFG.servo.lock_angle);
}

static void doUnlock(const String &by, const String &userName = ""){
  setMode(Mode::UNLOCKING);
  moveStartMs_ = millis();
  writeServoAngle(CFG.servo.unlock_angle);
  lastUnlockMs_ = millis();
  if(userName.length()) showWelcome(userName);
  pushEvent("UNLOCK", by, "", userName);
}

// ======= Config seed/load =======
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
  if(getInt("/config/servo/pin", v)) CFG.servo.pin=v;
  if(getInt("/config/servo/min_us", v)) CFG.servo.min_us=v;
  if(getInt("/config/servo/max_us", v)) CFG.servo.max_us=v;
  if(getInt("/config/servo/unlock_angle", v)) CFG.servo.unlock_angle=v;
  if(getInt("/config/servo/lock_angle", v)) CFG.servo.lock_angle=v;

  if(getUL("/config/lock/auto_lock_ms", ul)) CFG.lock.auto_lock_ms=ul;
  if(getBool("/config/lock/rfid_enable", b)) CFG.lock.rfid_enable=b;
  if(getBool("/config/lock/remote_enable", b)) CFG.lock.remote_enable=b;

  if(getBool("/config/lcd/enable", b)) CFG.lcd.enable=b;
  if(getInt("/config/lcd/sda", v)) CFG.lcd.sda=v;
  if(getInt("/config/lcd/scl", v)) CFG.lcd.scl=v;
  if(getInt("/config/lcd/address", v)) CFG.lcd.address=v;
  if(getInt("/config/lcd/cols", v)) CFG.lcd.cols=v;
  if(getInt("/config/lcd/rows", v)) CFG.lcd.rows=v;

  if(getUL("/config/app/poll_ms", ul)) CFG.app.poll_ms=ul;
  if(getUL("/config/app/servo_timeout_ms", ul)) CFG.app.servo_timeout_ms=ul;
}

static void applyConfig(bool first){
  lcdInit();
  showIdle();                 // friendly idle right after init
  servoAttach();
  writeServoAngle(CFG.servo.lock_angle);
  setMode(Mode::LOCKED);

  if(first){
    SPI.begin();
    mfrc.PCD_Init();
  }
}

// ======= Streams =======
static void onConfigStream(FirebaseStream /*data*/){
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
  if(js->get(jd, "by"))  by  = jd.to<String>();
  if(js->get(jd, "ts"))  ts  = jd.to<int>();
  if(js->get(jd, "ms"))  ms  = jd.to<int>();

  if(ts <= 0 || (unsigned long)ts <= lastActionTs_) return;
  lastActionTs_ = ts; setInt("/state/last_action_ts", lastActionTs_);

  if(!CFG.lock.remote_enable) { pushEvent("REMOTE", by, "", "", "remote_disabled"); return; }
  if(cmd == "unlock") {
    doUnlock(by, by); // if remote user name is in 'by', show it
  } else if(cmd == "lock") {
    doLock(); pushEvent("LOCK", by);
  } else if(cmd == "pulse") {
    unsigned long dur = (ms>0)? (unsigned long)ms : 3000;
    doUnlock(by, by);
    lastUnlockMs_ = millis() - (CFG.lock.auto_lock_ms - dur);
  } else {
    pushEvent("ERROR", by, "", "", String("unknown_cmd:") + cmd);
  }
}
static void onStreamTimeout(bool which){ Serial.printf("[Stream:%s] timeout\n", which?"config":"actions"); }

// ======= RFID authorize =======
static bool isAuthorizedUID(const String &uid, String &userOut){
  String path = String("/users/") + uid;
  if(Firebase.RTDB.get(&fbdo, path)){
    if(fbdo.dataType() == "string"){ userOut = fbdo.to<String>(); return userOut.length()>0; }
    if(fbdo.dataType() == "json"){
      FirebaseJson *j = fbdo.to<FirebaseJson*>();
      FirebaseJsonData jd;
      String name; bool enabled=true;
      if(j->get(jd, "name"))    name    = jd.to<String>();
      if(j->get(jd, "enabled")) enabled = jd.to<bool>();
      if(name.length()>0 && enabled){ userOut=name; return true; }
    }
  }
  return false;
}

// ======= Setup / Loop =======
void setup(){
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); // start off

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  while(WiFi.status() != WL_CONNECTED){ delay(100); Serial.print('.'); }
  Serial.println(); Serial.println(WiFi.localIP());

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  fbConfig.database_url = FIREBASE_HOST;
  fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);

  ArduinoOTA.setHostname("rac-smart-lock");
  ArduinoOTA.begin();

  seedDefaults();
  loadConfigFromRTDB();
  applyConfig(true);

  publishState();
  pushEvent("BOOT");

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

  showIdle(); // initial friendly screen
}

void loop(){
  ArduinoOTA.handle();

  unsigned long now = millis();

  if(mode_ == Mode::UNLOCKED){
    if(CFG.lock.auto_lock_ms > 0 && (now - lastUnlockMs_) >= CFG.lock.auto_lock_ms){
      doLock(); pushEvent("LOCK", "auto");
      // When we transition back to LOCKED, drop welcome and return to idle.
      showIdle();
    }
  }

  if((mode_ == Mode::UNLOCKING || mode_ == Mode::LOCKING) && (now - moveStartMs_) > CFG.app.servo_timeout_ms){
    if(mode_ == Mode::UNLOCKING){ 
      setMode(Mode::UNLOCKED); 
      // Keep welcome screen during UNLOCKED
    } else { 
      setMode(Mode::LOCKED); 
      showIdle(); // back to idle once locked
    }
  }

  if(CFG.lock.rfid_enable){
    if(mfrc.PICC_IsNewCardPresent() && mfrc.PICC_ReadCardSerial()){
      String uid = uidToString(mfrc.uid);
      String user; bool ok = isAuthorizedUID(uid, user);
      if(ok){
        doUnlock(user, user);        // show name on LCD
        setStr("/state/last_uid", uid);
        setStr("/state/last_user", user);
      } else {
        // brief clear feedback without being rude
        lcdLine(0, "Access denied");
        lcdLine(1, "Unauthorized UID");
        showingWelcome_ = false; // not welcome state
        pushEvent("RFID_DENY", "", uid, "", "unauthorized");
        // after a short moment, return to idle
        delay(1200);
        showIdle();
      }
      mfrc.PICC_HaltA();
      mfrc.PCD_StopCrypto1();
    }
  }

  static unsigned long lastStatePush=0;
  if(now - lastStatePush >= 2000){ lastStatePush = now; publishState(); }
}
