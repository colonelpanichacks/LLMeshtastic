#include <Arduino.h>
#include <M5Unified.h>
#include <ArduinoJson.h>

// ----------------------------------------------------
// Serial Port Assignments:
// ----------------------------------------------------
// PORT-C1 (LLM Port): Reversed pins – RX on GPIO14, TX on GPIO13.
HardwareSerial LLMSerial(1);
// PORT-C2 (External COM Port): Reversed pins – RX on GPIO17, TX on GPIO16.
HardwareSerial extSerial(2);

// ----------------------------------------------------
// Global Variables
// ----------------------------------------------------
String llmWorkId = "llm_work_id";  // Updated after successful LLM setup.
bool bootComplete = false;
unsigned long bootStart = 0;
const int lineHeight = 20;  // For display printing

// ----------------------------------------------------
// Old-School Display Printing
// ----------------------------------------------------
void printLine(const String &line) {
  int y = M5.Display.getCursorY();
  // If printing the next line would exceed the display height, clear the screen.
  if (y + lineHeight > M5.Display.height()) {
    M5.Display.clear(BLACK);
    M5.Display.setCursor(0, 0);
  }
  M5.Display.println(line);
}

// ----------------------------------------------------
// Communication Functions
// ----------------------------------------------------
String readJsonMessage(unsigned long timeout = 5000) {
  String candidate = "";
  unsigned long startTime = millis();
  while (millis() - startTime < timeout) {
    if (LLMSerial.available() > 0) {
      candidate = LLMSerial.readStringUntil('\n');
      candidate.trim();
      int idx = candidate.indexOf('{');
      if (idx >= 0) {
        candidate = candidate.substring(idx);
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, candidate);
        if (!err) {
          return candidate;
        }
      }
    }
    delay(10);
  }
  return "";
}

bool sendSetupQuery() {
  StaticJsonDocument<512> doc;
  doc["request_id"] = "llm_001";
  doc["work_id"] = "llm";
  doc["action"] = "setup";
  doc["object"] = "llm.setup";
  JsonObject dataObj = doc.createNestedObject("data");
  dataObj["model"] = "qwen2.5-0.5B-prefill-20e";
  dataObj["response_format"] = "llm.utf-8.stream";
  dataObj["input"] = "llm.utf-8.stream";
  dataObj["enoutput"] = true;
  dataObj["max_token_len"] = 1023;
  dataObj["prompt"] = "You are a knowledgeable assistant capable of answering various questions and providing information.";
  
  String jsonString;
  serializeJson(doc, jsonString);
  LLMSerial.println(jsonString);
  delay(200); // Allow time for response.
  
  String response = readJsonMessage(5000);
  response.trim();
  
  StaticJsonDocument<512> respDoc;
  DeserializationError err = deserializeJson(respDoc, response);
  if (err) return false;
  
  const char* respRequestId = respDoc["request_id"];
  if (!respRequestId || strcmp(respRequestId, "llm_001") != 0) return false;
  
  JsonObject errorObj = respDoc["error"];
  if (errorObj.containsKey("code") && errorObj["code"].as<int>() != 0) return false;
  
  const char* workId = respDoc["work_id"];
  if (!workId) return false;
  
  llmWorkId = String(workId);
  return true;
}

/**
 * Reads the full inference response from LLMSerial.
 * It waits until the "finish" flag is received or until no new data arrives
 * for 'inactivityTimeout' milliseconds, up to an overall timeout of 'overallTimeout' milliseconds.
 */
String readFullResponse(unsigned long overallTimeout = 60000, unsigned long inactivityTimeout = 15000) {
  String completeResponse = "";
  bool finished = false;
  unsigned long startTime = millis();
  unsigned long lastReceiveTime = millis();
  
  while (!finished && (millis() - startTime < overallTimeout)) {
    if (LLMSerial.available() > 0) {
      lastReceiveTime = millis();  // Reset inactivity timer when data arrives.
      String line = LLMSerial.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) {
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, line);
        if (error) continue;
        JsonObject dataObj = doc["data"];
        if (dataObj.containsKey("delta")) {
          completeResponse += String(dataObj["delta"].as<const char*>());
        }
        if (dataObj.containsKey("finish") && dataObj["finish"].as<bool>() == true) {
          finished = true;
        }
      }
    } else {
      if (millis() - lastReceiveTime > inactivityTimeout) break;
    }
    delay(10);
  }
  return completeResponse;
}

