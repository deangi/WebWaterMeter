//-------------------------------------------------------------------
// ESP32 based water meter server
//
// Dean Gienger, Oct 2022
//
// This will monitor a water meter output and keep track of water usage.
// This also runs a tiny web server that responds to requests to read the water usage information.
//
// Technical details:
// 1) The physical Water meter: the water meter has two wires coming out which are connected to a tiny magnetically
//    operated switch.  As water flows through the meter it spins a tiny turbine which has a tiny magnet
//    attached so that when the magenet sweeps past the magnetically
//    operating switch, causing it to momentarily close and open.   This happens such that the
//    switch closes/opens once very 10 gallons of water that flows through the meter.
// 2) The ESP32 monitors this switch and counts pulses of electrical signal caused by the switch opening and closing.
//    Each pulse represents a certain amount of water running through the water meter - for this meter each pulse
//    is 10 gallons.  By counting the pulses and multiplying by 10 we can find the amount of water in gallons
//    that has flowed through the water meter.
// 3) The ESP32 operates a web server on it's WIFI port and responds with data that shows the amount of water in gallons
//    on the meter.
// 4) The ESP32 will also keep a "log" by reading the water use every hour or so and logging the reading to a log file
//    kept in ESP32 flash.
// 5) The log can be read by the web server as well.
//
// 6) The ESP32 could loose power and have to reboot when power is restored.   It is recommended that the ESP32 be
//    connected to a battery to minimize this.  When the ESP32 does have to reboot - it won't know what the previous
//    count on the water meter was, and it won't know what the time/date is - so history is LOST - which is really
//    unfortunate.  We can try to minimize this by looking at the last log entry and using that as the current count,
//    date, and time, but the ESP32 might still loose count of some water if the 10 gallon pulse comes while
//    the ESP32 is not running.
//
// 8) Using a good Digital Scope on this actual meter we find:
//    a) It seems to close the magnetic reed switch one time every 10 gallons
//    b) The bounce time is quite minimal (< 1ms) to non existant
//    c) The switch stays closed a good long time (1 second or more)
//
//    Using a 3.3V pull-up resistor this means we see a waveform like this:
//
//    --------------------+                      +-------------------------  3.3VDC
//                        |                      |
//                        |                      |
//                        +----------------------+                           0V
//
//    So we're going to look for falling edges and count those as 10 gallons of water
//    To make the pulses from the switch we use a "pull up" resistor so that if the
//    switch is open, the voltage will read as 3.3V.   When the switch closes the
//    voltage will go to 0V.
//
//
//                                                                 E S P 3 2 Board
//
//                              +-------------------------------   GPIO 17
//                              |
//                              |      10K Ohm resistor
//       ____     Cable       _(S)_____/\/\/\___________________  (Vcc) 3.3VDC
//      /    \===============/
//      \____/               \_(G)______________________________  GND
//    water meter
//
// So, in short we are running three wires from ESP32:
//  1) Ground - to point (G) above
//  2) GPIO 0 - to point (S) above
//  3) 3.3V   - to point (Vcc) 3.3V above
//
// Connect 10K ohm resistor between points (S) and (Vcc)
//
// Arduino IDE configuration notes:
// Board: TTGO OLED ESP32 board 19-6-28 V1.1 XY-CP, board manager TTGO V1 selected
// Partition: Default (3Mb no OTA, 1Mb FFS)

//  Credits: Web server code from: Rui Santos
//  Complete project details at https://randomnerdtutorials.com  
//  Rui Santos: NTP Client: https://randomnerdtutorials.com/esp32-ntp-client-date-time-arduino-ide/

// Build notes:
// - Install the Arduino IDE (I'm using 1.8.19 on Windows 11 right now)
// - Install the ESP32 board support (lots of youtube videos on this one)
// - Install the ESPAsyncWebServer library - https://github.com/me-no-dev/ESPAsyncWebServer/archive/master.zip
// - Install the AsyncTCP library - https://github.com/me-no-dev/AsyncTCP/archive/master.zip
// - Install the NTP library - https://randomnerdtutorials.com/esp32-ntp-client-date-time-arduino-ide/
// - Install SSD1306 library from library manager
// - Create "data: folder under project, put in index.html and style.css ( folder actually named data )
// - Install the ESP32FS downloader tool - https://randomnerdtutorials.com/install-esp32-filesystem-uploader-arduino-ide/
// - Arduino IDE: Go to tools, ESP32 Sketch Data Upload, make sure serial monitor is not open, uploads file in data folder to esp32
//
// Board: LILYGO - TTGO ESP32 board with display - https://www.github.com/Xinyuan-LilyGO/TTGO-T-Display
//
//
//---------------------------------------------------------------------------------------------

// 0.4 - 10/20/2022 added scanning of log file on startup, scrolling display (sort of)
// 0.5 - 10/21/2022 added wifi reconnect
// 0.6 - 11/1/2022 - BT serial - too big with BT serial added - back it out
// 0.7 - 11/1/2022 - reboot once per day - web server gets slogged up - maybe some memory leak?
// 0.8 - 11/23/2022 - ability to echo a line to the log (update water use)
// 0.9 - 1/23/2023 - summarize log file
// 1.0 - 4/20/2023 - was stuck at 10000 gallons
// 1.1 - 5/8/2023  - still stuck at 10000 gallons - fixed
// 1.2 - 1/15/2024 - clean up for article and add report command to telnet interface

