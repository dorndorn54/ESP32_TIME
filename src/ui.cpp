#include <Arduino.h>
#include <SpotifyEsp32.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <ui.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_time.h" // Assumed external header for time functions
#include "secrets.h" // Assumed external header for WiFi/Spotify credentials
#include "debouncer.h"
#include "rotary.h"


TaskHandle_t spotifyTaskHandle = NULL;

/*buttons definitions*/
#define buttonPrev 25   // Previous track
#define buttonPlay 26   // Play/Resume
#define buttonPause 33  // Pause/Stop
#define buttonNext 27   // Next track

Debouncer button1(buttonPrev);
Debouncer button2(buttonPlay);
Debouncer button3(buttonPause);
Debouncer button4(buttonNext);

/*rotor pins*/
#define SW 14
#define DT 32
#define CLK 13
RotaryEncoder rotary(SW, DT, CLK);

// Add these global flags with your other globals
static bool requestPlay = false;
static bool requestNextTrack = false;
static bool requestPrevTrack = false;
static bool requestStop = false;

static bool increaseVolume = false;
static bool decreaseVolume = false;
static bool toggleMute = false;

/* Cache for all display data */
String lastTrack = "";
String cachedArtist = "";
String cachedTrack = "";
String cachedDeviceName = "";

/* Thread-safe handoff to main core */
static String nextArtist = "";
static String nextTrack = "";
static String nextDevice = "";
static bool newArtist = false;
static bool newTrack = false;
static bool newDevice = false;
SemaphoreHandle_t data_mutex = NULL;

// curr and end time
unsigned long cachedProgress = 0;
unsigned long cachedDuration = 0;
unsigned long progressTimestamp = 0;  // When we last got progress from API
bool isCurrentlyPlaying = false;

/*Screen settings*/
static const uint16_t screenWidth  = 240;
static const uint16_t screenHeight = 320;

// LVGL buffer - dynamic allocation
enum { SCREENBUFFER_SIZE_PIXELS = screenWidth * screenHeight / 10 };
static lv_color_t* buf = nullptr;

TFT_eSPI tft = TFT_eSPI( screenWidth, screenHeight );
Spotify sp(CLIENT_ID, CLIENT_SECRET, REFRESH_TOKEN);

// Timing for non-blocking updates
unsigned long lastSpotifyUpdate = 0;
unsigned long lastTimeUpdate = 0;
const unsigned long SPOTIFY_UPDATE_INTERVAL = 1000;
const unsigned long TIME_UPDATE_INTERVAL = 1000;

#if LV_USE_LOG != 0
void my_print(const char * buf) {
    Serial.printf(buf);
    Serial.flush();
}
#endif

// LVGL Display Flush Callback
void my_disp_flush (lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
    uint32_t w = ( area->x2 - area->x1 + 1 );
    uint32_t h = ( area->y2 - area->y1 + 1 );

    tft.startWrite();
    tft.setAddrWindow( area->x1, area->y1, w, h );
    tft.pushColors( (uint16_t*) pixelmap, w * h, true );
    tft.endWrite();

    lv_disp_flush_ready( disp );
}

// LVGL Touchpad Read Callback (Placeholder - no actual touch logic)
void my_touchpad_read (lv_indev_t * indev_driver, lv_indev_data_t * data) {
    uint16_t touchX = 0, touchY = 0;
    bool touched = false;

    if (!touched) {
        data->state = LV_INDEV_STATE_REL;
    } else {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touchX;
        data->point.y = touchY;
    }
}

static uint32_t my_tick_get_cb (void) { return millis(); }

