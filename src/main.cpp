#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <vector>
#include <deque>
#include "secrets.h"

// Constants
#define PORT 80
#define PORT 80
#define HEADER_HEIGHT 44
#define FOOTER_HEIGHT 60
#define MARGIN 10
#define MIN_FONT_SIZE 1
#define MAX_FONT_SIZE 6

// Globals
WebServer server(PORT);
uint8_t *imgBuffer = nullptr;
size_t imgReceivedLen = 0;
const size_t MAX_IMG_SIZE = 4 * 1024 * 1024; // 4MB Buffer (PLENTY for resized images)
String imageContentType = "";  // "map" if image is a map, empty for regular images

// Display State
// Stream Buffer
std::deque<String> streamBuffer;
const int MAX_STREAM_LINES = 100; // Increased buffer for smaller fonts

// TCP Server for Stream
WiFiServer streamServer(2323);
WiFiClient streamClient;

// Display State
enum DisplayMode { MODE_NONE, MODE_TEXT, MODE_IMAGE, MODE_STREAM, MODE_MQTT };
DisplayMode currentMode = MODE_NONE;
int currentRotation = 1;
bool uiVisible = true;

// MQTT State
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
String mqttBroker = "";
int mqttPort = 1883;
String mqttTopic = "";
String mqttUser = "";
String mqttPass = "";
bool mqttConnected = false;
String mqttLastMessage = "";

// Text Pagination State
String fullText = "";
std::vector<String> pages;
int currentPage = 0;
int currentTextSize = 2; // Default size
M5Canvas canvas(&M5.Display); // Global Sprite

// Power Management
const uint32_t TIMEOUT_MS = 180000; // 3 Minutes
uint32_t lastActivityTime = 0;

void resetActivity() {
    lastActivityTime = millis();
}

// Function Prototypes
void setupWiFi();
void handleRoot();
void handleText();
void handleStatus(); 
void handleScreenshot();
void handleStream();
void drawStream();
void handleImageUpload();
void updateAutoRotation();
void calculatePages();
void drawLayout();
void drawWelcome(bool sleeping = false); 
void handleTouch();
void resetActivity();
void handleMqtt();
void handleMqttLoop();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttReconnect();
void drawSleepOverlay();
void drawHeader(const char* modeName);

// =================================================================================
// Unified Header Drawing
// =================================================================================

void drawHeader(const char* modeName) {
    int w = M5.Display.width();
    
    // Fill header background
    M5.Display.fillRect(0, 0, w, HEADER_HEIGHT, TFT_LIGHTGREY);
    
    // Draw bottom separator line
    M5.Display.drawLine(0, HEADER_HEIGHT, w, HEADER_HEIGHT, TFT_BLACK);
    
    // Set text properties
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_BLACK);
    
    // Calculate vertical center with slight upward bias for visual balance
    int fontH = M5.Display.fontHeight();
    int yText = (HEADER_HEIGHT - fontH) / 2 + 1;  // +1 pushes text slightly down from pure center
    
    // === LEFT: IP Address ===
    M5.Display.setCursor(MARGIN, yText);
    M5.Display.print(WiFi.localIP());
    
    // === CENTER: Mode Name ===
    if (modeName && strlen(modeName) > 0) {
        int modeWidth = M5.Display.textWidth(modeName);
        M5.Display.setCursor((w - modeWidth) / 2, yText);
        M5.Display.print(modeName);
    }
    
    // === RIGHT: Battery Icon + Percentage ===
    int batLevel = M5.Power.getBatteryLevel();
    String batText = String(batLevel) + "%";
    int batTextWidth = M5.Display.textWidth(batText);
    
    // Battery icon dimensions
    int batIconW = 24;
    int batIconH = 12;
    int batTerminalW = 3;
    int batIconGap = 4;  // Gap between icon and text
    
    // Position from right edge: MARGIN | batText | gap | icon | terminal
    int batTextX = w - MARGIN - batTextWidth;
    int batIconX = batTextX - batIconGap - batIconW;
    int batIconY = (HEADER_HEIGHT - batIconH) / 2;
    
    // Draw battery outline (main body)
    M5.Display.drawRect(batIconX, batIconY, batIconW, batIconH, TFT_BLACK);
    
    // Draw battery terminal (the small nub on the right side of icon, left of text)
    int termX = batIconX + batIconW;
    int termY = batIconY + (batIconH - 6) / 2;  // Centered vertically, 6px tall
    M5.Display.fillRect(termX, termY, batTerminalW, 6, TFT_BLACK);
    
    // Draw battery fill level (inside the outline)
    int fillPadding = 2;
    int maxFillW = batIconW - (fillPadding * 2);
    int fillW = (batLevel * maxFillW) / 100;
    if (fillW > 0) {
        M5.Display.fillRect(batIconX + fillPadding, batIconY + fillPadding, 
                           fillW, batIconH - (fillPadding * 2), TFT_BLACK);
    }
    
    // Draw battery percentage text
    M5.Display.setCursor(batTextX, yText);
    M5.Display.print(batText);
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    
    // Display Setup
    M5.Display.setRotation(currentRotation);
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.setTextSize(2);
    
    // Allocate Image Buffer in PSRAM
    imgBuffer = (uint8_t*)heap_caps_malloc(MAX_IMG_SIZE, MALLOC_CAP_SPIRAM);
    
    setupWiFi();
    streamServer.begin(); // Start TCP

    // Server Routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus); 
    server.on("/api/screenshot", HTTP_GET, handleScreenshot);
    server.on("/api/text", HTTP_POST, handleText);
    server.on("/api/mqtt", HTTP_POST, handleMqtt);
    server.on("/api/image", HTTP_POST, 
        []() { server.send(200, "application/json", "{\"status\":\"ok\"}"); },
        handleImageUpload
    );
    
    // Collect custom headers for image content type detection
    const char* headerKeys[] = {"X-Content-Type"};
    server.collectHeaders(headerKeys, 1);

    server.begin();
    resetActivity();
    drawLayout(); // Draw Welcome Screen
}