#define SIGNON "\nWaterMeterServer 1.2 Jan 15, 2024\n"
#define WANTDISPLAY 1

// File names in the SPI flash file system SPIFFS
#define LOGFN "/watermeter.log"
#define CONFIGFN "/config.ini"
#define SUMMARYFN "/dayly.log"

//---------------- CODE ----------------
// Import required libraries
#include <WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>
#include "FS.h" 
#include <SPIFFS.h>
#include <ESP32Time.h>
#include "ESPTelnet.h"

//-----------------------------------------------------------------
// real time clock (software based, not backed up for power failures
ESP32Time rtc(-8*3600);  // -8 from GMT by default


#ifdef WANTDISPLAY

#include <TFT_eSPI.h>

// This board has a built-in display that we can use
// to see readings and status it's 135 pixels by 240 pixels
TFT_eSPI tft = TFT_eSPI(135, 240); // Invoke custom library 135x240

#endif

// Telnet interface - there is a simple Telnet shell built in for diagnostic access
ESPTelnet telnet;
IPAddress ip;
uint16_t  port = 23;
int telnetConnected = false;

// Define NTP Client to get time from the network so we know the real time and date
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

//-----------------------------------------------------------------------------
// Configuration file - this is read at startup from a file (/config.ini) on 
// the internal flash file system so we know how to connext to the WiFi, etc.
//
// Information from configuration file
char ssid[128];
char password[128];
char tmpbuf[129];
long tzoffsetSeconds = 0;
long meterOffsetGallons = 0;  // in case we start off with some gallons on the meter

String ipAddress = String("unknown");

// Set GPIO pin to monitor for pulses from the water meter.   
// Small cable with two wires come out of the water meter.
//  - Connect one wire to ground on the ESP32 (if there's a black wire, connect that one to ground)
//  - Connect the other wire to a GPIO pin
//  - One 10K Ohm resistor from that GPIO pin to +3.3 V on ESP32
//  - Enter the GPIO pin number below

// which GPIO pin number of the ESP32 is connected to the water meter
#define INPUTPIN (17)

// For "comfort" we will blink that every time a pulse is detected from the water meter
// so we can see if the thing is actually working.
// GPIO pin for "comfort" LED
#define LEDPIN (2)

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

//------------------------------------------------------------------------
// convert IP address to string
String IpAddress2String(const IPAddress& ipAddress)
{
  return String(ipAddress[0]) + String(".") +\
  String(ipAddress[1]) + String(".") +\
  String(ipAddress[2]) + String(".") +\
  String(ipAddress[3])  ; 
}

//---------- Telnet printing
// ----------- print --------
void zprint(char * msg)
{
  Serial.print(msg);
  if (telnetConnected)
  {
    telnet.print(msg);  
  }
}

void zprint(String msg)
{
  Serial.print(msg);
  if (telnetConnected) 
    telnet.print(msg);
}

void zprintln(char * msg)
{
  Serial.println(msg);
  if (telnetConnected)
    telnet.println(msg);  
}

void zprintln(String msg)
{
  Serial.println(msg);
  if (telnetConnected) 
    telnet.println(msg);
}

void zprint(int x)
{
  char buf[16];
  sprintf(buf,"%d",x);
  zprint(buf);
}

void zprintln(int x)
{
  zprint(x); zprint("\r\n");
}

//-------------------------------------------------------------------
// The application supports a telnet connection for remote dianostic
// access.   There are some simple 'shell' type commands that are
// available from Telnet
//
//----------- Telnet shell command handlers
void lsCmd(String str)
{
  // "ls"
  // list the root directory
  listDir(SPIFFS,"/",9); 
}

void catCmd(String str)
{
  // cat /file.txt
  // Type the contents of a file to the telnet terminal output
  String fn = str.substring(/*cat */4);
  readFile(SPIFFS, fn.c_str());
}

void cpCmd(String str)
{
  // cp /file1.txt /file2.txt
  // copy one file to another
  String tmp = str.substring(3); // cp_
  int idx = tmp.indexOf(" ");
  if (idx < 0)
  {
    zprintln("cp command error:  cp src.file dest.file");
    return;
  }
  String srcfile = tmp.substring(0,idx);
  String destfile = tmp.substring(idx+1);
  copyFile(SPIFFS, srcfile.c_str(), destfile.c_str());  
}

void rmCmd(String str)
{
  // rm /file.txt
  // remove a file 
  String fn = str.substring(3);
  deleteFile(SPIFFS, fn.c_str());
}

void apCmd(String str)
{
  // ap /file1.txt some line to be added to the end of the file
  // append some line of text to a file which may or may not exist
  // if it doesn't exist, it will be created
  //
  String tmp = str.substring(3); 
  int idx = tmp.indexOf(" ");
  if (idx < 0)
  {
    zprintln("ap command error:  ap src.file some line of text");
    return;
  }
  String srcfile = tmp.substring(0,idx);
  String msg = tmp.substring(idx+1);
  appendFile(SPIFFS, srcfile.c_str(), msg.c_str());  
}