// ----------------------------------------------------
// Command Cleaning Function
// ----------------------------------------------------
/**
 * Clean the incoming command:
 * 1. Trim the string.
 * 2. If it starts with "CMD:", remove that prefix.
 * 3. Then, if any colon remains, remove everything before and including the first colon.
 * Returns only the intended message.
 */
String cleanCommand(const String &cmd) {
  String cleaned = cmd;
  cleaned.trim();
  if (cleaned.startsWith("CMD:")) {
    cleaned = cleaned.substring(4);
    cleaned.trim();
  }
  int colonIndex = cleaned.indexOf(':');
  if (colonIndex != -1) {
    cleaned = cleaned.substring(colonIndex + 1);
  }
  cleaned.trim();
  return cleaned;
}

// ----------------------------------------------------
// Sentence Splitting and Sending Function
// ----------------------------------------------------
/**
 * Splits the provided text into sentences using ". " as the delimiter.
 * Each sentence is sent individually over extSerial with a 1-second delay between sentences.
 */
void sendSentences(const String &text) {
  int startIndex = 0;
  while (startIndex < text.length()) {
    int index = text.indexOf(". ", startIndex);
    if (index == -1) {
      String sentence = text.substring(startIndex);
      sentence.trim();
      if (sentence.length() > 0) {
        extSerial.println(sentence);
        delay(1000);
      }
      break;
    } else {
      String sentence = text.substring(startIndex, index + 1);  // Include the period.
      sentence.trim();
      if (sentence.length() > 0) {
        extSerial.println(sentence);
        delay(1000);
      }
      startIndex = index + 2; // Skip ". " delimiter.
    }
  }
}

// ----------------------------------------------------
// Setup
// ----------------------------------------------------
void setup() {
  M5.begin();
  M5.Display.clear(BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 0);
  printLine("Booting LLM...");
  printLine("Waiting 20 seconds...");
  
  // Initialize serial ports:
  // PORT-C1 (LLM): RX on GPIO14, TX on GPIO13.
  LLMSerial.begin(115200, SERIAL_8N1, 14, 13);
  // PORT-C2 (External COM): RX on GPIO17, TX on GPIO16.
  extSerial.begin(115200, SERIAL_8N1, 17, 16);
  
  bootStart = millis();
}

// ----------------------------------------------------
// Main Loop
// ----------------------------------------------------
void loop() {
  M5.update();
  
  // Wait for boot delay.
  if (!bootComplete) {
    if (millis() - bootStart >= 20000) {
      bootComplete = true;
      if (sendSetupQuery()) {
        printLine("LLM Setup Complete.");
        printLine("WorkID: " + llmWorkId);
      } else {
        printLine("LLM Setup Failed.");
      }
    } else {
      return;
    }
  }
  
  // Process an inference command from the external device.
  if (extSerial.available() > 0) {
    String command = extSerial.readStringUntil('\n');
    command.trim();
    command = cleanCommand(command);
    
    if (command.length() > 0) {
      // Print only the cleaned command.
      printLine(command);
      
      // Build JSON query for the LLM.
      StaticJsonDocument<256> doc;
      doc["request_id"] = "llm_001";
      doc["work_id"] = llmWorkId;
      doc["action"] = "inference";
      doc["object"] = "llm.utf-8.stream";
      JsonObject jsonData = doc.createNestedObject("data");
      jsonData["delta"] = command;
      jsonData["index"] = 0;
      jsonData["finish"] = true;
      
      String jsonString;
      serializeJson(doc, jsonString);
      LLMSerial.println(jsonString);
      
      // Read the full response from the LLM.
      String response = readFullResponse(60000, 15000);
      printLine("Response:");
      printLine(response);
      
      // Split the full response into sentences and send each one with a 1-second delay.
      sendSentences(response);
    }
  }
  
  delay(10);
}