void printMemory(const char* location) {
    //Serial.printf("[%s] Free: %d, Largest: %d\n", location, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

// Utility function to format milliseconds into M:SS string
String formatTime(unsigned long ms) {
    unsigned long seconds = ms / 1000;
    unsigned long minutes = seconds / 60;
    seconds = seconds % 60;
    
    char timeStr[10];
    sprintf(timeStr, "%lu:%02lu", minutes, seconds);
    return String(timeStr);
}

// Calculates estimated track progress based on last API call and elapsed time
unsigned long getEstimatedProgress() {
    if (!isCurrentlyPlaying || cachedDuration == 0) {
        return cachedProgress;
    }
    
    // Calculate elapsed time since last API update
    unsigned long elapsed = millis() - progressTimestamp;
    unsigned long estimated = cachedProgress + elapsed;
    
    // Don't exceed track duration
    if (estimated > cachedDuration) {
        estimated = cachedDuration;
    }
    
    return estimated;
}

// Worker function to fetch Spotify data (runs on Core 1)
void updateSpotifyData() {
    unsigned long startTime = millis();

    // API call to get playback state
    JsonDocument filter;
    filter["progress_ms"] = true;
    filter["is_playing"] = true;
    filter["item"]["name"] = true;
    filter["item"]["duration_ms"] = true;
    filter["item"]["artists"][0]["name"] = true;
    filter["device"]["name"] = true;
    
    response playback_resp = sp.current_playback_state(filter);

    unsigned long elapsed = millis() - startTime;
    Serial.printf("Spotify API calls took %lu ms\n", elapsed);

    if (playback_resp.status_code == 200) {
        JsonDocument& doc = playback_resp.reply;
        
        // Extract all data from single response
        String artist = "";
        String track = "";
        String deviceName = "";
        unsigned long progress = 0;
        unsigned long duration = 0;
        bool playing = false;
        
        if (doc["item"]["artists"][0]["name"]) {
            artist = doc["item"]["artists"][0]["name"].as<String>();
        }
        if (doc["item"]["name"]) {
            track = doc["item"]["name"].as<String>();
        }
        if (doc["device"]["name"]) {
            deviceName = doc["device"]["name"].as<String>();
        }
        if (doc["progress_ms"]) {
            progress = doc["progress_ms"].as<unsigned long>();
        }
        if (doc["item"]["duration_ms"]) {
            duration = doc["item"]["duration_ms"].as<unsigned long>();
        }
        if (doc["is_playing"]) {
            playing = doc["is_playing"].as<bool>();
        }

        // Update cached values with mutex for thread-safe handoff
        if (xSemaphoreTake(data_mutex, (TickType_t)10) == pdTRUE) {
            if (artist.length() > 0 && artist != cachedArtist) {
                cachedArtist = artist;
                nextArtist = artist;
                newArtist = true;
                Serial.println("Artist fetched: " + artist);
            }

            if (track.length() > 0 && track != cachedTrack) {
                cachedTrack = track;
                nextTrack = track;
                newTrack = true;
                Serial.println("Track fetched: " + track);
            }


            
            if (deviceName.length() > 0 && deviceName != cachedDeviceName) {
                cachedDeviceName = deviceName;
                nextDevice = deviceName;
                newDevice = true;
                Serial.println("Device fetched: " + deviceName);
            }
            
            // Cache progress/duration
            if (progress > 0 && duration > 0) {
                cachedProgress = progress;
                cachedDuration = duration;
                progressTimestamp = millis();
                isCurrentlyPlaying = playing;
                Serial.printf("Progress: %s / %s\n", 
                             formatTime(progress).c_str(), 
                             formatTime(duration).c_str());
            }

            xSemaphoreGive(data_mutex);
        }
    } else {
        Serial.printf("Spotify API error: %d\n", playback_resp.status_code);
    }
}

void updateTimeDisplay() {
    String currentTime = getCurrentTime();
    String currentDate = getCurrentDate();

    lv_label_set_text(ui_TIME, currentTime.c_str());
    lv_label_set_text(ui_DATE, currentDate.c_str());
}
//===================== button checks ========================
int get_current_volume() {
    JsonDocument filter;
    filter["device"]["volume_percent"] = true;
    
    response data = sp.current_playback_state(filter);
    
    // Just check if the reply contains the data
    if (!data.reply.isNull() && data.reply.containsKey("device")) {
        int volume = data.reply["device"]["volume_percent"].as<int>();
        return volume;
    }
    
    return -1; // Return -1 if unable to get volume
}

bool buttonFlag(void){
    // the goal of this function is to speed up the API CALL
    return requestPlay || requestNextTrack || requestPrevTrack || requestStop || increaseVolume || decreaseVolume || toggleMute;
}

void executeButtonAction(){
    bool doPlay = false;
    bool doNextTrack = false;
    bool doPrevTrack = false;
    bool doStop = false;
    bool doIncreaseVolume = false;
    bool doDecreaseVolume = false;
    bool doToggleMute = false;

    if (xSemaphoreTake(data_mutex, (TickType_t)10) == pdTRUE) {
        doPlay = requestPlay;
        doNextTrack = requestNextTrack;
        doPrevTrack = requestPrevTrack;
        doStop = requestStop;
        doIncreaseVolume = increaseVolume;
        doDecreaseVolume = decreaseVolume;
        doToggleMute = toggleMute;

        // Reset requests
        requestPlay = false;
        requestNextTrack = false;
        requestPrevTrack = false;
        requestStop = false;
        increaseVolume = false;
        decreaseVolume = false;
        toggleMute = false;

        xSemaphoreGive(data_mutex);
    }

    if(doPlay){
        Serial.println("Executing Play");
        sp.start_resume_playback();
    }
    if(doStop){
        Serial.println("Executing Stop");
        sp.pause_playback();
    }
    if(doNextTrack){
        Serial.println("Executing Next Track");
        sp.skip();
    }
    if(doPrevTrack){
        Serial.println("Executing Previous Track");
        sp.previous();
    }
    if(doIncreaseVolume){
        int currentVolume = get_current_volume();
        if(currentVolume >= 0 && currentVolume < 100){
            int newVolume = min(currentVolume + 2, 100);
            Serial.printf("Increasing volume from %d to %d\n", currentVolume, newVolume);
            sp.set_volume(newVolume);
        } else {
            Serial.println("Unable to get current volume for increase.");
        }
    }
    if(doDecreaseVolume){
        int currentVolume = get_current_volume();
        if(currentVolume >= 0 && currentVolume > 0){
            int newVolume = max(currentVolume - 2, 0);
            Serial.printf("Decreasing volume from %d to %d\n", currentVolume, newVolume);
            sp.set_volume(newVolume);
        } else {
            Serial.println("Unable to get current volume for decrease.");
        }
    }
    if(doToggleMute){
        int currentVolume = get_current_volume();
        if(currentVolume >= 0){
            if(currentVolume > 0){
                Serial.printf("Muting volume from %d to 0\n", currentVolume);
                sp.set_volume(0);
            } else {
                Serial.println("Unmuting volume to 20\n");
                sp.set_volume(20); // Unmute to a default level
            }
        } else {
            Serial.println("Unable to get current volume for mute toggle.");
        }
    }
}

void buttonChecks(){
    if(button1.justPressed()){
        Serial.println("Previous Track");
        if (xSemaphoreTake(data_mutex, (TickType_t)10) == pdTRUE) {
            requestPrevTrack = true;
            xSemaphoreGive(data_mutex);
        }
    }
    if(button2.justPressed()){
        Serial.println("Play");
        if (xSemaphoreTake(data_mutex, (TickType_t)10) == pdTRUE) {
            requestPlay = true;
            xSemaphoreGive(data_mutex);
        }
    }
    if(button3.justPressed()){
        Serial.println("Pause");
        if (xSemaphoreTake(data_mutex, (TickType_t)10) == pdTRUE) {
            requestStop = true;
            xSemaphoreGive(data_mutex);
        }
    }
    if(button4.justPressed()){
        Serial.println("Button 4: Next Track");
        if (xSemaphoreTake(data_mutex, (TickType_t)10) == pdTRUE) {
            requestNextTrack = true;
            xSemaphoreGive(data_mutex);
        }
    }
    if(rotary.is_clockwise()){
        Serial.println("Rotated Clockwise");
        if (xSemaphoreTake(data_mutex, (TickType_t)10) == pdTRUE) {
            increaseVolume = true;
            xSemaphoreGive(data_mutex);
        }
    }
    if(rotary.is_counterclockwise()){
        Serial.println("Rotated Counter-Clockwise");
        if (xSemaphoreTake(data_mutex, (TickType_t)10) == pdTRUE) {
            decreaseVolume = true;
            xSemaphoreGive(data_mutex);
        }
    }
    if(rotary.is_button_pressed()){
        Serial.println("Rotary Button Pressed");
        if (xSemaphoreTake(data_mutex, (TickType_t)10) == pdTRUE) {
            toggleMute = true;
            xSemaphoreGive(data_mutex);
        }
    }
}

// RTOS Task for Spotify API polling (runs on Core 1)
void spotifyTask(void *parameter) {
    for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
            if (buttonFlag()) {
                executeButtonAction();
            }
            else{
                updateSpotifyData();
            }
        }
        vTaskDelay(SPOTIFY_UPDATE_INTERVAL / portTICK_PERIOD_MS);
    }
}