void reportCmd(String str)
{
  // print a report of the water meter measurement and status
  zprintln("---- Water Meter Status ----");
  zprintln(SIGNON); // signon with version information
  zprint("Ip addr: "); zprintln(IpAddress2String(WiFi.localIP()));
  zprint("Reading: "); zprintln(String(rtc.getTime("%Y/%m/%d %H:%M:%S  "))+String(ReadGallons())+String("gal."));
  long tBytes = SPIFFS.totalBytes(); 
  long uBytes = SPIFFS.usedBytes();
  long aBytes = tBytes - uBytes;
  zprintln("FileSys: "+String(tBytes)+" size (bytes), "+String(uBytes)+" used, "+String(aBytes)+" available");
}

//----------------------------------------------------------------
// Telnet handler called when a line of text is received from
// the telnet interface
void onInputReceived(String str)
{
  // -- this is the very simple telnet shell command dispatcher
  if (str=="ls")
    lsCmd(str);
  else if (str.startsWith("cat "))
    catCmd(str);
  else if (str.startsWith("cp "))
    cpCmd(str); 
  else if (str.startsWith("rm "))
    rmCmd(str);
  else if (str.startsWith("ap "))
    apCmd(str); // append one line at a time to a file
  else if (str=="report")
    reportCmd(str);
  else
    zprintln("\r\n ehh?");

  telnet.print(">"); // prompt

}


//----------------------------------------------------------------
// called by the telnet service when a new client has connected
void onTelnetConnect(String ip) 
{
  Serial.print("- Telnet: ");
  Serial.print(ip);
  Serial.println(" connected");
  telnet.println("\nWelcome " + telnet.getIP());
  telnet.println("(Use ^] + q  to disconnect.)");
  telnet.print(">"); // prompt
}


//----------------------------------------------------------------
// called by the telnet service when a client disconnects
void onTelnetDisconnect(String ip) 
{
  Serial.print("- Telnet: ");
  Serial.print(ip);
  Serial.println(" disconnected");
  telnetConnected = false;
}

//----------------------------------------------------------------
// called by the telnet service when a client reconnects
void onTelnetReconnect(String ip) 
{
  Serial.print("- Telnet: ");
  Serial.print(ip);
  Serial.println(" reconnected");
  telnetConnected = true;
}

//----------------------------------------------------------------
// called by the telnet service when a connection is attempted
void onTelnetConnectionAttempt(String ip) 
{
  Serial.print("- Telnet: ");
  Serial.print(ip);
  Serial.println(" tried to connect");
}

//----------------------------------------------------------------
// Initialize the telnet service
void setupTelnet() 
{  
  telnetConnected = false;
  Serial.print("- Telnet: "); Serial.print(ip); Serial.print(" "); Serial.println(port);  telnetConnected = true;
  // passing on functions for various telnet events
  telnet.onConnect(onTelnetConnect);
  telnet.onConnectionAttempt(onTelnetConnectionAttempt);
  telnet.onReconnect(onTelnetReconnect);
  telnet.onDisconnect(onTelnetDisconnect);
  telnet.onInputReceived(onInputReceived);

  telnet.setLineMode(true);
  
  Serial.print("- Telnet Line Mode: "); Serial.println(telnet.isLineModeSet() ? "YES" : "NO");
  
  Serial.print("- Telnet: ");
  if (telnet.begin(port)) {
    Serial.println("running");
  } else {
    Serial.println("error.");
    //errorMsg("Will reboot...");
  }
  
}

//---------------------------------------------------------------
// SPIFFS routines - dealing with the on-board flash file system
//---------------------------------------------------------------

//---------------------------------------------------------------
// list a directory with possible recursion for subdirectories
void listDir(fs::FS &fs, const char * dirname, uint8_t levels){
   zprint("Listing directory: ");
   zprintln(dirname);

   File root = fs.open(dirname);
   if(!root){
      zprintln("- failed to open directory");
      return;
   }
   if(!root.isDirectory()){
      zprintln("- not a directory");
      return;
   }

   File file = root.openNextFile();
   while(file){
      if(file.isDirectory()){
         zprint("  DIR : ");
         zprintln(file.name());
         if(levels){
            listDir(fs, file.name(), levels -1);
         }
      } else {
         zprint("  FILE: "); zprint(file.name()); zprint("\tSIZE: "); zprintln(file.size());
      }
      file = root.openNextFile();
   }
}

//---------------------------------------------------------------
// read some data from a file
void readFile(fs::FS &fs, const char * path){
   char buf[2];
  
   zprint("Reading file: "); zprintln(path);

   File file = fs.open(path);
   if(!file || file.isDirectory()){
       zprintln("- failed to open file for reading");
       return;
   }

   zprintln("-------- data read from file --------");
   buf[1]='\0';
   while(file.available()){
      buf[0]=file.read();
      zprint(buf);
   }
   file.close();
}

