#include <Base64.h>

#include <SoftwareSerial.h>

#include <Adafruit_FONA.h>

#include <AES.h>

/*****************************************
 accurite 5n1 weather station decoder
  
  for arduino and 433 MHz OOK RX module
  Note: use superhet (with xtal) rx board
  the regen rx boards are too noisy

 Jens Jensen, (c)2015
*****************************************/
#import <avr/eeprom.h>
String ID = String("13968815618");
AES aes;
byte key[] = 
{
  0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
} ;

byte my_iv[] = 
{
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

byte plain[] =
{
  // 0xf3, 0x44, 0x81, 0xec, 0x3c, 0xc6, 0x27, 0xba, 0xcd, 0x5d, 0xc3, 0xfb, 0x08, 0xf2, 0x73, 0xe6
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
} ;

byte cipher [4*N_BLOCK];
byte iv [N_BLOCK] ;
byte succ;

#define FONA_RX 2
#define FONA_TX 3
#define FONA_RST 4

SoftwareSerial fonaSS = SoftwareSerial(FONA_TX, FONA_RX);
SoftwareSerial *fonaSerial = &fonaSS;

// Hardware serial is also possible!
//  HardwareSerial *fonaSerial = &Serial1;

// Use this for FONA 800 and 808s
Adafruit_FONA fona = Adafruit_FONA(FONA_RST);

// pulse timings
// SYNC
#define SYNC_HI      675
#define SYNC_LO      575

// HIGH == 1
#define LONG_HI      450
#define LONG_LO      375

// SHORT == 0
#define SHORT_HI     250
#define SHORT_LO     175

#define RESETTIME    1000000

// other settables
#define LED          13
#define PIN           2  // data pin from 433 RX module
#define MAXBITS      65  // max framesize

//#define DEBUG         1  // uncomment to enable debugging
#define DEBUGPIN     A0  // pin for triggering logic analyzer
#define METRIC_UNITS  0  // select display of metric or imperial units

// sync states
#define RESET     0   // no sync yet
#define INSYNC    1   // sync pulses detected 
#define SYNCDONE  2   // complete sync header received 

volatile unsigned int    pulsecnt = 0; 
volatile unsigned long   risets = 0;     // track rising edge time
volatile unsigned int    syncpulses = 0; // track sync pulses
volatile byte            state = RESET;  
volatile byte            buf[8] = {0,0,0,0,0,0,0,0};  // msg frame buffer
volatile bool            reading = false;            // have valid reading

unsigned int   raincounter = 0;
unsigned int   EEMEM raincounter_persist;    // persist raincounter in eeprom
#define  MARKER  0x5AA5
unsigned int   EEMEM eeprom_marker = MARKER; // indicate if we have written to eeprom or not before

// wind directions:
// { "NW", "WSW", "WNW", "W", "NNW", "SW", "N", "SSW",
//   "ENE", "SE", "E", "ESE", "NE", "SSE", "NNE", "S" };
const float winddirections[] = { 315.0, 247.5, 292.5, 270.0, 
                                 337.5, 225.0, 0.0, 202.5,
                                 67.5, 135.0, 90.0, 112.5,
                                 45.0, 157.5, 22.5, 180.0 };

// wx message types
#define  MT_WS_WD_RF  49    // wind speed, wind direction, rainfall
#define  MT_WS_T_RH   56    // wind speed, temp, RH

#define NO_RECEIVER 0
#define RECEIVER_WS_T_RH 1
#define RECEIVER_WS_WD_RF 2
#define RECEIVER_DONE 3

byte status_code = NO_RECEIVER;
String T_RH;
String WD_RF;
String WS;
void setup() {
  // put your setup code here, to run once:
  succ = aes.set_key (key, 256);
  fona.setGPRSNetworkSettings(F("rogers-core-appl1.apn"));
  status_code = NO_RECEIVER;
  Serial.begin(9600); 
  Serial.println(F("Starting Acurite5n1 433 WX Decoder v0.2 ..."));
  pinMode(PIN, INPUT);
  raincounter = getRaincounterEEPROM();
  #ifdef DEBUG
    // setup a pin for triggering logic analyzer for debugging pulse train
    pinMode(DEBUGPIN, OUTPUT);
    digitalWrite(DEBUGPIN, HIGH);
  #endif
  attachInterrupt(0, My_ISR, CHANGE);

}

void loop() {
    if (reading) {
        // reading found
        noInterrupts();
        if (acurite_crc(buf, sizeof(buf))) {
            // passes crc, good message
            digitalWrite(LED, HIGH);
            #ifdef DEBUG
            int i;
            for (i = 0; i < 8; i++) {
                Serial.print(buf[i], HEX);
                Serial.print(" ");
            }
            Serial.println(F("CRC OK"));
            #endif

            float windspeedkph = getWindSpeed(buf[3], buf[4]);
            String ws = String((int) (windspeedkph));
            WS = String("\"ws\":") + ws;
            int msgtype = (buf[2] & 0x3F);
            if (msgtype == MT_WS_WD_RF) {
                // wind speed, wind direction, rainfall
                float rainfall = 0.00;
                unsigned int curraincounter = getRainfallCounter(buf[5], buf[6]);
                updateRaincounterEEPROM(curraincounter);
                if (raincounter > 0) {
                    // track rainfall difference after first run
                    rainfall = (curraincounter - raincounter) * 0.01;
                } else {
                    // capture starting counter
                    raincounter = curraincounter;
                }
                String winddir = String((int) (getWindDirection(buf[4])));
                String rf = String((int) convInMm(rainfall));
                
                WD_RF =  String("\"wb\":") + winddir + ",\"rf\":" + rf + ",";
                if (status_code == NO_RECEIVER) {
                   status_code = RECEIVER_WS_WD_RF;
                }
                if (status_code == RECEIVER_WS_T_RH) {
                   status_code = RECEIVER_DONE;
                }
            } else if (msgtype == MT_WS_T_RH) {
                // wind speed, temp, RH  
                String tempf = String((int) (convFC(getTempF(buf[4], buf[5]))*10));
                String humidity = String(getHumidity(buf[6]));
                String batteryok = String(((buf[2] & 0x40) >> 6));
                
                T_RH =  String("\"at\":") + tempf + ",\"ah\":" + humidity + ",\"ba\":" + batteryok + ",";
                if (status_code == NO_RECEIVER) {
                   status_code = RECEIVER_WS_T_RH;
                }
                if (status_code == RECEIVER_WS_WD_RF) {
                   status_code = RECEIVER_DONE;
                }
            } else {
                Serial.print("unknown msgtype: ");
                for (int i = 0; i < 8; i++) {
                    Serial.print(buf[i], HEX);
                    Serial.print(" ");
                }
            }
            // time
            unsigned int timesincestart = millis() / 60 / 1000;
            Serial.print(", mins since start: ");
            Serial.print(timesincestart);
            Serial.println();

        } else {
            // failed CRC
            #ifdef DEBUG
              Serial.println(F("CRC BAD"));
            #endif
        }
        if (status_code == RECEIVER_DONE) {
          String result = String("{\"id\":") + ID + ",\"de\":\"5n1\"," + T_RH + WD_RF + WS + "}";   //"id":"18811799743","ti":"2016010407220400","de":"5n1",
          
          int blocks = 2;
          for (byte i = 0 ; i < 16 ; i++)
             iv[i] = my_iv[i];
          succ = aes.cbc_encrypt (plain, cipher, blocks, iv) ;
          int inputLen = sizeof(cipher);
          int encodedLen = base64_enc_len(inputLen);
          char encoded[encodedLen];
//                base64_encode(encoded, cipher, inputLen);
          fona.enableGPRS(true);
          uint16_t statuscode;
          int16_t length;
          char url[80];
          fona.HTTP_GET_start(url, &statuscode, (uint16_t *)&length);
          fona.HTTP_GET_end();
          fona.enableGPRS(false);
        }        
        digitalWrite(LED, LOW);
        reading = false;
        interrupts();
    }

    delay(100);
}
bool acurite_crc(volatile byte row[], int cols) {
      // sum of first n-1 bytes modulo 256 should equal nth byte
      cols -= 1; // last byte is CRC
        int sum = 0;
      for (int i = 0; i < cols; i++) {
        sum += row[i];
      }    
      if (sum != 0 && sum % 256 == row[cols]) {
        return true;
      } else {
        return false;
      }
}

float getTempF(byte hibyte, byte lobyte) {
  // range -40 to 158 F
  int highbits = (hibyte & 0x0F) << 7;
  int lowbits = lobyte & 0x7F;
  int rawtemp = highbits | lowbits;
  float temp = (rawtemp - 400) / 10.0;
  return temp;
}

float getWindSpeed(byte hibyte, byte lobyte) {
  // range: 0 to 159 kph
  int highbits = (hibyte & 0x7F) << 3;
  int lowbits = (lobyte & 0x7F) >> 4;
  float speed = highbits | lowbits;
  // speed in m/s formula according to empirical data
  if (speed > 0) {
    speed = speed * 0.23 + 0.28;
  }
  float kph = speed * 60 * 60 / 1000;
  return kph;
}

float getWindDirection(byte b) {
  // 16 compass points, ccw from (NNW) to 15 (N), 
        // { "NW", "WSW", "WNW", "W", "NNW", "SW", "N", "SSW",
        //   "ENE", "SE", "E", "ESE", "NE", "SSE", "NNE", "S" };
  int direction = b & 0x0F;
  return winddirections[direction];
}

int getHumidity(byte b) {
  // range: 1 to 99 %RH
  int humidity = b & 0x7F;
  return humidity;
}

int getRainfallCounter(byte hibyte, byte lobyte) {
  // range: 0 to 99.99 in, 0.01 increment rolling counter
  int raincounter = ((hibyte & 0x7f) << 7) | (lobyte & 0x7F);
  return raincounter;
}

float convKphMph(float kph) {
  return kph * 0.62137;
}

float convFC(float f) {
  return (f-32) / 1.8;
}

float convInMm(float in) {
  return in * 25.4;
}

unsigned int getRaincounterEEPROM() {
  unsigned int oldraincounter = 0;
  unsigned int marker = eeprom_read_word(&eeprom_marker);
  #ifdef DEBUG 
    Serial.print("marker: ");
    Serial.print(marker, HEX);
  #endif
  if (marker == MARKER) {
    // we have written before, use old value
    oldraincounter = eeprom_read_word(&raincounter_persist);
    #ifdef DEBUG
      Serial.print(", raincounter_persist raw value: ");
      Serial.println(raincounter, HEX);
    #endif 
  } 
  return oldraincounter;
}

void updateRaincounterEEPROM(unsigned int raincounter) {
  eeprom_update_word(&raincounter_persist, raincounter);
  eeprom_update_word(&eeprom_marker, MARKER); // indicate first write
  #ifdef DEBUG
    Serial.print("updateraincountereeprom: ");
    Serial.print(eeprom_read_word(&raincounter_persist), HEX);
    Serial.print(", eeprommarker: ");
    Serial.print(eeprom_read_word(&eeprom_marker), HEX);
    Serial.println();
  #endif
}

void My_ISR()
{
  // decode the pulses
  unsigned long timestamp = micros();
  if (digitalRead(PIN) == HIGH) {
    // going high, start timing
    if (timestamp - risets > RESETTIME) {
      // detect reset condition
      Serial.println("RESET");
      state=RESET;
      syncpulses=0;
      pulsecnt=0;
    }
    risets = timestamp;
    return;
  }
  
  // going low
  unsigned long duration = timestamp - risets;
  
  if (state == RESET || state == INSYNC) {
    // looking for sync pulses
    //Serial.println("state == RESET || state == INSYNC");
    if ((SYNC_LO) < duration && duration < (SYNC_HI))  {
      // start counting sync pulses
      state=INSYNC;
      syncpulses++;
      if (syncpulses > 3) {
        // found complete sync header
        state = SYNCDONE;
        syncpulses = 0;
        pulsecnt=0;
        
        #ifdef DEBUG
          // quick debug to trigger logic analyzer at sync
          digitalWrite(DEBUGPIN, LOW);
        #endif
      }
      return; 
      
    } else {
      //Serial.println("state = RESET");
      // not interested, reset  
      syncpulses=0;
      pulsecnt=0;
      state=RESET;
      #ifdef DEBUG
        digitalWrite(DEBUGPIN, HIGH); //return trigger
      #endif
      return; 
    }
  } else {
    // SYNCDONE, now look for message 
    // detect if finished here
    Serial.println(pulsecnt);
    if ( pulsecnt > MAXBITS ) {
      state = RESET;
      pulsecnt = 0;
      reading = true;
      return;
    }
    // stuff buffer with message
    byte bytepos = pulsecnt / 8;
    byte bitpos = 7 - (pulsecnt % 8); // reverse bitorder
    if ( LONG_LO < duration && duration < LONG_HI) {
      bitSet(buf[bytepos], bitpos);
      pulsecnt++;
    }
    else if ( SHORT_LO < duration && duration < SHORT_HI) {
      bitClear(buf[bytepos], bitpos);
      pulsecnt++;
    }
  
  }
}
    
