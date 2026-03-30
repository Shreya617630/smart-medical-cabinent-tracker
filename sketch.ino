#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <RTClib.h>
#include <DHT.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

#define RFID_SS    10
#define RFID_RST    9
#define DHT_PIN     2
#define DHT_TYPE DHT22
#define GREEN_LED  A2
#define YELLOW_LED A3
#define RED_LED    A4
#define BUZZER     A5

const byte ROWS = 4, COLS = 4;
char keys[4][4] = {{'1','2','3','A'},{'4','5','6','B'},{'7','8','9','C'},{'*','0','#','D'}};
byte rowPins[4] = {3,4,5,6};
byte colPins[4] = {7,8,A0,A1};
Keypad keypad = Keypad(makeKeymap(keys),rowPins,colPins,ROWS,COLS);

MFRC522 rfid(RFID_SS,RFID_RST);
RTC_DS1307 rtc;
DHT dht(DHT_PIN,DHT_TYPE);
LiquidCrystal_I2C lcd(0x27,16,2);

#define EEPROM_COUNT_ADDR    0
#define EEPROM_BADHOURS_ADDR 2
#define EEPROM_MISSED_ADDR   4
#define EEPROM_MED_START   100
#define MAX_MED 6

struct Med {
  char name[16]; uint32_t tag; uint16_t yr; uint8_t mo,dy;
  uint8_t doseHour,doseMin;
  bool doseTaken,doseAlerted,present,inCabinet,storageWarn;
};
Med meds[MAX_MED];
int medCount=0;

bool expired=false,expiring=false,badTemp=false;
float lastTemp=0,lastHumidity=0;
uint16_t badHours=0,missedDoses=0;
unsigned long badTempStart=0,lastScan=0,lastLCD=0,lastTempMs=0;
unsigned long lastBuzz=0,lastSerial=0,lastDoseCheck=0;
bool badTempActive=false;
int scrollIdx=0;
uint8_t lastDoseCheckDay=255;

enum Mode{MONITOR,REGISTER};
Mode mode=MONITOR;
String regName="",regDate="",regDoseTime="";
int regStep=0;
uint32_t pendingTag=0;

void saveMeds(){
  EEPROM.put(EEPROM_COUNT_ADDR,medCount);
  EEPROM.put(EEPROM_BADHOURS_ADDR,badHours);
  EEPROM.put(EEPROM_MISSED_ADDR,missedDoses);
  for(int i=0;i<medCount;i++) EEPROM.put(EEPROM_MED_START+i*sizeof(Med),meds[i]);
}

void loadMeds(){
  EEPROM.get(EEPROM_COUNT_ADDR,medCount);
  EEPROM.get(EEPROM_BADHOURS_ADDR,badHours);
  EEPROM.get(EEPROM_MISSED_ADDR,missedDoses);
  if(medCount<0||medCount>MAX_MED){medCount=0;badHours=0;missedDoses=0;}
  for(int i=0;i<medCount;i++) EEPROM.get(EEPROM_MED_START+i*sizeof(Med),meds[i]);
}

uint32_t getTag(){uint32_t id=0;for(byte i=0;i<4;i++)id=(id<<8)|rfid.uid.uidByte[i];return id;}
int findMed(uint32_t tag){for(int i=0;i<medCount;i++)if(meds[i].tag==tag)return i;return -1;}

int daysLeft(int i){
  DateTime now=rtc.now();
  DateTime exp(meds[i].yr,meds[i].mo,meds[i].dy,23,59,59);
  return(int)(((long)exp.unixtime()-(long)now.unixtime())/86400L);
}

void checkExpiry(){
  expired=false;expiring=false;
  for(int i=0;i<medCount;i++){int d=daysLeft(i);if(d<0)expired=true;else if(d<=30)expiring=true;}
}

void ledsOff(){digitalWrite(GREEN_LED,LOW);digitalWrite(YELLOW_LED,LOW);digitalWrite(RED_LED,LOW);}

void updateLEDs(){
  ledsOff();
  if(expired){static unsigned long lf=0;if(millis()-lf>300){lf=millis();digitalWrite(RED_LED,!digitalRead(RED_LED));}}
  else if(expiring||badTemp) digitalWrite(YELLOW_LED,HIGH);
  else digitalWrite(GREEN_LED,HIGH);
}