//---------------------------------------------------------------
// copy one file to another
void copyFile(fs::FS &fs, const char * srcpath, const char * destpath){
   zprint("Copy file: "); zprint(srcpath); zprint(" to "); zprintln(destpath);
 
   File inpfile = fs.open(srcpath);
   if(!inpfile || inpfile.isDirectory()){
       zprintln("- failed to open file for reading");
       return;
   }
  
   File outfile = fs.open(destpath, FILE_WRITE);
   if(!outfile){
      zprintln("- failed to open file for writing");
      return;
   }

   while (inpfile.available())
   {
      outfile.write(inpfile.read());
   }
   inpfile.close();
   outfile.close();
}

//---------------------------------------------------------------
// Write a message to a file
void writeFile(fs::FS &fs, const char * path, const char * message){
   zprint("Writing file: "); zprintln(path);

   File file = fs.open(path, FILE_WRITE);
   if(!file){
      zprintln("- failed to open file for writing");
      return;
   }
   if(file.print(message)){
      zprintln("- file written");
   }else {
      zprintln("- file write failed");
   }
}

//---------------------------------------------------------------
// append a message to a file
void appendFile(fs::FS &fs, const char * path, const char * message){
   zprint("Appending to file: "); zprintln(path);

   File file = fs.open(path, FILE_APPEND);
   if(!file){
      zprintln("- failed to open file for appending");
      return;
   }
   if(file.println(message)){
      zprintln("- message appended");
   } else {
      zprintln("- append failed");
   }
}

//---------------------------------------------------------------
// rename a file
void renameFile(fs::FS &fs, const char * path1, const char * path2){
   zprint("Renaming file "); zprint(path1); zprint(" to "); zprintln(path2);
   if (fs.rename(path1, path2)) {
      zprintln("- file renamed");
   } else {
      zprintln("- rename failed");
   }
}

//---------------------------------------------------------------
// delete a file
void deleteFile(fs::FS &fs, const char * path){
   zprint("Deleting file: "); zprintln(path);
   if(fs.remove(path)){
      zprintln("- file deleted");
   } else {
      zprintln("- delete failed");
   }
}


//-------------------------------------------------------------------------------------------------------
// Code to handle detecting pulses from the water meter
//------------------------------------------------------------------------------------------------------- 
//
// NOTE: DEBOUNCE is a very important consideration here - it turns out all mechanical switches
// have a characteristic that they don't go from open to closed or closed to open perfectly.
// These switches when they are operated tend to "bounce" - for example when  going from open
// to closed, they switch will bounce open-close-open-close-open-close multiple times before
// it settles to closed.
//
// Since the ESP32 is looking at the pulse thousands of times a second, we don't want to count
// each of these "bounces" as if it were a 10 gallon of water passing through the meter.
//
// To measure accurately we have to ignore these bounces somehow.   This is called "debouncing"
// a switch and is a very common thing to do in embedded systems that monitor mechanical things.
//
// For debouncing a switch, we need to know the maximum "bounce" time - how long is the switch
// in this unstable bouncing state.   The ESP32 will have to look for the very first contact,
// and then IGNORE the switch during this bounce time, and then read it a second time to see if the
// switch has really closed before counting it as the passage of another 10 gallons of water.
//

long pulseCounter = 0;  // count pulses

//----------------------------------------------------------
// Read amount of water used
#define GALLONSPERPULSE (10L)
long ReadGallons()
{
  return meterOffsetGallons + pulseCounter * GALLONSPERPULSE; 
}

//------------------------------------------------------------
// Notes on how we count pulses
//
// We're going to use a timer interrupt for this task.
// This means we will set up an ESP32 timer to cause a CPU 
// interrupt periodically - like every 1ms.
//
// There will be a special function - an interrupt service
// routine - or ISR - that will be called every 1ms no matter
// what else the CPU is doing.
//
// In that ISR we'll have some code that watches for debounced
// falling edges (transition from a GPIO pin reading 1 to reading 0).
// We'll count the number of falling edge transitions using a
// variable called pulseCounter which will get incremented by 1
// each time the ISR detects a falling edge.
//
// Then in main code, we can read pulseCounter and multiply by 10
// to see how many gallons have passed through the water meter.

hw_timer_t *My_timer = NULL; // handling a timer on the ESP32 chip

// This ISR is set up as a state machine to detect falling edges.
// The state machine can be in one of three states:
// 0 - waiting for a falling edge
//     When the ISR sees one, it transitions to state 1
// 1 - waiting for the debounce interval to expire
//     Here we completely ignore the GPIO pin until the
//     debounce period expires - so we won't count a mechanically
//     bouncing switch (that produces multiple edges) as multiple
//     10 gallons passing through the meter.   After the debounce
//     period we transition to state 2
// 2 - checking to see if it's a real debounced falling edge
//     read the GPIO pin again and if it's 0 after ignoring it
//     for the debounce period.   
//     If it is, then we count it as a pulse
//     If it is not a 0 after debounce period, then it was just some
//     momentary noise, and we ignore it.   
//     Either way we go back to state 0
//
// -- states of a state-machine that runs in the ISR to look for pulses and debounce them
#define STATE_WAIT_FOR_EDGE (0)
#define STATE_DEBOUNCE (1)
#define STATE_HANDLE_EDGE (2)