//==================== SETUP AND LOOP ========================
void setup () {
    Serial.begin( 115200 );
    delay(1000);

    Serial.println("Configuring buttons...");
    Serial.println("Buttons configured.");
    

    String LVGL_Arduino = "LVGL v" + String(lv_version_major()) + "." +
                          String(lv_version_minor()) + "." + String(lv_version_patch());
    Serial.println(LVGL_Arduino);

    printMemory("Initial");

    data_mutex = xSemaphoreCreateMutex();
    if (data_mutex == NULL) {
        Serial.println("FATAL: Failed to create data mutex!");
        while (1) delay(1000);
    }

    // Allocate LVGL buffer
    buf = (lv_color_t*)malloc(SCREENBUFFER_SIZE_PIXELS * sizeof(lv_color_t));
    if (buf == nullptr) {
        Serial.println("FATAL: Failed to allocate LVGL buffer!");
        while(1) delay(1000);
    }
    Serial.printf("✓ LVGL buffer allocated: %d bytes\n",
                  SCREENBUFFER_SIZE_PIXELS * sizeof(lv_color_t));
    printMemory("After LVGL buffer");

    // WiFi connection
    Serial.print("Connecting to WiFi");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println(" Connected!");
    printMemory("After WiFi");

    // Initialize time and Spotify
    setupTime(); // Assumed function from esp_time.h
    sp.begin();
    printMemory("After Spotify init");

    // Initialize LVGL
    lv_init();

    // Initialize display
    tft.begin();
    tft.setRotation(0);

    static lv_disp_t* disp;
    disp = lv_display_create( screenWidth, screenHeight );
    lv_display_set_buffers( disp, buf, NULL, SCREENBUFFER_SIZE_PIXELS * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL );
    lv_display_set_flush_cb( disp, my_disp_flush );

    static lv_indev_t* indev;
    indev = lv_indev_create();
    lv_indev_set_type( indev, LV_INDEV_TYPE_POINTER );
    lv_indev_set_read_cb( indev, my_touchpad_read );

    lv_tick_set_cb( my_tick_get_cb );

    ui_init(); // Assumed function from ui.h
    lv_obj_set_style_text_font(ui_ARTIST_SONG, &NotoSansCJK_Regular_compressed_v2, 0);
    lv_obj_set_style_text_font(ui_ARTIST_NAME1, &NotoSansCJK_Regular_compressed_v2, 0);
    printMemory("After UI init");

    // Initial updates
    updateTimeDisplay();
    
    Serial.println("\n✓ Setup complete!");

    // Start Spotify background task on Core 1
    xTaskCreatePinnedToCore(
        spotifyTask,
        "SpotifyTask",
        16384, // Stack size
        NULL,  // Parameter
        5,     // Priority
        &spotifyTaskHandle,
        1     // Core to pin to
    );
    Serial.println("✓ Spotify task started on Core 1");
}