void loop() {
    M5.update();
    server.handleClient();
    handleStream(); // Check TCP
    handleMqttLoop(); // Check MQTT
    updateAutoRotation(); 
    handleTouch();        
    
    // Timeout Check - always retain content on e-ink when sleeping
    if (millis() - lastActivityTime > TIMEOUT_MS) {
        if (currentMode != MODE_NONE) {
            // Content displayed: redraw without UI chrome + "Sleeping..." overlay
            drawSleepOverlay();
            delay(2000);
        } else {
            // No content: show welcome screen with "Sleeping..."
            drawWelcome(true);
            delay(2000);
        }
        M5.Power.powerOff();
    }
    
    delay(10);
}

// =================================================================================
// Pagination Logic
// =================================================================================

void calculatePages() {
    pages.clear();
    currentPage = 0; // Reset to start on reflow/new text
    
    if (fullText.length() == 0) return;

    M5.Display.setTextSize(currentTextSize);
    int screenW = M5.Display.width();
    int screenH = M5.Display.height();
    if (uiVisible) {
        screenH -= (HEADER_HEIGHT + FOOTER_HEIGHT + MARGIN);  // Account for extra padding below header
    }
    int lineHeight = M5.Display.fontHeight() * 1.2; 
    int maxLines = (screenH - (MARGIN * 2)) / lineHeight;
    int maxW = screenW - (MARGIN * 2);
    
    if (maxLines <= 0) maxLines = 1; // Safety

    String currentPageText = "";
    int currentLines = 0;
    
    int len = fullText.length();
    int lineStart = 0;
    
    while (lineStart < len) {
        int nextNewLine = fullText.indexOf('\n', lineStart);
        if (nextNewLine == -1) nextNewLine = len;
        
        String paragraph = fullText.substring(lineStart, nextNewLine);
        lineStart = nextNewLine + 1;
        
        int pIdx = 0;
        String currentLine = "";
        
        while (pIdx < paragraph.length()) {
            int nextSpace = paragraph.indexOf(' ', pIdx);
            if (nextSpace == -1) nextSpace = paragraph.length();
            
            String word = paragraph.substring(pIdx, nextSpace);
            if (nextSpace < paragraph.length()) word += " ";
            
            pIdx = nextSpace + 1;
            
            if (M5.Display.textWidth(currentLine + word) > maxW) {
                currentPageText += currentLine + "\n";
                currentLines++;
                currentLine = word; 
                
                if (currentLines >= maxLines) {
                    pages.push_back(currentPageText);
                    currentPageText = "";
                    currentLines = 0;
                }
            } else {
                currentLine += word;
            }
        }
        
        if (currentLine.length() > 0) {
            currentPageText += currentLine + "\n";
            currentLines++;
            
             if (currentLines >= maxLines) {
                 pages.push_back(currentPageText);
                 currentPageText = "";
                 currentLines = 0;
            }
        }
    }
    
    if (currentPageText.length() > 0) {
        pages.push_back(currentPageText);
    }
    
    if (pages.empty()) pages.push_back(""); 
}