int state=STATE_WAIT_FOR_EDGE;
int lastGpioState=LOW;
int debounceCounter=0;
int edge=0;

#define BOUNCE_WAIT (100) /* ms */

//------------------------------------------------------------------
// Handle a debounced detection of a rising (0-1 transition) or 
// falling (1-0 transition) of a pulse

void handleEdge(int edge)
{
  // count only falling edges as 10 gallons!
  // -1 means rising, +1 means falling
  digitalWrite(LEDPIN,(edge<0 ? HIGH : LOW));
  if (edge > 0) pulseCounter++; // count falling edges
}

//------------------------------------------------------------------
// This is ISR (interrupt service routine) code that gets run
// by the action of a continuous running timer inside the ESP32
// In SETUP we will program the timer to generate an interrupt
// every millisecond.   Then we can expect this code to run
// once every millisecond.  
//
// Every millisecond, the code will look at what state the
// state machine is in and take appropriate action, moving from
// waiting for the first edge, ignoring the switch for a debounce
// time, and handling the edge if one was detected.
//
// ** IMPORTANT NOTE **
// because this is an ISR, we want to minimize the amount of work
// to be done.   In this case just reading some GPIO pin, some if
// statements, managing the state variable, and incrementing the
// pulseCounter variable.
// NO Serial IO!   NO long calculations, no creating objects or
// destroying objects.  K/I/S/S - keep it simple and stupid.
//

void IRAM_ATTR onTimer(){
  // simple state machine
  int currentGpioState=digitalRead(INPUTPIN);
  if (state==STATE_WAIT_FOR_EDGE)
  // -- first state: WAITING for an initial edge by reading the GPIO and
  //    seeing if it changed from last time we read it,
  //    which may indicate that the switch is changing state
  {
    if (currentGpioState != lastGpioState) // found first edge, might be noise
    {
      // ok, the GPIO pin has changed state, now we ignore it for
      // a while by going to the debounce wait state.
      state=STATE_DEBOUNCE; // wait for a while and see if it was noise
      debounceCounter=BOUNCE_WAIT; // how long are we going to ignore it?
    }
    return;
  }
  else if (state==STATE_DEBOUNCE)
  {
    // we're just tapping our fingers, ignoring the switch, for a while
    // we use a counter to count down until it gets to 0 then we check
    // if the gpio pin has really changed

    if (debounceCounter==0)
    {
      // Ok, the ignore timer has expired, and we check the gpio pin again
      currentGpioState=digitalRead(INPUTPIN);
      if (currentGpioState != lastGpioState) // ---> yep, it is still an edge, count it as debounced
      {
        state=STATE_HANDLE_EDGE; // go to handle edge state next interrupt
        edge = lastGpioState-currentGpioState; // -1 means rising, +1 means falling edge
        lastGpioState = currentGpioState;
      }
      else // ---> nope, it really didn't change, so maybe it was was just noise
      {
        state=STATE_WAIT_FOR_EDGE; // let's just go back and wait for an edge some more
      }
    }
    else
    {
      // counter hasn't gone down to 0, so we're just waiting for a while
      // and ignoring the GPIO pin
      debounceCounter--; 
    }
    return;
  }
  else // STATE==STATE_HANDLE_EDGE
  {
    // Finally, we've found a debounced edge, let's call this to handle it
    handleEdge(edge); // called for rising (0->1) and falling (1->0) edges
    state=STATE_WAIT_FOR_EDGE; // and now we go back to waiting for the next edge
  }
}


// Stores web status
String msgBuf;

//----------------------------------------------------------
// summarize log file to dayly use (midnight)
void summarizeLogFile()
{
  // the log file contains entries for every hour
  // it can get really long, and it's probably not necessary
  // to know hourly water use that much, but it's there if
  // you want it (until the flash file system runs out of space).
  // By calling this, we "squeeze" the log file to just one
  // entry per day.
  char line[128];
  File finp = SPIFFS.open(LOGFN, FILE_READ);
  File fout = SPIFFS.open(SUMMARYFN, FILE_WRITE);
  
  if (!finp)
  {
    Serial.println("Unable to read log file");
    return;
  }
  if (!fout)
  {
    Serial.println("Unable to write summary file");
    return;
  }

  fout.println("Dayly water use log");
  
  // read file line by line
  while (readln(finp, (uint8_t*) line, 127))
  {
    //                                  0123456789.123456789.123456789
    // see if log line looks like this: 2022/10/20,14:00:00,2010,gal.
    if ((strlen(line)==29)
      && (line[10]==',')
      && (line[19]==',')
      && (strncmp(&line[11],"01:00:00",8)==0)) // 1AM readings
    {
      fout.println(line);
    }
  }
  finp.close();
  fout.close();
}