void checkTemp(){
  float t=dht.readTemperature(),h=dht.readHumidity();
  if(isnan(t)||isnan(h))return;
  lastTemp=t; lastHumidity=h;
  bool nowBad=(t>30.0||h>70.0);
  if(nowBad&&!badTempActive){badTempActive=true;badTempStart=millis();}
  else if(!nowBad&&badTempActive){
    badTempActive=false;
    badHours+=(uint16_t)((millis()-badTempStart)/3600000UL);
    saveMeds();
  }
  badTemp=nowBad;
  if(badTemp){
    for(int i=0;i<medCount;i++) meds[i].storageWarn=true;
    lcd.clear();lcd.setCursor(0,0);lcd.print("!!STORAGE ALERT!");
    lcd.setCursor(0,1);lcd.print("T:");lcd.print((int)t);lcd.print("C H:");lcd.print((int)h);lcd.print("%");
    tone(BUZZER,1000,500);delay(2000);
    Serial.print(F("{\"event\":\"ENV_ALERT\",\"temp\":"));Serial.print(t,1);
    Serial.print(F(",\"humidity\":"));Serial.print(h,1);
    Serial.print(F(",\"badHours\":"));Serial.print(badHours);Serial.println(F("}"));
  }
}

void checkDoseSchedule(){
  DateTime now=rtc.now();
  if(now.day()!=lastDoseCheckDay){
    lastDoseCheckDay=now.day();
    for(int i=0;i<medCount;i++){meds[i].doseTaken=false;meds[i].doseAlerted=false;}
    saveMeds();
  }
  uint8_t h=now.hour(),m=now.minute();
  for(int i=0;i<medCount;i++){
    if(meds[i].doseHour==255)continue;
    bool inWindow=(h==meds[i].doseHour);
    bool pastWindow=(h==meds[i].doseHour+1);
    if(inWindow&&meds[i].present&&!meds[i].doseTaken){
      meds[i].doseTaken=true;
      lcd.clear();lcd.setCursor(0,0);lcd.print("Dose taken:");
      lcd.setCursor(0,1);lcd.print(meds[i].name);
      tone(BUZZER,1600,100);delay(150);tone(BUZZER,2000,200);delay(2000);
      Serial.print(F("{\"event\":\"DOSE_TAKEN\",\"medicine\":\""));Serial.print(meds[i].name);
      Serial.print(F("\",\"time\":\""));Serial.print(h);Serial.print(':');
      if(m<10)Serial.print('0');Serial.print(m);Serial.println(F("\"}"));
      saveMeds();
    }
    if(pastWindow&&!meds[i].doseTaken&&!meds[i].doseAlerted){
      meds[i].doseAlerted=true;missedDoses++;
      lcd.clear();lcd.setCursor(0,0);lcd.print("MISSED DOSE!!");
      lcd.setCursor(0,1);lcd.print(meds[i].name);
      for(int b=0;b<3;b++){tone(BUZZER,900,300);delay(500);}
      delay(2000);
      Serial.print(F("{\"event\":\"MISSED_DOSE\",\"medicine\":\""));Serial.print(meds[i].name);
      Serial.print(F("\",\"scheduledHour\":"));Serial.print(meds[i].doseHour);
      Serial.print(F(",\"totalMissed\":"));Serial.print(missedDoses);Serial.println(F("}"));
      saveMeds();
    }
  }
}

void scanRFID(){
  for(int i=0;i<medCount;i++) meds[i].present=false;
  if(!rfid.PICC_IsNewCardPresent())return;
  if(!rfid.PICC_ReadCardSerial())return;
  uint32_t tag=getTag();
  int idx=findMed(tag);
  if(mode==REGISTER&&regStep==0){
    pendingTag=tag;regStep=1;
    lcd.clear();lcd.setCursor(0,0);lcd.print("Tag scanned!");
    lcd.setCursor(0,1);lcd.print("Enter name:");
  } else if(idx>=0){
    meds[idx].present=true;meds[idx].inCabinet=true;
    int d=daysLeft(idx);
    lcd.clear();lcd.setCursor(0,0);lcd.print(meds[idx].name);
    lcd.setCursor(0,1);
    if(d<0){lcd.print("EXPIRED! ");lcd.print(abs(d));lcd.print("d");}
    else{lcd.print("Days left: ");lcd.print(d);}
    delay(1500);
  } else {
    lcd.clear();lcd.setCursor(0,0);lcd.print("Unknown tag!");
    lcd.setCursor(0,1);lcd.print("Press A to reg");
    tone(BUZZER,800,200);
    Serial.print(F("{\"event\":\"UNKNOWN_TAG\",\"uid\":"));Serial.print(tag);Serial.println(F("}"));
  }
  rfid.PICC_HaltA();rfid.PCD_StopCrypto1();
}