// Helper to get JPEG dimensions
bool getJpegSize(uint8_t* data, size_t len, int* w, int* h) {
    if (len < 4) return false;
    // Check Magic
    if (data[0] != 0xFF || data[1] != 0xD8) return false;
    
    size_t pos = 2;
    while (pos < len) {
        if (data[pos] != 0xFF) return false; // Invalid marker
        uint8_t marker = data[pos+1];
        size_t lenChunk = (data[pos+2] << 8) | data[pos+3];
        
        // SOF0 (Baseline) or SOF2 (Progressive) -> 0xC0 .. 0xC2
        if (marker == 0xC0 || marker == 0xC2) {
            *h = (data[pos+5] << 8) | data[pos+6];
            *w = (data[pos+7] << 8) | data[pos+8];
            return true;
        }
        
        pos += 2 + lenChunk;
    }
    return false;
}

void drawWelcome(bool sleeping) {
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.fillScreen(TFT_WHITE);
    
    int w = M5.Display.width();
    int h = M5.Display.height();
    String ip = WiFi.localIP().toString();
    
    // Draw unified header (empty mode name for welcome screen)
    drawHeader("");
    
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    
    // Start content below header with padding
    int y = HEADER_HEIGHT + MARGIN + 20;
    
    // Title - large
    M5.Display.setTextSize(3);
    M5.Display.drawString("Paper Piper", w/2, y);
    y += 50;
    
    // Section spacing
    int sectionGap = 15;
    int cmdLineH = 32; // Line height for size 2
    
    // TEXT MODE
    M5.Display.setTextSize(3);
    M5.Display.drawString("-- TEXT --", w/2, y);
    y += 40;
    M5.Display.setTextSize(2);
    M5.Display.drawString("paper_cli.py text \"Hello\"", w/2, y);
    y += cmdLineH;
    M5.Display.drawString("curl -d 'msg' " + ip + "/api/text", w/2, y);
    y += cmdLineH + sectionGap;
    
    // IMAGE MODE
    M5.Display.setTextSize(3);
    M5.Display.drawString("-- IMAGE --", w/2, y);
    y += 40;
    M5.Display.setTextSize(2);
    M5.Display.drawString("paper_cli.py image < photo.jpg", w/2, y);
    y += cmdLineH + sectionGap;
    
    // STREAM MODE
    M5.Display.setTextSize(3);
    M5.Display.drawString("-- STREAM --", w/2, y);
    y += 40;
    M5.Display.setTextSize(2);
    M5.Display.drawString("nc " + ip + " 2323", w/2, y);
    y += cmdLineH;
    M5.Display.drawString("paper_cli.py stream", w/2, y);
    y += cmdLineH + sectionGap;
    
    // MAP MODE
    M5.Display.setTextSize(3);
    M5.Display.drawString("-- MAP --", w/2, y);
    y += 40;
    M5.Display.setTextSize(2);
    M5.Display.drawString("paper_cli.py map", w/2, y);
    y += cmdLineH;
    M5.Display.drawString("--location \"Berlin, Germany\"", w/2, y);
    y += cmdLineH + sectionGap;
    
    // MQTT MODE
    M5.Display.setTextSize(3);
    M5.Display.drawString("-- MQTT --", w/2, y);
    y += 40;
    M5.Display.setTextSize(2);
    M5.Display.drawString("paper_cli.py mqtt", w/2, y);
    y += cmdLineH;
    M5.Display.drawString("--broker host --topic sensors/#", w/2, y);
    
    if (sleeping) {
        M5.Display.setTextSize(3);
        M5.Display.setTextDatum(bottom_center);
        M5.Display.drawString("Sleeping...", w/2, h - 20);
    }

    M5.Display.startWrite(); M5.Display.endWrite();
}

// =================================================================================
// MQTT Functions
// =================================================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    resetActivity();
    
    // Convert payload to String
    String message = "";
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    
    mqttLastMessage = message;
    
    // Update display with new message (reuse text mode logic)
    fullText = message;
    fullText.replace("\r", "");
    fullText.replace("\\n", "\n");
    
    currentTextSize = 2; // Default size
    calculatePages();
    drawLayout();
}

void mqttReconnect() {
    if (!mqttClient.connected() && mqttBroker.length() > 0) {
        String clientId = "PaperS3-" + String(random(0xffff), HEX);
        
        bool connected = false;
        if (mqttUser.length() > 0) {
            connected = mqttClient.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str());
        } else {
            connected = mqttClient.connect(clientId.c_str());
        }
        
        if (connected) {
            mqttClient.subscribe(mqttTopic.c_str());
            mqttConnected = true;
        } else {
            mqttConnected = false;
        }
    }
}

void handleMqttLoop() {
    if (currentMode != MODE_MQTT) return;
    
    if (!mqttClient.connected()) {
        static uint32_t lastReconnectAttempt = 0;
        uint32_t now = millis();
        if (now - lastReconnectAttempt > 5000) { // Retry every 5 seconds
            lastReconnectAttempt = now;
            mqttReconnect();
        }
    } else {
        mqttClient.loop();
    }
}