//----------------------------------------------------------
// Copy summary file to main log file to 'collapse' it
// and make it smaller
void compressLogFile()
{
  // gets smaller because we reduce it to one entry per day
  // rather than one entry per hour.
  char line[128];
  File finp = SPIFFS.open(SUMMARYFN, FILE_READ);
  if (!finp)
  {
    Serial.println("Unable to read summary file");
    return;
  }

  File fout = SPIFFS.open(LOGFN, FILE_WRITE);
  if (!fout)
  {
    Serial.println("Unable to write log file");
    finp.close();
    return;
  }

  // read file line by line
  while (readln(finp, (uint8_t*) line, 127))
  {
    fout.println(line);
  }
  finp.close();
  fout.close();
}

//----------------------------------------------------------
// retrieve latest reading from log file
long getLatestReading()
{
  // if there is a power loss, we will extract the latest
  // reading from the log file and use that as the offset
  // rather than the offset in the config file.
  long reading=-1;
  File finp = SPIFFS.open(LOGFN, FILE_READ);
  if (!finp)
  {
    Serial.println("Unable to read log file");
    return -1;
  }
  char line[128];
  // read file line by line
  while (readln(finp, (uint8_t*) line, 127))
  {
    //                                  123456789.123456789.1234567890
    // see if log line looks like this: 2022/10/20,14:00:00,123456,gal.
    if ((strlen(line) > 25) // V1.1
      && (line[10]==',')
      && (line[19]==','))
    {
      long val=atol(&line[20]);
      if (val>reading) 
      {
        reading=val; // find max
      }
    }
  }
  finp.close();
  return reading;
}

//----------------------------------------------------------
// retrieve a value for a key in the config file
void readKey(char* configFn, char* key, char* outbuf, int maxlen)
{
  outbuf[0] = 0; // returning null string on error 
  //
  // Config file is key=value format
  // SSID=mywifi
  // PASSWORD=mypassword
  // TIMEZONE=-8
  // OFFSET=123590 
  //
  // pass in key with trailing = sign!!!

  //Serial.print("Looking for config key "); Serial.println(key);
  
  File finp = SPIFFS.open(CONFIGFN, FILE_READ);
  if (!finp)
  {
    Serial.println("Unable to read config file");
    return;
  }
  // scan file and look for key
  char buf[128];
  int n = strlen(key);
  while (readln(finp, (uint8_t*) buf, 127))
  {
    if (strncmp(buf,key,n) == 0) // found
    {
      //Serial.print("Found key "); Serial.println(buf);
      strncpy(outbuf,&buf[n],maxlen);
      //Serial.println(outbuf);
      break;
    }
  }
  finp.close();
 
}

//----------------------------------------------------------
// read line from input text file
int readln(File finp, uint8_t* buf, int maxlen)
{
  // return true on successful read, false on EOF
  // 10 or 13 (LF, CR) or both are EOL indicators
  int len=0;
  int eof=false;

  buf[0]=0;
  while (len<(maxlen-1))
  {
    if (!finp.available())
    {
      eof=true;
      break;
    }
    char c = finp.read();
    if (c < 0) 
    {
      eof=true;
      break; // EOF
    }
    if (c==13) continue; // ignore CR
    if (c==10) break; // end-of-line
    buf[len++]=c;
  }
  buf[len]=0; // null terminate
  return !eof;
}

//--------------------------------------------------------------
// Replaces placeholder with value in web pages
// called from deep within the ESP32 webserver somewhere
// when a page is requested that needs some variable data
String processor(const String& var)
{
  // This is a service routine called by the web page service
  // to generate dynamic content - in this case date time and
  // current water use reading - to put on the page served to
  // the http requestor
  if(var == "READING")
  {
    return String(rtc.getTime("%Y/%m/%d %H:%M:%S  "))+String(ReadGallons())+String("gal.");
  }
  if(var == "LOGINFO")
  {
    return String(SIGNON);
  }
  return String();
}

//-----------------------------------------------------------------
// print diagnostic information - including the on-board display
void print(char* msg)
{
#ifdef WANTDISPLAY
  char buf[256];
  int cursorLine;
  strncpy(buf,msg,200);
  tft.print(buf);
  cursorLine=tft.getCursorY(); // pixels, rows are 8 pixels high, characters are 6 pixels wide
  if (cursorLine>16*8) tft.setCursor(0,32);
#endif
  Serial.print(msg);
}

//-----------------------------------------------------------------
// print with end-of-line
void println(char* msg)
{
  print(msg);
  print("\n");
}

//-----------------------------------------------------------------
// print diagnostic information
void print(String msg)
{
  char buf[256];
  msg.toCharArray(buf,255);
  print(buf);
  Serial.print(msg);
}


//-----------------------------------------------------------------
// print with end-of-line
void println(String msg)
{
  print(msg);
  print("\n");
}