void updateLCD(){
  if(millis()-lastLCD<3000)return;
  lastLCD=millis();
  if(medCount==0){lcd.clear();lcd.setCursor(0,0);lcd.print("No medicines");lcd.setCursor(0,1);lcd.print("Press A to add");return;}
  int i=scrollIdx%medCount,d=daysLeft(i);
  scrollIdx++;
  lcd.clear();lcd.setCursor(0,0);
  char n[13];strncpy(n,meds[i].name,12);n[12]='\0';lcd.print(n);
  if(meds[i].storageWarn) lcd.print(" ST!");
  else lcd.print(d<0?" !!":(d<=30?" ! ":" OK"));
  lcd.setCursor(0,1);
  if(d<0){lcd.print("EXPIRED ");lcd.print(abs(d));lcd.print("d ago");
    if(millis()-lastBuzz>5000){lastBuzz=millis();tone(BUZZER,1500,300);}}
  else{
    lcd.print("EXP:");lcd.print(d);lcd.print("d");
    if(meds[i].doseHour!=255) lcd.print(meds[i].doseTaken?" DK":" D?");
  }
}

void sendSerialHeartbeat(){
  if(millis()-lastSerial<10000)return;
  lastSerial=millis();
  DateTime now=rtc.now();
  Serial.print(F("{\"event\":\"HEARTBEAT\",\"time\":\""));
  Serial.print(now.hour());Serial.print(':');
  if(now.minute()<10)Serial.print('0');Serial.print(now.minute());
  Serial.print(F("\",\"temp\":"));Serial.print(lastTemp,1);
  Serial.print(F(",\"humidity\":"));Serial.print(lastHumidity,1);
  Serial.print(F(",\"badHours\":"));Serial.print(badHours);
  Serial.print(F(",\"missedDoses\":"));Serial.print(missedDoses);
  Serial.print(F(",\"medicines\":["));
  for(int i=0;i<medCount;i++){
    if(i)Serial.print(',');
    Serial.print(F("{\"name\":\""));Serial.print(meds[i].name);
    Serial.print(F("\",\"daysLeft\":"));Serial.print(daysLeft(i));
    Serial.print(F(",\"doseTaken\":"));Serial.print(meds[i].doseTaken?"true":"false");
    Serial.print(F(",\"inCabinet\":"));Serial.print(meds[i].inCabinet?"true":"false");
    Serial.print(F(",\"storageWarn\":"));Serial.print(meds[i].storageWarn?"true":"false");
    Serial.print('}');
  }
  Serial.println(F("]}"));
}

void handleKey(char key){
  if(regStep==1){
    if(key=='#'&&regName.length()>0){regStep=2;regDate="";}
    else if(key=='*'&&regName.length()>0)regName=regName.substring(0,regName.length()-1);
    else if(key>='0'&&key<='9'&&regName.length()<15)regName+=key;
    else{char L[]={'A','B','C','D'};for(int i=0;i<4;i++)if(key==L[i]&&regName.length()<15){regName+=L[i];break;}}
    lcd.setCursor(0,1);lcd.print("                ");lcd.setCursor(0,1);lcd.print(regName.substring(0,16));
  } else if(regStep==2){
    if(key=='#'&&regDate.length()==8){
      regStep=3;regDoseTime="";
      lcd.clear();lcd.setCursor(0,0);lcd.print("Dose time HHMM");lcd.setCursor(0,1);lcd.print("(# to skip)");
    } else if(key=='*'&&regDate.length()>0)regDate=regDate.substring(0,regDate.length()-1);
    else if(key>='0'&&key<='9'&&regDate.length()<8)regDate+=key;
    lcd.setCursor(0,1);lcd.print("                ");lcd.setCursor(0,1);lcd.print(regDate);
  } else if(regStep==3){
    if(key=='#'){
      if(medCount<MAX_MED){
        Med m;
        strncpy(m.name,regName.c_str(),15);m.name[15]='\0';
        m.tag=pendingTag;
        m.dy=regDate.substring(0,2).toInt();m.mo=regDate.substring(2,4).toInt();m.yr=regDate.substring(4,8).toInt();
        if(regDoseTime.length()==4){m.doseHour=regDoseTime.substring(0,2).toInt();m.doseMin=regDoseTime.substring(2,4).toInt();}
        else{m.doseHour=255;m.doseMin=0;}
        m.doseTaken=false;m.doseAlerted=false;m.present=false;m.inCabinet=true;m.storageWarn=false;
        meds[medCount++]=m;saveMeds();
        lcd.clear();lcd.setCursor(0,0);lcd.print("SAVED!");lcd.setCursor(0,1);lcd.print(m.name);
        Serial.print(F("{\"event\":\"MED_REGISTERED\",\"name\":\""));Serial.print(m.name);
        Serial.print(F("\",\"doseHour\":"));Serial.print(m.doseHour==255?-1:m.doseHour);Serial.println(F("}"));
        tone(BUZZER,1200,200);delay(300);tone(BUZZER,1600,300);delay(2000);
      }
      mode=MONITOR;regStep=0;regName="";regDate="";regDoseTime="";lcd.clear();
    } else if(key=='*'&&regDoseTime.length()>0)regDoseTime=regDoseTime.substring(0,regDoseTime.length()-1);
    else if(key>='0'&&key<='9'&&regDoseTime.length()<4)regDoseTime+=key;
    lcd.setCursor(0,1);lcd.print("                ");lcd.setCursor(0,1);lcd.print(regDoseTime);
  }
}

