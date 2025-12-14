//Display 64x64 album art as 128x128 using TJpg_Decoder
#include <Arduino.h>
#include <SpotifyEsp32.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <ui.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <TJpg_Decoder.h>
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
String album_art = "";
String cachedArtist = "";
String cachedTrack = "";
String cachedAlbumArtUrl = "";
String cachedDeviceName = "";

/* Thread-safe handoff to main core */
static String nextArtist = "";
static String nextTrack = "";
static String nextAlbumArtUrl = "";
static String nextDevice = "";
static bool newArtist = false;
static bool newTrack = false;
static bool newAlbumArt = false;
static bool newDevice = false;
SemaphoreHandle_t data_mutex = NULL;

// ==================== ALBUM ART GLOBALS ========================
static lv_img_dsc_t* albumArtImg = nullptr;
static uint16_t* rgb565_buffer = nullptr;
static const int IMG_WIDTH = 64;
static const int IMG_HEIGHT = 64;
static bool albumArtReady = false;
static int objWidth = 128;
static int objHeight = 128;

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
const unsigned long SPOTIFY_UPDATE_INTERVAL = 3000;
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
    Serial.printf("[%s] Free: %d, Largest: %d\n", 
                  location, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

// ==================== ALBUM ART FUNCTIONS ========================
// TJpg_Decoder callback to output decoded 64x64 image data to the RGB565 buffer
bool tjpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (!rgb565_buffer) return false;
    
    // Copy directly - no transformations needed
    for (int16_t j = 0; j < h; j++) {
        for (int16_t i = 0; i < w; i++) {
            int destX = x + i;
            int destY = y + j;
            if (destX < IMG_WIDTH && destY < IMG_HEIGHT) {
                rgb565_buffer[destY * IMG_WIDTH + destX] = bitmap[j * w + i];
            }
        }
    }
    return true;
}

// Downloads JPEG album art and decodes it into a 64x64 RGB565 buffer
void downloadAndStoreAlbumArt(String url) {
    if (url.length() == 0 || url == "Something went wrong" || !url.startsWith("http")) {
        Serial.println("Invalid album art URL: " + url);
        return;
    }
    
    // Store old buffer to free after new one is ready
    uint16_t* old_buffer = nullptr;
    if (xSemaphoreTake(data_mutex, (TickType_t)100) == pdTRUE) {
        old_buffer = rgb565_buffer;
        rgb565_buffer = nullptr;  // Clear reference temporarily
        albumArtReady = false;    // Prevent LVGL from using old buffer
        xSemaphoreGive(data_mutex);
    }
    
    Serial.println("Downloading album art from: " + url);
    printMemory("Before download");
    
    HTTPClient http;
    http.begin(url);
    http.setTimeout(5000);
    int httpCode = http.GET();
    
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("HTTP GET failed, code: %d\n", httpCode);
        http.end();
        if (old_buffer) free(old_buffer);
        return;
    }
    
    int len = http.getSize();
    Serial.printf("Image size: %d bytes\n", len);
    
    if (len <= 0 || len > 100000) {
        Serial.printf("Invalid image size: %d\n", len);
        http.end();
        if (old_buffer) free(old_buffer);
        return;
    }
    
    WiFiClient* stream = http.getStreamPtr();
    
    // Allocate buffer for compressed JPEG
    uint8_t* jpegBuffer = (uint8_t*)malloc(len);
    if (!jpegBuffer) {
        Serial.println("Failed to allocate JPEG buffer");
        http.end();
        if (old_buffer) free(old_buffer);
        return;
    }
    
    int bytesRead = 0;
    while (http.connected() && bytesRead < len) {
        size_t available = stream->available();
        if (available) {
            int toRead = min((int)available, len - bytesRead);
            int read = stream->readBytes(jpegBuffer + bytesRead, toRead);
            bytesRead += read;
        }
        delay(1);
    }
    http.end();
    
    Serial.printf("Downloaded %d bytes\n", bytesRead);
    printMemory("After download");
    
    // Validate JPEG header
    if (bytesRead < 4 || jpegBuffer[0] != 0xFF || jpegBuffer[1] != 0xD8) {
        Serial.println("ERROR: Invalid JPEG header");
        free(jpegBuffer);
        if (old_buffer) free(old_buffer);
        return;
    }
    
    // Allocate RGB565 buffer (64x64 = 4096 pixels)
    rgb565_buffer = (uint16_t*)heap_caps_malloc(IMG_WIDTH * IMG_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!rgb565_buffer) {
        Serial.println("Failed to allocate RGB565 buffer");
        free(jpegBuffer);
        if (old_buffer) free(old_buffer);
        return;
    }
    memset(rgb565_buffer, 0, IMG_WIDTH * IMG_HEIGHT * sizeof(uint16_t));
    
    // Decode JPEG to RGB565 buffer
    TJpgDec.setJpgScale(1); // Decode to actual size (64x64)
    TJpgDec.setSwapBytes(false);  // This matches your display's byte order
    TJpgDec.setCallback(tjpgOutput);
    
    JRESULT jres = TJpgDec.drawJpg(0, 0, jpegBuffer, bytesRead);
    
    if (jres != JDR_OK) {
        Serial.printf("ERROR: JPEG decode failed, result=%d\n", jres);
        free(jpegBuffer);
        free(rgb565_buffer);
        rgb565_buffer = nullptr;
        if (old_buffer) free(old_buffer);
        return;
    }
    
    free(jpegBuffer);
    
    Serial.println("✓ Album art decoded to RGB565 (64x64)");
    printMemory("After decode");
    
    // Mark as ready for LVGL and cleanup old buffer
    if (xSemaphoreTake(data_mutex, portMAX_DELAY) == pdTRUE) {
        albumArtReady = true;
        xSemaphoreGive(data_mutex);
    }
    
    // Safe to free old buffer now that new one is ready
    if (old_buffer) {
        free(old_buffer);
    }
}