//-----------------------------------------------------------------
// The main setup routine for the water meter
// Runs one time on boot-up
void setup()
{
  int wifiWaitCounter = 0;
  long largestReading;
  
  // ------ Set up the serial port for debugging purposes
  Serial.begin(115200);
  Serial.println(SIGNON);

  
  // ------ setup GPIO pins
  pinMode(LEDPIN, OUTPUT); // set up comfort LED
  pinMode(INPUTPIN, INPUT); // input signal from water meter
  
  // ------ Initialize SPIFFS (file system) so we can read the config file
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    for (;;) ; // can't do anything if there's no file system
    // since we can't read the config file
    return;
  }

  // ------ initialize the RTC, uses timer 0
  rtc.setTime(01,02,03,10, 10, 2022); // default time 01:02:03 10/10/2022

  //------------ Set up the timer and ISR ------------------
  // timer 1 - 1ms interrupts for debounce logic
  My_timer = timerBegin(1, 80, true);
  timerAttachInterrupt(My_timer, &onTimer, true);
  timerAlarmWrite(My_timer, 1000, true); // timer for 1ms interrupts
  timerAlarmEnable(My_timer); //Just Enable

  //------------ Read config file ---------------
  // read 4 values from the config file
  readKey(CONFIGFN,"SSID=",ssid,127); // get from the config file
  readKey(CONFIGFN,"PASSWORD=",password,127);
  readKey(CONFIGFN,"TIMEZONE=",tmpbuf,127);
  tzoffsetSeconds=atol(tmpbuf);
  readKey(CONFIGFN,"METEROFFSET=",tmpbuf,127);
  meterOffsetGallons=atol(tmpbuf);

  // ------ get latest reading from the log file to start out our measurement
  largestReading=getLatestReading();
  if (largestReading>meterOffsetGallons) meterOffsetGallons=largestReading; // keep larger of offset in config file and log file


  // ------ Connect to Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(1000);
    Serial.println("Connecting to WiFi..");
    if (wifiWaitCounter++ > 30) break;
  }
  ipAddress = IpAddress2String(WiFi.localIP());
  Serial.println(ipAddress);   // Print ESP32 Local IP Address

  // ------ initialize the telnet server
  setupTelnet(); // V1.0

  
  // ------ Initialize NTP Client
  timeClient.begin();
  timeClient.setTimeOffset(0);

  // ------ Initialize the OLED Display
#ifdef WANTDISPLAY
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);

    tft.setSwapBytes(true);

    tft.setRotation(1);
    tft.fillScreen(TFT_RED);
    delay(250);
    tft.fillScreen(TFT_BLUE);
    delay(250);
    tft.fillScreen(TFT_GREEN);
    delay(250);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.setCursor(0, 0);

    print(SIGNON);
    println(ipAddress);
    println("---------------------------------");
    // NOTE: tft.setCursor(x,y) - x and y are pixel based
    // with 5x7 char set, characters are 6 wide, 8 high
    //
#endif

  //-------------------------------------------------------
  // Setup the expected web page handlers
  // - This is an assynchronous web server, when requests
  //   come in - specific handler request routines are
  //   called.
  //
  // Supported pages
  //  /    - index.html - show root page to the requester
  //  /log - show the log file to the requester
  //  /dayly  - show the dayly (summarized) log file to the requester
  //  /compress - compress the log file and show the compressed file
  //  /maintenance?metercorrection=123    - set a meter correction factor
  //-------------------------------------------------------
  // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });
  
  // Route to load style.css file
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/style.css", "text/css");
  });

  // Route to read log file
  server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request){    
    request->send(SPIFFS, LOGFN, String(), false, processor);
  });

  // Route to summarize log file (V0.8)
  server.on("/daily", HTTP_GET, [](AsyncWebServerRequest *request){    
    summarizeLogFile();
    request->send(SPIFFS, SUMMARYFN, String(), false, processor);
  });

   // Route to summarize log file (V0.8)
  server.on("/compress", HTTP_GET, [](AsyncWebServerRequest *request){    
    summarizeLogFile();
    compressLogFile();
    request->send(SPIFFS, LOGFN, String(), false, processor);
  }); 
  
  // Route to maintenance page
  server.on("/maintenance", HTTP_GET, [](AsyncWebServerRequest *request){
    String message; // /maintenance?metercorrection=123

    if (request->hasParam("metercorrection"))
    {
      message = request->getParam("metercorrection")->value();
      long correction = message.toInt();
      meterOffsetGallons += correction;
      String msg = String(rtc.getTime("%Y/%m/%d,%H:%M:%S,")+String(correction)+String(",gal.(CORRECTION FROM WEB)"));
      appendToLogFile(msg);
    }
    request->send(SPIFFS, LOGFN, String(), false, processor);
  });

  // Start the web server
  server.begin();

  // write a message to the log file that we started up
  appendToLogFile(rtc.getTime("%Y/%m/%d,%H:%M:%S,Startup completed"));

}

int lastHr = -1;
//-------------------------------------------------------------
// Top of the hour detector
int topOfTheHourDetector()
{
  // Return true one time every hour, no mater how many
  // times this is called.
  int hr = rtc.getHour(true);
  if (hr == lastHr) return false;

  lastHr = hr;
  return true;
}

int lastSec = -1;
//-------------------------------------------------------------
// One second detector
int secondDetector()
{
  // Return true one time every second, no mater how many
  // times this is called.
  int sec = rtc.getSecond();
  if (sec == lastSec) return false;

  lastSec = sec;
  return true;
}