void setup(){
  Serial.begin(115200);
  SPI.begin();Wire.begin();
  rfid.PCD_Init();rtc.begin();dht.begin();
  lcd.init();lcd.backlight();
  pinMode(GREEN_LED,OUTPUT);pinMode(YELLOW_LED,OUTPUT);pinMode(RED_LED,OUTPUT);pinMode(BUZZER,OUTPUT);
  loadMeds();
  lcd.setCursor(0,0);lcd.print("MediTrack v2.0");
  lcd.setCursor(0,1);lcd.print("NIT Hamirpur");
  tone(BUZZER,1000,150);delay(200);tone(BUZZER,1400,150);delay(200);tone(BUZZER,1800,300);
  delay(2000);lcd.clear();
  Serial.print(F("{\"event\":\"STARTUP\",\"medicines\":"));Serial.print(medCount);
  Serial.print(F(",\"badHours\":"));Serial.print(badHours);
  Serial.print(F(",\"missedDoses\":"));Serial.print(missedDoses);Serial.println(F("}"));
  checkTemp();checkExpiry();updateLEDs();
  lastDoseCheckDay=rtc.now().day();
}

void loop(){
  char key=keypad.getKey();
  if(key){
    if(key=='A'&&mode==MONITOR){mode=REGISTER;regStep=0;regName="";regDate="";regDoseTime="";lcd.clear();lcd.setCursor(0,0);lcd.print("REGISTER MODE");lcd.setCursor(0,1);lcd.print("Scan tag first");}
    else if(key=='B'&&mode==REGISTER){mode=MONITOR;regStep=0;regName="";regDate="";regDoseTime="";lcd.clear();}
    else if(key=='C'&&mode==MONITOR){checkExpiry();lcd.clear();lcd.setCursor(0,0);lcd.print("Status check:");lcd.setCursor(0,1);lcd.print(expired?"EXPIRED!":expiring?"EXPIRING":"ALL OK");delay(2000);lcd.clear();}
    else if(key=='D'&&mode==MONITOR){lcd.clear();lcd.setCursor(0,0);lcd.print("Missed:");lcd.print(missedDoses);lcd.setCursor(0,1);lcd.print("BadHrs:");lcd.print(badHours);delay(3000);lcd.clear();}
    else if(mode==REGISTER)handleKey(key);
  }
  if(millis()-lastScan>2000){lastScan=millis();scanRFID();}
  static unsigned long lastChk=0;
  if(millis()-lastChk>60000){lastChk=millis();checkExpiry();}
  if(millis()-lastTempMs>30000){lastTempMs=millis();checkTemp();}
  if(millis()-lastDoseCheck>60000){lastDoseCheck=millis();checkDoseSchedule();}
  sendSerialHeartbeat();
  updateLEDs();
  if(mode==MONITOR)updateLCD();
  else if(millis()-lastLCD>500){
    lastLCD=millis();lcd.clear();
    if(regStep==0){lcd.setCursor(0,0);lcd.print("REGISTER MODE");lcd.setCursor(0,1);lcd.print("Scan tag first");}
    else if(regStep==1){lcd.setCursor(0,0);lcd.print("Medicine name:");lcd.setCursor(0,1);lcd.print(regName.substring(0,16));}
    else if(regStep==2){lcd.setCursor(0,0);lcd.print("Expiry DDMMYYYY");lcd.setCursor(0,1);lcd.print(regDate);}
    else if(regStep==3){lcd.setCursor(0,0);lcd.print("Dose time HHMM");lcd.setCursor(0,1);lcd.print(regDoseTime);}
  }
  delay(100);
}