void handleMqtt() {
    resetActivity();
    
    String body = "";
    if (server.hasArg("plain")) {
        body = server.arg("plain");
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);
    
    if (error) {
        server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    
    // Required: broker and topic
    if (!doc["broker"].is<const char*>() || !doc["topic"].is<const char*>()) {
        server.send(400, "application/json", "{\"error\":\"broker and topic required\"}");
        return;
    }
    
    mqttBroker = doc["broker"].as<String>();
    mqttTopic = doc["topic"].as<String>();
    mqttPort = doc["port"] | 1883;
    mqttUser = doc["username"] | "";
    mqttPass = doc["password"] | "";
    
    // Disconnect existing connection if any
    if (mqttClient.connected()) {
        mqttClient.disconnect();
    }
    
    // Configure MQTT client
    mqttClient.setServer(mqttBroker.c_str(), mqttPort);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(4096); // Larger buffer for bigger messages
    
    // Try to connect
    mqttReconnect();
    
    if (mqttClient.connected()) {
        currentMode = MODE_MQTT;
        
        // Show waiting message
        fullText = "MQTT Connected\n\nBroker: " + mqttBroker + "\nTopic: " + mqttTopic + "\n\nWaiting for messages...";
        calculatePages();
        drawLayout();
        
        JsonDocument resp;
        resp["status"] = "ok";
        resp["connected"] = true;
        resp["broker"] = mqttBroker;
        resp["topic"] = mqttTopic;
        
        String response;
        serializeJson(resp, response);
        server.send(200, "application/json", response);
    } else {
        server.send(500, "application/json", "{\"error\":\"failed to connect to MQTT broker\"}");
    }
}

// =================================================================================
// Sleep Overlay (content retained on e-ink when device powers off)
// =================================================================================

void drawSleepOverlay() {
    // Redraw content without UI elements, with proper padding
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    
    int w = M5.Display.width();
    int h = M5.Display.height();
    int sleepPadding = 20;  // Nice padding from top edge
    
    // Clear screen and redraw content without header/footer
    M5.Display.fillScreen(TFT_WHITE);
    
    // Redraw content based on mode
    if (currentMode == MODE_TEXT || currentMode == MODE_MQTT) {
        if (!pages.empty() && currentPage < pages.size()) {
            M5.Display.setTextColor(TFT_BLACK);
            M5.Display.setTextSize(currentTextSize);
            M5.Display.setCursor(MARGIN, sleepPadding);
            M5.Display.print(pages[currentPage]);
        }
    } else if (currentMode == MODE_IMAGE) {
        // Redraw image without header
        int imgW = 0, imgH = 0;
        bool valid = getJpegSize(imgBuffer, imgReceivedLen, &imgW, &imgH);
        if (valid && canvas.width() == imgW && canvas.height() == imgH) {
            int scrW = M5.Display.width();
            int scrH = M5.Display.height();
            float scaleX = (float)scrW / imgW;
            float scaleY = (float)scrH / imgH;
            float scale = (scaleX < scaleY) ? scaleX : scaleY;
            canvas.pushRotateZoom(&M5.Display, scrW / 2, scrH / 2, 0, scale, scale);
        }
    }
    
    // Draw sleep overlay at bottom
    int overlayHeight = 50;
    int overlayY = h - overlayHeight;
    
    M5.Display.fillRect(0, overlayY, w, overlayHeight, TFT_WHITE);
    M5.Display.drawLine(0, overlayY, w, overlayY, TFT_BLACK);
    
    // Draw "Sleeping..." centered in overlay
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("Sleeping...", w / 2, overlayY + (overlayHeight / 2));
    
    M5.Display.startWrite(); M5.Display.endWrite();
}

void drawLayout() {
    // Reset to Quality mode for standard views (Text/Image) to ensure correct rendering
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.fillScreen(TFT_WHITE);
    
    if (currentMode == MODE_NONE) {
        drawWelcome();
    }
    else if (currentMode == MODE_TEXT || currentMode == MODE_MQTT) {
        // Draw Text
        if (!pages.empty() && currentPage < pages.size()) {
            M5.Display.setTextColor(TFT_BLACK);
            M5.Display.setTextSize(currentTextSize);
            int yStart = MARGIN;
            if (uiVisible) yStart += HEADER_HEIGHT + MARGIN;  // Extra padding below header
            M5.Display.setCursor(MARGIN, yStart);
            M5.Display.print(pages[currentPage]);
        }
        
        // Draw UI elements
        if (uiVisible) {
            // --- HEADER ---
            const char* modeName = (currentMode == MODE_MQTT) ? "MQTT" : "TEXT";
            drawHeader(modeName);

            // --- FOOTER ---
            int yFoot = M5.Display.height() - FOOTER_HEIGHT;
            M5.Display.drawLine(0, yFoot, M5.Display.width(), yFoot, TFT_BLACK);
            
            // Buttons: |<<  <   Page   >   >>|
            int w = M5.Display.width();
            int btnW = w / 5;
            int yText = yFoot + (FOOTER_HEIGHT/2) - (M5.Display.fontHeight()/2);
            
            // Button 1: Start (|<<)
            M5.Display.drawRect(0, yFoot, btnW, FOOTER_HEIGHT, TFT_LIGHTGREY);
            M5.Display.setCursor(btnW*0.5 - 15, yText); M5.Display.print("|<<");

            // Button 2: Prev (<)
            M5.Display.drawRect(btnW, yFoot, btnW, FOOTER_HEIGHT, TFT_LIGHTGREY);
            M5.Display.setCursor(btnW*1.5 - 10, yText); M5.Display.print("<");

            // Center: Page Info
            String pageInfo = String(currentPage + 1) + "/" + String(pages.size());
            int wPage = M5.Display.textWidth(pageInfo);
            M5.Display.setCursor((w/2) - (wPage/2), yText);
            M5.Display.print(pageInfo);

            // Button 3: Next (>)
            M5.Display.drawRect(btnW*3, yFoot, btnW, FOOTER_HEIGHT, TFT_LIGHTGREY);
            M5.Display.setCursor(btnW*3.5 - 10, yText); M5.Display.print(">");

            // Button 4: End (>>|)
            M5.Display.drawRect(btnW*4, yFoot, btnW, FOOTER_HEIGHT, TFT_LIGHTGREY);
            M5.Display.setCursor(btnW*4.5 - 15, yText); M5.Display.print(">>|");
        }
    } 
    else if (currentMode == MODE_IMAGE) {
        int imgW = 0, imgH = 0;
        bool valid = getJpegSize(imgBuffer, imgReceivedLen, &imgW, &imgH);
        
        if (valid) {
             // Create Sprite matching Image Size (16-bit color for memory saving)
            if (canvas.width() != imgW || canvas.height() != imgH) {
                canvas.deleteSprite();
                canvas.setColorDepth(16);
                void* ptr = canvas.createSprite(imgW, imgH);
                if (!ptr) {
                    // OOM Fallback -> Draw direct
                    M5.Display.drawJpg(imgBuffer, imgReceivedLen, 0, 0);
                    return;
                }
            }
            
            // 1. Decode JPEG to Sprite (Native Resolution)
            canvas.drawJpg(imgBuffer, imgReceivedLen, 0, 0);
            
            // 2. Calculate Scaling to Fit Screen
            int scrW = M5.Display.width();
            int scrH = M5.Display.height();
            
            float scaleX = (float)scrW / imgW;
            float scaleY = (float)scrH / imgH;
            // Use "cover" scaling - fill entire screen, may crop edges
            float scale = (scaleX > scaleY) ? scaleX : scaleY;
            
            // 3. Render Scaled Sprite to Display
            // pushRotateZoom renders *centered* at the destination coordinate (scrW/2, scrH/2).
            canvas.pushRotateZoom(&M5.Display, scrW / 2, scrH / 2, 0, scale, scale);
            
        } else {
             // Fallback for unknown formats
             bool success = M5.Display.drawJpg(imgBuffer, imgReceivedLen, 0, 0);
             if (!success) M5.Display.drawPng(imgBuffer, imgReceivedLen, 0, 0);
        }
        
        if (uiVisible) {
            // Display "MAP" if image is a map, otherwise "IMAGE"
            const char* headerName = (imageContentType == "map") ? "MAP" : "IMAGE";
            drawHeader(headerName);
        }
    }

    M5.Display.startWrite(); M5.Display.endWrite();
}

void handleTouch() {
    if (currentMode == MODE_NONE) return; // Allow TEXT and IMAGE
    
    if (M5.Touch.getCount() > 0) {
        resetActivity();
        auto t = M5.Touch.getDetail(0);
        
        if ((currentMode == MODE_TEXT || currentMode == MODE_STREAM || currentMode == MODE_MQTT) && t.wasFlicked()) {
            // Determine direction
            int dx = t.distanceX();
            int dy = t.distanceY();
            
            bool changed = false;
            
            // Prefer axis with larger movement
            if (abs(dx) > abs(dy)) {
                // Horizontal Swipe (Page Nav) - For Text and MQTT Mode
                if (currentMode == MODE_TEXT || currentMode == MODE_MQTT) {
                    if (dx < 0) { 
                        // Swipe Left (Right to Left) -> Next Page
                        if (currentPage < pages.size() - 1) {
                             currentPage++;
                             changed = true;
                        }
                    } else {
                        // Swipe Right (Left to Right) -> Prev Page
                        if (currentPage > 0) {
                            currentPage--;
                            changed = true;
                        }
                    }
                }
            } else {
                // Vertical Swipe (Font Size) - For Text, MQTT AND Stream
                if (dy < 0) {
                    // Swipe Up -> Increase Font
                    if (currentTextSize < MAX_FONT_SIZE) {
                        currentTextSize++;
                        if (currentMode == MODE_TEXT || currentMode == MODE_MQTT) calculatePages(); // Reflow Text
                        changed = true;
                    }
                } else {
                    // Swipe Down -> Decrease Font
                    if (currentTextSize > MIN_FONT_SIZE) {
                        currentTextSize--;
                        if (currentMode == MODE_TEXT || currentMode == MODE_MQTT) calculatePages(); // Reflow Text
                        changed = true;
                    }
                }
            }
            
            if (changed) {
                if (currentMode == MODE_STREAM) drawStream();
                else drawLayout();
                delay(100); 
            }
        } else if (t.wasClicked()) {
            // Check for Button Taps (if UI visible)
            int y = t.y;
            int x = t.x;
            bool btnHit = false;

            if (uiVisible && y > M5.Display.height() - FOOTER_HEIGHT) {
                // Footer Hit - Check Buttons
                int w = M5.Display.width();
                int btnW = w / 5;
                
                if (x < btnW) { // |<<
                    currentPage = 0; btnHit = true;
                } else if (x < btnW * 2) { // <
                    if (currentPage > 0) { currentPage--; btnHit = true; }
                } else if (x > btnW * 3 && x < btnW * 4) { // >
                    if (currentPage < pages.size() - 1) { currentPage++; btnHit = true; }
                } else if (x > btnW * 4) { // >>|
                    currentPage = pages.size() - 1; btnHit = true;
                }
            }

            if (btnHit) {
                drawLayout();
                delay(100);
            } else {
                // Not a button hit -> Toggle UI
                uiVisible = !uiVisible;
                if (currentMode == MODE_TEXT || currentMode == MODE_MQTT) calculatePages();
                
                if (currentMode == MODE_STREAM) drawStream();
                else drawLayout();
                
                delay(200);
            }
        }
    }
}

// =================================================================================
// Handlers (Mostly same)
// =================================================================================

void handleText() {
    resetActivity();
    fullText = "";
    currentTextSize = 2; // Default

    // 1. Check for "text" form field (curl -d "text=hello")
    if (server.hasArg("text")) {
        fullText = server.arg("text");
        if (server.hasArg("size")) currentTextSize = server.arg("size").toInt();
    }
    // 2. Check for "plain" body (JSON or Raw)
    else if (server.hasArg("plain")) {
        String body = server.arg("plain");
        
        // Check if body looks like JSON
        if (body.startsWith("{")) {
            // Use streaming filter to handle large JSON payloads efficiently
            // Only parse "text" and "size" fields, ignore everything else
            struct JsonFilter {
                bool text = false;
                bool size = false;
            };
            
            // Manual extraction for large payloads to avoid memory issues
            // Find "text" field value
            int textStart = body.indexOf("\"text\"");
            if (textStart >= 0) {
                // Find the colon after "text"
                int colonPos = body.indexOf(':', textStart);
                if (colonPos >= 0) {
                    // Skip whitespace and find opening quote
                    int valueStart = colonPos + 1;
                    while (valueStart < body.length() && (body[valueStart] == ' ' || body[valueStart] == '\t')) {
                        valueStart++;
                    }
                    
                    if (body[valueStart] == '"') {
                        valueStart++; // Skip opening quote
                        // Find closing quote (handle escaped quotes)
                        int valueEnd = valueStart;
                        while (valueEnd < body.length()) {
                            if (body[valueEnd] == '"' && body[valueEnd-1] != '\\') {
                                break;
                            }
                            valueEnd++;
                        }
                        fullText = body.substring(valueStart, valueEnd);
                        // Unescape common sequences
                        fullText.replace("\\n", "\n");
                        fullText.replace("\\t", "\t");
                        fullText.replace("\\\"", "\"");
                        fullText.replace("\\\\", "\\");
                    }
                }
            }
            
            // Find "size" field value (small, can use simple parsing)
            int sizeStart = body.indexOf("\"size\"");
            if (sizeStart >= 0) {
                int colonPos = body.indexOf(':', sizeStart);
                if (colonPos >= 0) {
                    int valueStart = colonPos + 1;
                    while (valueStart < body.length() && (body[valueStart] == ' ' || body[valueStart] == '\t')) {
                        valueStart++;
                    }
                    // Read digits
                    String sizeStr = "";
                    while (valueStart < body.length() && isdigit(body[valueStart])) {
                        sizeStr += body[valueStart];
                        valueStart++;
                    }
                    if (sizeStr.length() > 0) {
                        currentTextSize = sizeStr.toInt();
                    }
                }
            }
        } else {
            // Not JSON, treat as raw text
            fullText = body;
        }
    }
    // 3. Fallback: Parse 'curl -d "hello"' (treated as key "hello" with empty value)
    else if (server.args() > 0) {
        fullText = server.argName(0);
    }
    else {
        server.send(400, "application/json", "{\"error\":\"no body, 'text' field, or args\"}");
        return;
    }

    if (fullText.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"empty text\"}");
        return;
    }
    
    // Sanitization
    fullText.replace("\r", "");      // Remove CR
    fullText.replace("\\n", "\n");   // Expand literal \n (common in shell piping)

    currentMode = MODE_TEXT;
    calculatePages();
    drawLayout();

    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void updateAutoRotation() {
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    
    int newRot = -1;
    float threshold = 0.5;

    if (ay > threshold) newRot = 1;
    else if (ay < -threshold) newRot = 3;
    else if (ax > threshold) newRot = 0;
    else if (ax < -threshold) newRot = 2;

    if (newRot >= 0 && newRot != currentRotation) {
        delay(100); 
        float ax2, ay2, az2;
        M5.Imu.getAccel(&ax2, &ay2, &az2);
        
        bool stable = false;
        if (newRot == 1 && ay2 > threshold) stable = true;
        if (newRot == 3 && ay2 < -threshold) stable = true;
        if (newRot == 0 && ax2 > threshold) stable = true;
        if (newRot == 2 && ax2 < -threshold) stable = true;

        if (stable) {
            currentRotation = newRot;
            M5.Display.setRotation(currentRotation);
            
            if (currentMode == MODE_TEXT || currentMode == MODE_MQTT) {
                calculatePages();
            }
            drawLayout();
            delay(300);
        }
    }
}

void setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    // Minimal feedback during boot
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }
    
    // drawLayout not called here because mode is NONE, loop will handle updates if needed or text api called
}