int lastDay = -1;
//-------------------------------------------------------------
// one day detector V0.7
int dayDetector()
{
  // return true one time every day
  int day = rtc.getDay();
  if (day == lastDay) return false;
  if (lastDay==-1)
  {
    lastDay = day;
    return false;
  }
  // now we know it's a new day, is it 02:00:00 AM?
  int twoAm = (rtc.getHour(true)==2) && (rtc.getMinute()==0) && (rtc.getSecond()==0);
  if (!twoAm) return false; // otherwise 
  
  // do this at 2:00:00 AM on a new day only
  lastDay = day;
  return true;
}

//-------------------------------------------------------------
//
void appendToLogFile(String msg)
{
  println(msg);

  // append to log file
  File fout = SPIFFS.open(LOGFN, FILE_APPEND);
  if (!fout)
  {
    println("Unable to append to log file");
  }
  else
  {
    fout.println(msg);
    fout.close();
  }
}

//-------------------------------------------------------------
// Logging process
void updateLog()
{
  long reading = ReadGallons();
  String msg = String(rtc.getTime("%Y/%m/%d,%H:%M:%S,")+String(reading)+String(",gal."));
  appendToLogFile(msg);
}

//-------------------------------------------------------------
// Clear log file
void clearLog()
{
  Serial.println("---- Clear log file ----");
  File fout = SPIFFS.open(LOGFN, FILE_WRITE);  
  if (!fout)
  {
    Serial.println("Unable to clear log file");
  }
  else
  {
    fout.println(rtc.getTime("%Y/%m/%d,%H:%M:%S,Reset log file"));
    fout.close();
  }
}

//-------------------------------------------------------------
// Dump log to serial port
void dumpLog()
{
  Serial.println("---------------- LOG FILE ------------------");
  File finp = SPIFFS.open(LOGFN, FILE_READ);
  if (!finp)
  {
    Serial.println("Unable to read log file");
  }
  else
  {
    char buf[128];
    while (readln(finp, (uint8_t*) buf, 127))
    {
      Serial.println(buf);
    }
    finp.close();
  }
  Serial.println("-------------------------------------------");  
}

//-------------------------------------------------------------
void dumpStatus()
{
  Serial.println("\n-------- STATUS --------------");
  Serial.println("Time   : "+rtc.getTime("%Y/%m/%d,%H:%M:%S"));
  Serial.print  ("IP     : "); Serial.println(WiFi.localIP());
  Serial.print  ("Reading: "); Serial.println(ReadGallons());
  Serial.println("------------------------------");
}
int waitingForNtp = true;

int wifiDownCounter = 0;
#define WIFI_STATUS_DOWN (0)
#define WIFI_STATUS_CONNECTING (1)
#define WIFI_STATUS_UP (2)
int wifiStatus=WIFI_STATUS_UP;

//-------------------------------------------------------------
// One second tasks
void doOneSecondTasks()
{
  // check on WIFI
  // if it's down for 10 seconds, 
  //   disconnect and wait 20 seconds
  //   then try to reconnect every 19 seconds until it's reconnected
  //
  if (WiFi.status() == WL_CONNECTED)
  {
    wifiDownCounter=0;
    wifiStatus=WIFI_STATUS_UP;
  }
  else
  {
    wifiDownCounter++;
    if (wifiDownCounter == 10) // if WIFI has been down for 10 seconds
    {
      wifiStatus=WIFI_STATUS_DOWN; // we disconnect at 10 seconds of being down
      WiFi.disconnect();
      println("-- WIFI down --  ");
    }
    if (wifiDownCounter == 30)
    {
      WiFi.begin(ssid,password); // at 30 seconds, we try to reconnect
      println("-- WIFI connecting --  ");
      wifiDownCounter=11; // keep trying to reconnect every 20 seconds
    }
  }
}

//-------------------------------------------------------------
// The main loop of the water meter - called over and over
// again forever until power fails
void loop()
{
  // Hourly tasks ----------------------
  if (topOfTheHourDetector())
  {
    updateLog();  // write reading to log file every hour
  }

  // One second tasks ------------------
  if (secondDetector())
  {
    doOneSecondTasks();
  }

  if (dayDetector()) // 0.7 - reboot once per day at 2am
  {
    ESP.restart(); // reboot this thing one time per day
  }
  
  // Waiting for Network Time Protocol client to get a response
  if (waitingForNtp)
  {
    if (!timeClient.update())
      timeClient.forceUpdate();
    else
      waitingForNtp = false;

    if (!waitingForNtp)
    {
      unsigned long epochTime = timeClient.getEpochTime();
      print("NTP Time: ");
      println(timeClient.getFormattedDate());
      rtc.setTime(epochTime);
      appendToLogFile(rtc.getTime("%Y/%m/%d,%H:%M:%S,BOOT/NTP"));
    }
  }

  telnet.loop(); // process any telnet traffic

  // some simple commands for the serial port
  //
  int charAvail = -1;
  if (Serial.available()) charAvail = Serial.read();
  // serial diagnostic commands
  if (charAvail > 0)
  {
    int c = charAvail;
    if (c=='d') dumpLog();
    if (c=='?') dumpStatus();
    if (c=='u') updateLog();
    
  }
}