// Applies the decoded 64x64 image to the LVGL image object and sets scale to 128x128
void applyAlbumArtToLVGL() {
    // Thread-safe access to buffer
    if (xSemaphoreTake(data_mutex, (TickType_t)10) == pdTRUE) {
        if (!rgb565_buffer) {
            Serial.println("No RGB565 buffer to apply");
            xSemaphoreGive(data_mutex);
            return;
        }
        
        // Create LVGL image descriptor if needed
        if (!albumArtImg) {
            albumArtImg = (lv_img_dsc_t*)malloc(sizeof(lv_img_dsc_t));
            if (!albumArtImg) {
                Serial.println("Failed to allocate LVGL image descriptor");
                xSemaphoreGive(data_mutex);
                return;
            }
            memset(albumArtImg, 0, sizeof(lv_img_dsc_t));
        }
        
        // Setup image descriptor for 64x64 RGB565 image
        albumArtImg->header.cf = LV_COLOR_FORMAT_RGB565;
        albumArtImg->header.w = IMG_WIDTH;
        albumArtImg->header.h = IMG_HEIGHT;
        albumArtImg->data = (const uint8_t*)rgb565_buffer;
        albumArtImg->data_size = IMG_WIDTH * IMG_HEIGHT * sizeof(uint16_t);
        
        // Set the source to the in-memory image
        lv_image_set_src(ui_Image1, (const void*)albumArtImg);
        
        // Scale 64x64 -> 128x128 (2x zoom = 512 in LVGL units where 256 = 1x)
        lv_img_set_zoom(ui_Image1, 512);
        
        // Reset the albumArtReady flag to prevent repeated applications
        albumArtReady = false;
        
        xSemaphoreGive(data_mutex);
        
        Serial.println("✓ Album art applied (64x64 -> 128x128)");
        printMemory("After LVGL apply");
    } else {
        Serial.println("Failed to acquire mutex for album art application");
    }
}

// ==================== END ALBUM ART FUNCTIONS ====================

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

    // Separate call for album art (using the specific 64x64 image size index)
    String albumArtUrl = sp.get_current_album_image_url(2);

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

            // Album art URL check
            if (albumArtUrl != cachedAlbumArtUrl && 
                albumArtUrl.length() > 0 && 
                albumArtUrl != "Something went wrong" &&
                albumArtUrl.startsWith("http")) {
                cachedAlbumArtUrl = albumArtUrl;
                nextAlbumArtUrl = albumArtUrl;
                newAlbumArt = true;
                Serial.println("Album art URL fetched: " + albumArtUrl);
            } else if (albumArtUrl == "Something went wrong" || !albumArtUrl.startsWith("http")) {
                Serial.println("Invalid album art URL from Spotify API");
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

        // Download album art OUTSIDE mutex - this is a blocking operation
        if (newAlbumArt && nextAlbumArtUrl.length() > 0) {
            // The downloadAndStoreAlbumArt function sets albumArtReady to true inside a mutex
            Serial.println("Downloading new album art...");
            downloadAndStoreAlbumArt(nextAlbumArtUrl);
            newAlbumArt = false; // Reset flag after download attempt
            Serial.println("Album art download attempt finished.");
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
        if(xSemaphoreTake(data_mutex, (TickType_t)10) == pdTRUE) {
            requestStop = true;
            xSemaphoreGive(data_mutex);
        }
    }
    if(button4.justPressed()){
        Serial.println("Next Track");
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
        1,     // Priority
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
    bool applyAlbumArt = false;
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
        if (albumArtReady) {
            albumArtReady = false;
            applyAlbumArt = true;
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
    if (applyAlbumArt) {
        applyAlbumArtToLVGL();
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