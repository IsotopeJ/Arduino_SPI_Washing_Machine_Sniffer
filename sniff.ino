
//setup pins
const byte SNIFF_CLOCK = 2;
const byte SNIFF_MOSI = 9;  //PORTK bit 1
const byte SNIFF_MISO = 10; //PORTK bit 2



byte MOSI_buffer;
byte MISO_buffer;

byte bitsRead;
unsigned long millisLastClock;

bool readingMOSIMessage;  //set when new MOSI message detected
bool readingMISOMessage;  //set when new MISO message detected
bool pollingSlave;
byte bytesToRead;
byte checksum;

byte message[20];
byte messageLength;
byte messageSource;
byte MOSI_last_byte;
byte MISO_acknowledge;

//LIFO message queue
//format: M 04 FF FF FF FF
//first character = source (master or slave) second char is message length
byte messageQueue[10][20];
const byte MQUEUE_SIZE = 10;
byte messageQueuePushIndex;
byte messageQueuePopIndex;

//LIFO queue for strings to send over serial
String stringQueue[5];
const byte QUEUE_SIZE = 5;
byte stringQueuePushIndex;
byte stringQueuePopIndex;

void sendMessage(String);
void queueMessage(String);
bool isValidMessage();
void processMessage();

String uartString;

void setup() {
    //SPI.begin();
    bitsRead = 0;
    bytesToRead = 0xFF;
    readingMOSIMessage = false;
    readingMISOMessage = false;
    pollingSlave = false;
    stringQueuePushIndex = 0;
    stringQueuePopIndex = 0;
    messageQueuePushIndex = 0;
    messageQueuePopIndex = 0;
    pinMode(SNIFF_MOSI, INPUT);
    pinMode(SNIFF_MISO, INPUT);
    pinMode(SNIFF_CLOCK, INPUT);
    attachInterrupt(digitalPinToInterrupt(SNIFF_CLOCK), clockPulse, FALLING);
    
    Serial.begin(115200);
    Serial.println("Hello.");
    queueMessage("test");
}





void loop() {
  /*
    //clear bit counter between each byte of traffic on SPI bus (only necessary if gets out of sync)
    if(millis() - millisLastClock > 3){
      bitsRead = 0;
      MOSI_buffer = 0x00;
      MISO_buffer = 0x00;
    }
    */
    /*
    //check if a string is queued
    if(stringQueuePopIndex != stringQueuePushIndex){
      sendMessage(stringQueue[stringQueuePopIndex]);
      stringQueuePopIndex++;
      if(stringQueuePopIndex == QUEUE_SIZE) stringQueuePopIndex = 0;
    }
    */
    
    //check if message queued, build string and send over serial to PC
    if(messageQueuePopIndex != messageQueuePushIndex){
      uartString = messageQueue[messageQueuePopIndex][0] == 1 ? "S " : "M ";
      uartString += "0"+String(messageQueue[messageQueuePopIndex][1],HEX)+" ";
      for(byte i=0; i<messageQueue[messageQueuePopIndex][1];i++ ){
        uartString += ((messageQueue[messageQueuePopIndex][i+2]<16?"0":"")+String(messageQueue[messageQueuePopIndex][i+2],HEX) + " ");
      }
      //uartString += (String(messageQueue[messageQueuePopIndex][2],HEX) + " ");
      Serial.println(uartString);
      messageQueuePopIndex++;
      if(messageQueuePopIndex >= MQUEUE_SIZE) messageQueuePopIndex = 0; 
    }
}

