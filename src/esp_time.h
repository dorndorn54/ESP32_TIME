#ifndef TIME_H
#define TIME_H

#include <Arduino.h>

// Simple time functions for ESP32
// Must call setupTime() after WiFi is connected

// Initialize NTP time sync (call after WiFi connects)
void setupTime() {
    configTime(28800, 0, "pool.ntp.org", "time.nist.gov");  // GMT+8 for Singapore
    Serial.println("Time configured, waiting for sync...");
    delay(2000);  // Wait for time to sync
}

// Get current time in "HH:MM AM/PM" format
String getCurrentTime() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return "-- : --";
    }
    
    char timeStr[20];
    strftime(timeStr, sizeof(timeStr), "%I:%M %p", &timeinfo);  // 12-hour format with AM/PM
    return String(timeStr);
}

// Get current time in "HH:MM" format (24-hour)
String getCurrentTime24() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return "--:--";
    }
    
    char timeStr[10];
    strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);  // 24-hour format
    return String(timeStr);
}

// Get current date in "DD/MM/YYYY" format
String getCurrentDate() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return "--/--/----";
    }
    
    char dateStr[15];
    strftime(dateStr, sizeof(dateStr), "%d/%m/%Y", &timeinfo);
    return String(dateStr);
}

// Check if time is synced
bool isTimeSynced() {
    struct tm timeinfo;
    return getLocalTime(&timeinfo);
}

#endif