void loop () {
    // LVGL needs to be called frequently (main loop)
    lv_timer_handler();
    vTaskDelay(1);

    buttonChecks();

    // Thread-safe data handoff from Core 1 to Core 0 (where LVGL runs)
    bool applyArtist = false;
    bool applyTrack = false;
    bool applyDevice = false;
    String artistLocal;
    String trackLocal;
    String deviceLocal;

    if (xSemaphoreTake(data_mutex, (TickType_t)0) == pdTRUE) {
        if (newArtist) {
            artistLocal = nextArtist;
            newArtist = false;
            nextArtist = "";
            applyArtist = true;
        }
        if (newTrack) {
            trackLocal = nextTrack;
            newTrack = false;
            nextTrack = "";
            applyTrack = true;
        }
        if (newDevice) {
            deviceLocal = nextDevice;
            newDevice = false;
            nextDevice = "";
            applyDevice = true;
        }

        xSemaphoreGive(data_mutex);
    }

    if (applyArtist) {
        lv_label_set_text(ui_ARTIST_NAME1, artistLocal.c_str());
        Serial.println("Artist applied to LVGL: " + artistLocal);
    }

    if (applyTrack) {
        lv_label_set_text(ui_ARTIST_SONG, trackLocal.c_str());
        Serial.println("Track applied to LVGL: " + trackLocal);

        if (trackLocal != lastTrack) {
            lastTrack = trackLocal;
        }
    }
    if (applyDevice) {
        lv_label_set_text(ui_PLAYING_DEVICE, deviceLocal.c_str()); 
        Serial.println("Device applied to LVGL: " + deviceLocal);
    }

    // Time and progress update (every 1 second)
    unsigned long currentMillis = millis();
    if (currentMillis - lastTimeUpdate >= TIME_UPDATE_INTERVAL) {
        lastTimeUpdate = currentMillis;
        
        // Update clock display
        updateTimeDisplay();
        
        // Update playback progress (smooth interpolation)
        if (cachedDuration > 0) {
            unsigned long currentProgress = getEstimatedProgress();  // Use interpolated progress
            String progressStr = formatTime(currentProgress);
            String durationStr = formatTime(cachedDuration);
            
            // Update LVGL labels
            lv_label_set_text(ui_CURR_TIME, progressStr.c_str());
            lv_label_set_text(ui_END_TIME, durationStr.c_str());

            // Update progress bar
            int progressPercent = (currentProgress * 100) / cachedDuration;
            lv_bar_set_value(ui_Bar1, progressPercent, LV_ANIM_OFF);
        }
    }
}