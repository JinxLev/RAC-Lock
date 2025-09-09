#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <ESP32Servo.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>  // LCD library for PCF8574

// --- CONFIG ---
#define WIFI_SSID     "abc"
#define WIFI_PASSWORD "linuxabc"
#define FIREBASE_HOST "rac-app-63e7f-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "b9xaDAbvYoGS05DnqaE2t97LaK9vu2gyDbLLh44f" // legacy DB secret

// --- RFID ---
#define SS_PIN  5
#define RST_PIN 21
MFRC522 rfid(SS_PIN, RST_PIN);

// --- LCD via PCF8574 ---
#define LCD_SDA 27  // choose your SDA pin
#define LCD_SCL 26  // choose your SCL pin
LiquidCrystal_I2C lcd(0x27, 16, 2);  // Adjust I2C address if needed

// --- Servo ---
Servo myservo;
const int SERVO_PIN = 22;
int servoAngle = 90; // initial servo angle

// --- Firebase ---
FirebaseConfig config;
FirebaseAuth auth;
FirebaseData fbdo;

// --- UID to string (Firebase-ready) ---
String uidToString(MFRC522::Uid* uid) {
  String result = "";
  for (byte i = 0; i < uid->size; i++) {
    if (uid->uidByte[i] < 0x10) result += "0";   // pad single digits
    result += String(uid->uidByte[i], HEX);
  }
  result.toUpperCase();  // modifies result in place
  return result;         // e.g., "04AB1234"
}


// --- Update servo from Firebase ---
void updateServoFromFirebase() {
  if (Firebase.RTDB.get(&fbdo, "/servo")) {
    String dtype = fbdo.dataType();
    int angle = servoAngle; // fallback to current angle

    if (dtype == "string") {
      String s = fbdo.stringData();
      s.trim();
      if (s.length()) angle = s.toInt();
      else Serial.println("Empty string at /servo");
    } 
    else if (dtype == "int") {
      angle = fbdo.intData();
    } 
    else {
      String p = fbdo.payload();
      p.trim();
      if (p.length()) angle = p.toInt();
      Serial.print("Fallback parse type: "); Serial.println(dtype);
    }

    angle = constrain(angle, 0, 180);
    if (angle != servoAngle) {
      Serial.print("Servo -> "); Serial.println(angle);
      myservo.write(angle);
      servoAngle = angle;
    }
  } else {
    Serial.print("Firebase GET failed. HTTP: ");
    Serial.print(fbdo.httpCode());
    Serial.print("  reason: ");
    Serial.println(fbdo.errorReason());
  }
}

void checkRFID() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  String uidStr = uidToString(&rfid.uid); // convert UID to string
  uidStr.replace(" ", "");                // remove spaces for Firebase key
  uidStr.toUpperCase();

  // Toggle servo
  servoAngle = (servoAngle == 0) ? 180 : 0;
  myservo.write(servoAngle);

  // Update servo value in Firebase
  if (Firebase.RTDB.setString(&fbdo, "/servo", String(servoAngle))) {
    Serial.println("Servo value updated in Firebase!");
  } else {
    Serial.print("Firebase servo error: ");
    Serial.println(fbdo.errorReason());
  }

  // Lookup user name in Firebase
  String path = "/users/" + uidStr;
  if (Firebase.RTDB.getString(&fbdo, path)) {
    String userName = fbdo.stringData();
    Serial.print("User matched: "); Serial.println(userName);

    // Display UID and user on LCD
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("UID: " + uidStr);
    lcd.setCursor(0, 1);
    lcd.print("User: " + userName);
  } else {
    Serial.print("No user found for UID: "); Serial.println(uidStr);

    // Display UID and unknown user
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("UID: " + uidStr);
    lcd.setCursor(0, 1);
    lcd.print("Unknown User");
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void setup() {
  Serial.begin(115200);
  SPI.begin();
  rfid.PCD_Init();

  myservo.attach(SERVO_PIN);
  myservo.write(servoAngle);

  // LCD I2C
  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Initializing...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(200); Serial.print('.'); }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  config.database_url = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Setup complete, scan RFID card or unlock from app.");
}

void loop() {
  checkRFID();
  updateServoFromFirebase();
  delay(250); // adjust polling rate as needed
}