void clockPulse(){
  millisLastClock = millis();
  
  MOSI_buffer = (MOSI_buffer << 1) + !digitalRead(SNIFF_MOSI); // == 1? 0:1);
  MISO_buffer = (MISO_buffer << 1) + !digitalRead(SNIFF_MISO); // == 1? 0:1);
  
  //MOSI_buffer = (MOSI_buffer << 1) + ((PORTK & 0b00000010) >> 1);  //better time with direct port access(?)
  //MISO_buffer = (MISO_buffer << 1) + ((PORTK & 0b00000100) >> 2);
  bitsRead++;
  //Serial.print(bitsRead);
  if(bitsRead > 7){
    //Serial.println(" "+String(MOSI_buffer,HEX)+" "+String(MISO_buffer,HEX)+" "+String(readingMOSIMessage)+" "+String(readingMISOMessage)+" "+pollingSlave);
    //queueMessage(String(MOSI_buffer,HEX)+" "+String(MISO_buffer,HEX)+" "+readingMOSIMessage+" "+readingMISOMessage+" "+pollingSlave);
    bitsRead = 0;
    
    //not in the middle of any message, this is the first byte recorded on the bus
    if( !readingMOSIMessage && !readingMISOMessage && MOSI_buffer == 0xAA){  //look for start of outgoing message
      readingMOSIMessage = true;
      return;
    }
    if( !readingMOSIMessage && !readingMISOMessage && MOSI_buffer == 0xC6){  //indicates master is polling slave
      pollingSlave = true;
      return;
    }
    
    //we read the first byte captured on the bus and it indicated master was polling slave
    //now reading message length in bytes
    if(pollingSlave){
      if(MISO_buffer != 0xFF && MISO_buffer != 0x00){       //0xFF is not ready, 0x00 is no data to send
        if(MISO_buffer > 20){ //bad data no way a message is this long
          pollingSlave = false;
          return;
        }
  //Serial.println("LS: "+String(MISO_buffer,HEX));
        bytesToRead = MISO_buffer + 1;                      //message + checksum
        messageLength = MISO_buffer;
        readingMISOMessage = true;
      }
      pollingSlave = false;
      return;
    }
    
    //readingMISOMessage means we already have captured the number of bytes to read from the slave
    if(readingMISOMessage){
      bytesToRead--;
      if(bytesToRead > 0){
        message[messageLength-bytesToRead] = MISO_buffer;
        return;
      }
      else{
        checksum = MISO_buffer;
        messageSource = 1;
        //Serial.println("r: "+String(messageSource)+" "+String(messageLength,HEX));
        processMessage();
        bytesToRead = 0xFF;           //indicates it is not set yet
        readingMISOMessage = false;
        return;
      }
    }
    
    //we have just started or are in the process of intercepting a message from the master
    if(readingMOSIMessage){
      //bytes haven't been read yet, we are at beginning of message
      if(bytesToRead == 0xFF && MOSI_buffer < 20){  
        messageLength = MOSI_buffer;
        bytesToRead = MOSI_buffer + 2;  //(add 2 bytes for checksum and response)
        return; 
      }
      //we are in middle of reading data of message
      else if(bytesToRead > 2){
        message[messageLength+2-bytesToRead] = MOSI_buffer;
        bytesToRead--;
        return;
      }
      //we are at end of message (checksum)
      else if(bytesToRead == 2){
        checksum = MOSI_buffer;
        bytesToRead--;
        return;
      }
      //we have read checksum, now checking for acknowledgement
      else if(bytesToRead == 1){
        MOSI_last_byte = MOSI_buffer;
        MISO_acknowledge = MISO_buffer;
        messageSource = 0;
        //Serial.println("r: "+String(messageSource)+" "+String(messageLength,HEX));
        processMessage();
        bytesToRead = 0xFF;
        readingMOSIMessage = false;
        return;
      }
    }    
  }
}

void processMessage(){
  /*
  String msg;
  if(isValidMessage()){
    msg = messageSource;
    for(byte i=0;i<messageLength;i++){
      msg += String(message[i], HEX) + " ";
    }
    queueMessage(msg);
  }
  */
  
  //add message to message queue
  if(true){
    messageQueue[messageQueuePushIndex][0] = messageSource;
    messageQueue[messageQueuePushIndex][1] = messageLength;
    for(byte i=0; i<messageLength; i++){
      messageQueue[messageQueuePushIndex][i+2] = message[i];
    }
    messageQueuePushIndex++;
    if(messageQueuePushIndex >= MQUEUE_SIZE) messageQueuePushIndex = 0;
  }
}

bool isValidMessage(){
  byte calcChecksum = 0x00;
  for(byte i=0; i< messageLength; i++){
    calcChecksum ^= message[i];
  }
  if(calcChecksum == checksum)
    return true;
  else
    return false;
}

void queueMessage(String msg){
  stringQueue[stringQueuePushIndex] = msg;
  stringQueuePushIndex++;
  if(stringQueuePushIndex == QUEUE_SIZE ) stringQueuePushIndex = 0;
}

void sendMessage(String msg){
  Serial.println(msg);
}