void handleRoot() {
    server.send(200, "text/plain", "PaperS3 Remote Display with Gestures");
}

String getModeString() {
    switch(currentMode) {
        case MODE_TEXT: return "TEXT";
        case MODE_IMAGE: return "IMAGE";
        case MODE_STREAM: return "STREAM";
        case MODE_MQTT: return "MQTT";
        default: return "NONE";
    }
}

void handleStatus() {
    JsonDocument doc;
    doc["mode"] = getModeString();
    doc["heap_free"] = esp_get_free_heap_size();
    doc["heap_min"] = esp_get_minimum_free_heap_size();
    doc["spiram_free"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["screen_width"] = M5.Display.width();
    doc["screen_height"] = M5.Display.height();
    doc["rotation"] = currentRotation;
    
    // MQTT Status
    if (currentMode == MODE_MQTT) {
        doc["mqtt_connected"] = mqttClient.connected();
        doc["mqtt_topic"] = mqttTopic;
        doc["mqtt_broker"] = mqttBroker;
    }
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// BMP Header Helpers
uint16_t read16(WiFiClient &c) {
    return 0; // Not used
}

void write32(WiFiClient &c, uint32_t v) {
    c.write((v) & 0xFF);
    c.write((v >> 8) & 0xFF);
    c.write((v >> 16) & 0xFF);
    c.write((v >> 24) & 0xFF);
}

void write16(WiFiClient &c, uint16_t v) {
    c.write((v) & 0xFF);
    c.write((v >> 8) & 0xFF);
}

void handleScreenshot() {
    WiFiClient client = server.client();
    if (!client) return;
    
    // BMP Format details
    int w = M5.Display.width();
    int h = M5.Display.height();
    uint32_t imageOffset = 54;
    uint32_t fileSize = imageOffset + (w * h * 3); // 24-bit RGB
    
    // Send HTTP Header
    String response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: image/bmp\r\n";
    response += "Connection: close\r\n\r\n";
    client.print(response);
    
    // BMP Header (14 bytes)
    client.write('B'); client.write('M');
    write32(client, fileSize);
    write32(client, 0); // Reserved
    write32(client, imageOffset);
    
    // DIB Header (40 bytes)
    write32(client, 40); // Header size
    write32(client, w);
    write32(client, -h); // Top-down
    write16(client, 1);  // Planes
    write16(client, 24); // Bits per pixel
    write32(client, 0);  // Compression (BI_RGB)
    write32(client, 0);  // Image size (ignored for BI_RGB)
    write32(client, 0);  // X pixels/meter
    write32(client, 0);  // Y pixels/meter
    write32(client, 0);  // Colors used
    write32(client, 0);  // Important colors

    // Pixel Data
    // Buffer one line at a time to improve speed
    uint8_t* lineBuffer = (uint8_t*)malloc(w * 3);
    if (!lineBuffer) {
        client.stop();
        return;
    }
    
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint16_t color = M5.Display.readPixel(x, y);
            
            // RGB565 to RGB888
            uint8_t r = (color >> 11) * 255 / 31;
            uint8_t g = ((color >> 5) & 0x3F) * 255 / 63;
            uint8_t b = (color & 0x1F) * 255 / 31;
            
            // BMP is BGR
            lineBuffer[x*3] = b;
            lineBuffer[x*3+1] = g;
            lineBuffer[x*3+2] = r;
        }
        client.write(lineBuffer, w * 3);
    }
    
    free(lineBuffer);
    client.stop();
}

void handleImageUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        resetActivity();
        imgReceivedLen = 0;
        // Check for X-Content-Type header to identify maps vs regular images
        if (server.hasHeader("X-Content-Type")) {
            imageContentType = server.header("X-Content-Type");
        } else {
            imageContentType = "";  // Regular image
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (imgReceivedLen + upload.currentSize < MAX_IMG_SIZE) {
            memcpy(imgBuffer + imgReceivedLen, upload.buf, upload.currentSize);
            imgReceivedLen += upload.currentSize;
        }
        resetActivity(); // Keep alive (for slow uploads)
    } else if (upload.status == UPLOAD_FILE_END) {
        resetActivity();
        currentMode = MODE_IMAGE;
        drawLayout();
    }
}

void handleStream() {
    if (streamServer.hasClient()) {
        if (!streamClient || !streamClient.connected()) {
            if (streamClient) streamClient.stop();
            streamClient = streamServer.available();
            currentMode = MODE_STREAM;
            streamBuffer.clear();
            fullText = ""; // Clear text mode buffer to save RAM? (Optional)
            resetActivity();
            M5.Display.fillScreen(TFT_WHITE); // Clear on new connection
        }
    }

    // Debounce State
    static uint32_t lastDrawTime = 0;
    static bool streamDirty = false;

    if (streamClient && streamClient.connected()) {
        if (streamClient.available()) {
            static String lineBuffer = ""; // Persist partial lines
            
            while (streamClient.available()) {
                char c = streamClient.read();
                resetActivity(); // Keep alive
                
                if (c == '\r') continue; // Ignore CR
                
                if (c == '\n') {
                    // Line Complete
                    if (lineBuffer.length() > 0) {
                        streamBuffer.push_back(lineBuffer);
                        if (streamBuffer.size() > MAX_STREAM_LINES) {
                            streamBuffer.pop_front();
                        }
                        lineBuffer = "";
                        streamDirty = true;
                    }
                } else {
                    lineBuffer += c;
                }
            }
        }
    }
    
    // Periodic Redraw (Debounced) or if forced by other events
    if (streamDirty && (millis() - lastDrawTime > 500)) {
        drawStream();
        lastDrawTime = millis();
        streamDirty = false;
    }
}

void drawStream() {
    // fast mode for stream to avoid flashing
    M5.Display.setEpdMode(epd_mode_t::epd_fast); 
    
    int yStart = MARGIN;
    if (uiVisible) yStart += HEADER_HEIGHT + MARGIN;  // Consistent padding below header
    
    int scrH = M5.Display.height();
    int scrW = M5.Display.width();
    
    // Clear Text Area
    M5.Display.fillRect(0, yStart, scrW, scrH - yStart, TFT_WHITE);
    
    M5.Display.setTextSize(currentTextSize); 
    M5.Display.setTextColor(TFT_BLACK);
    
    int lineHeight = M5.Display.fontHeight() * 1.1;
    int maxW = scrW - (MARGIN * 2);
    
    // Bottom-Up Rendering
    int currentY = scrH - MARGIN; 
    
    for (int i = streamBuffer.size() - 1; i >= 0; i--) {
        String line = streamBuffer[i];
        
        int pxWidth = M5.Display.textWidth(line);
        int numLines = (pxWidth + maxW - 1) / maxW; 
        if (numLines < 1) numLines = 1;
        
        int blockHeight = numLines * lineHeight;
        currentY -= blockHeight;
        
        if (currentY < yStart) break;
        
        M5.Display.setCursor(MARGIN, currentY);
        M5.Display.println(line);
    }
    
    // Draw Header (if visible)
    if (uiVisible) {
        drawHeader("STREAM");
    }
    
    M5.Display.startWrite(); M5.Display.endWrite();
}
