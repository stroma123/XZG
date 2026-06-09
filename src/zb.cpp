#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <FS.h>
#include <LittleFS.h>
#include <ETH.h>
#include <HTTPClient.h>

// #include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <CCTools.h>

#include "config.h"
#include "web.h"
#include "log.h"
#include "etc.h"
#include "zb.h"
#include "mqtt.h"
#include "const/keys.h"
#include "main.h"

extern struct SysVarsStruct vars;
extern struct ThisConfigStruct hwConfig;
extern BrdConfigStruct brdConfigs[BOARD_CFG_CNT];

extern struct SystemConfigStruct systemCfg;
extern struct NetworkConfigStruct networkCfg;
extern struct VpnConfigStruct vpnCfg;
extern struct MqttConfigStruct mqttCfg;

extern LEDControl ledControl;

extern const char *tempFile;

size_t lastSize = 0;

String tag_ZB = "[RCP]";

extern CCTools CCTool;

bool zbFwCheck()
{
    if (systemCfg.zbRole == COORDINATOR)
    {
        const int maxAttempts = 3;
        for (int attempt = 0; attempt < maxAttempts; attempt++)
        {
            LOGD("Try: %d", attempt + 1);
            delay(500 * (attempt * 2));
            if (CCTool.checkFirmwareVersion())
            {
                printLogMsg(tag_ZB + " FW: " + String(CCTool.chip.fwRev));
                // if (systemCfg.zbRole == UNDEFINED)
                // {
                //     systemCfg.zbRole = COORDINATOR;
                //     saveSystemConfig(systemCfg);
                // }
                return true;
            }
            else
            {
                CCTool.restart();
            }
        }
    }
    else if((systemCfg.zbSaveinfo) and (strlen(systemCfg.zbFw) > 5))
    {
        /* only coordinator has FW check function in case of router/openthread FW - return
           version stored in system config at programming time */
        printLogMsg(tag_ZB + " FW: " + String(systemCfg.zbFw));
        return true;
    }
    printLogMsg(tag_ZB + " FW: Unknown! Check serial speed!");
    return false;
}

void zbHwCheck()
{
    ledControl.modeLED.mode = LED_BLINK_1Hz;

    if (CCTool.detectChipInfo())
    {
        printLogMsg(tag_ZB + " Chip: " + CCTool.chip.hwRev);
        printLogMsg(tag_ZB + " IEEE: " + CCTool.chip.ieee);
        LOGI("modeCfg %s", String((CCTool.chip.modeCfg), HEX).c_str());
        LOGI("bslCfg %s", String((CCTool.chip.bslCfg), HEX).c_str());
        printLogMsg(tag_ZB + " Flash size: " + String(CCTool.chip.flashSize / 1024) + " KB");

        vars.hwZigbeeIs = true;
        ledControl.modeLED.mode = LED_OFF;
    }
    else
    {
        printLogMsg(tag_ZB + " No Zigbee chip!");
        vars.hwZigbeeIs = false;
        ledControl.modeLED.mode = LED_BLINK_3Hz;
    }
    CCTool.restart();
}

bool zbLedToggle()
{
    if (CCTool.ledToggle())
    {
        if (CCTool.ledState == 1)
        {
            printLogMsg("[RCP] LED toggle ON");
            // vars.zbLedState = 1;
        }
        else
        {
            printLogMsg("[RCP] LED toggle OFF");
            // vars.zbLedState = 0;
        }
        return true;
    }
    else
    {
        printLogMsg("[RCP] LED toggle ERROR");
        return false;
    }
}

bool zigbeeErase()
{
    if (CCTool.eraseFlash())
    {
        LOGD("magic");
        return true;
    }
    return false;
}
void nvPrgs(const String &inputMsg)
{

    const uint8_t eventLen = 30;
    String msg = inputMsg;
    if (msg.length() > 25)
    {
        msg = msg.substring(0, 25);
    }
    sendEvent(tagZB_NV_prgs, eventLen, msg);
    LOGD("%s", msg.c_str());
}

void zbEraseNV(void *pvParameters)
{
    vars.zbFlashing = true;
    CCTool.nvram_reset(nvPrgs);
    logClear();
    printLogMsg("NVRAM erase finish! Restart CC2652!");
    vars.zbFlashing = false;
    vTaskDelete(NULL);
}

void flashZbUrl(String url)
{
    // zbFwCheck();
    ledControl.modeLED.mode = LED_BLINK_3Hz;
    vars.zbFlashing = true;

    // checkDNS();
    //delay(250);

    Serial2.updateBaudRate(500000);

    freeHeapPrint();
    delay(250);

    // Conditional WiFi shutdown:
    // If both Ethernet and WiFi are connected, disconnect WiFi to free up memory.
    // If only WiFi or only Ethernet is connected, keep the connection open.
    if (networkCfg.wifiEnable)
    {
        if (vars.connectedEther)
        {
            // Connect to both Ethernet and WiFi, then stop WiFi to free up memory.
            LOGD("Both Ethernet and WiFi connected, stopping WiFi to free memory");
            stopWifi();
        }
        else
        {
            // Only a WiFi connection is required; keep WiFi running so that the web server and HTTPClient can function properly.
            LOGD("Only WiFi connected, keeping WiFi for web server and HTTPClient");
        }
    }

    if (mqttCfg.enable)
    {
        mqttDisconnectCleanup();
    }
    
    // Clean up memory
    heap_caps_free(NULL);
  
    freeHeapPrint();
    delay(250);

    float last_percent = 0;

    const uint8_t eventLen = 11;
    // Use a static buffer to store the progress string to avoid frequently creating String objects.
    static char percentStr[16];

    auto progressShow = [&last_percent](float percent) mutable
    {
        // Update every 5% to reduce the frequency of sendEvent calls.
        if ((percent - last_percent) > 5.0 || percent < 0.1 || percent == 100)
        {
            // char buffer[100];
            // snprintf(buffer, sizeof(buffer), "Flash progress: %.2f%%", percent);
            // printLogMsg(String(buffer));
            LOGI("%.2f%%", percent);
            // Use snprintf instead of the String constructor to reduce memory allocation.
            snprintf(percentStr, sizeof(percentStr), "%.2f", percent);
            sendEvent(tagZB_FW_prgs, eventLen, String(percentStr));
            last_percent = percent;
        }
    };

    printLogMsg("Start Zigbee flashing");
    sendEvent(tagZB_FW_info, eventLen, String("start"));

    // https://raw.githubusercontent.com/xyzroe/XZG/zb_fws/ti/coordinator/CC1352P7_coordinator_20240316.bin
    //  CCTool.enterBSL();
    int key = url.indexOf("?b=");

    String clear_url = url.substring(0, key);

    // printLogMsg("Clear from " + clear_url);
    String baud_str = url.substring(key + 3, url.length());
    // printLogMsg("Baud " + baud_str);
    systemCfg.serialSpeed = baud_str.toInt();

    // Optimize log output and reduce string concatenation.
    char logMsg[256];
    snprintf(logMsg, sizeof(logMsg), "ZB flash %s @ %d", clear_url.c_str(), systemCfg.serialSpeed);
    printLogMsg(logMsg);

    sendEvent(tagZB_FW_file, eventLen, String(clear_url));

    if (eraseWriteZbUrl(clear_url.c_str(), progressShow, CCTool))
    {
        sendEvent(tagZB_FW_info, eventLen, String("finish"));
        printLogMsg("Flashed successfully");
        Serial2.updateBaudRate(systemCfg.serialSpeed);

        int lineIndex = clear_url.lastIndexOf("_");
        int binIndex = clear_url.lastIndexOf(".bin");
        int lastSlashIndex = clear_url.lastIndexOf("/");

        if (lineIndex > -1 && binIndex > -1 && lastSlashIndex > -1)
        {
            String zbFw = clear_url.substring(lineIndex + 1, binIndex);
            // LOGI("1 %s", zbFw);

            strncpy(systemCfg.zbFw, zbFw.c_str(), sizeof(systemCfg.zbFw) - 1);
            zbFw = "";

            int i = 0;

            int preLastSlash = -1;
            // LOGI("2 %s", String(url.c_str()));

            while (clear_url.indexOf("/", i) > -1 && i < lastSlashIndex)
            {
                int result = clear_url.indexOf("/", i);
                // LOGI("r %d", result);
                if (result > -1)
                {
                    i = result + 1;
                    if (result < lastSlashIndex)
                    {
                        preLastSlash = result;
                        // LOGI("pl %d", preLastSlash);
                    }
                }
                // LOGI("l %d", lastSlashIndex);
                // delay(500);
            }

            // LOGD("%s %s", String(preLastSlash), String(lastSlashIndex));
            String zbRole = clear_url.substring(preLastSlash + 1, lastSlashIndex);
            LOGI("%s", zbRole.c_str());

            if (zbRole.indexOf("coordinator") > -1)
            {
                systemCfg.zbRole = COORDINATOR;
            }
            else if (zbRole.indexOf("router") > -1)
            {
                systemCfg.zbRole = ROUTER;
            }
            else if (zbRole.indexOf("thread") > -1)
            {
                systemCfg.zbRole = OPENTHREAD;
            }
            else
            {
                systemCfg.zbRole = UNDEFINED;
            }
            zbRole = "";
            // systemCfg.zbSaveinfo = false;
            saveSystemConfig(systemCfg);
        }
        else
        {
            LOGW("URL error");
        }
        // if (systemCfg.zbRole == COORDINATOR)
        // {
        //     zbFwCheck();
        //     // zbLedToggle();
        //     // delay(1000);
        //     // zbLedToggle();
        // }
        sendEvent(tagZB_FW_file, eventLen, String(systemCfg.zbFw));
        delay(500);
        restartDevice();
    }
    else
    {
        Serial2.updateBaudRate(systemCfg.serialSpeed);
        printLogMsg("Failed to flash Zigbee");

        // Send error event
        sendEvent(tagZB_FW_err, eventLen, String("Failed!"));
        
        // Immediately address the web server issue to ensure events are sent to the front end.
        webServerHandleClient();
        delay(100);  // Give the event some time to send.
        webServerHandleClient();
    }
    ledControl.modeLED.mode = LED_OFF;
    vars.zbFlashing = false;
    if (mqttCfg.enable && !vars.mqttConn)
    {
        connectToMqtt();
    }
}

/*void printBufferAsHex(const byte *buffer, size_t length)
{
    const char *TAG = "BufferHex";
    char hexStr[CCTool.TRANSFER_SIZE + 10];
    std::string hexOutput;

    for (size_t i = 0; i < length; ++i)
    {
        if (buffer[i] != 255)
        {
            sprintf(hexStr, "%02X ", buffer[i]);
            hexOutput += hexStr;
        }
    }

    LOGD("Buffer content:\n%s", hexOutput.c_str());
}*/

// Use static buffers to avoid allocating large buffers on the stack.
static byte flashBuffer[CCTools::TRANSFER_SIZE];

bool eraseWriteZbUrl(const char *url, std::function<void(float)> progressShow, CCTools &CCTool)
{
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    // Optimize HTTPClient configuration to reduce memory usage.
    http.setTimeout(30000); // 30-second timeout
    http.setReuse(false);   // Do not reuse connections, release resources

    int loadedSize = 0;
    int totalSize = 0;
    const int maxRetries = 3;   //Reduce from 7 to 3, reducing the number of retries.
    int retryCount = 0;
    const int retryDelay = 500;
    bool isSuccess = false;
    bool flashErased = false;  // Flag: Has the Flash memory been erased? Avoid repeated erasing.

    while (retryCount < maxRetries && !isSuccess)
    {
        // Check memory; if memory is insufficient, fail prematurely.
        size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        if (freeHeap < 10*1024)  // If less than 10KB of memory is available, fail immediately.
        {
            LOGW("Critical low memory during flash: %d bytes, aborting", freeHeap);
            printLogMsg("OTA failed: Insufficient memory");
            sendEvent(tagZB_FW_err, 11, String("Insufficient memory"));
            webServerHandleClient();
            delay(100);
            webServerHandleClient();
            http.end();
            CCTool.restart();
            return false; // Return early and do not retry
        }

        if (!dnsLookup(url))
        {
            retryCount++;
            // If it fails early (the first two times), return quickly.
            if (retryCount >= 2)
            {
                LOGW("DNS lookup failed after %d retries, aborting", retryCount);
                printLogMsg("OTA failed: DNS lookup failed");
                sendEvent(tagZB_FW_err, 11, String("DNS lookup failed"));
                webServerHandleClient();
                delay(100);
                webServerHandleClient();
                http.end();
                CCTool.restart();
                return false; // Return early and do not retry
            }
            delay(retryDelay);
            // Ensure the web server can handle events so that the front end can receive error messages promptly.
            webServerHandleClient();
            continue;
        }

        // Erase only the first time or when you need to start over, to avoid repeated erasing.
        if (loadedSize == 0 && !flashErased)
        {
            CCTool.eraseFlash();
            sendEvent("ZB_FW_info", 11, String("erase"));
            printLogMsg("Erase completed!");
            flashErased = true;
        }

        http.begin(url);
        http.addHeader("Content-Type", "application/octet-stream");

        if (loadedSize > 0)
        {
            // Use character arrays instead of Strings to reduce memory allocation.
            char rangeHeader[32];
            snprintf(rangeHeader, sizeof(rangeHeader), "bytes=%d-", loadedSize);
            http.addHeader("Range", rangeHeader);
        }

        int httpCode = http.GET();

        if (httpCode != HTTP_CODE_OK && httpCode != HTTP_CODE_PARTIAL_CONTENT)
        {
            char buffer[100];
            snprintf(buffer, sizeof(buffer), "Failed to download file, HTTP code: %d\n", httpCode);
            printLogMsg(buffer);

            http.end();
            retryCount++;
            
            // If it fails early (the first two times), return quickly.
            if (retryCount >= 2)
            {
                LOGW("HTTP error persists after %d retries, aborting", retryCount);
                printLogMsg("OTA failed: Network error");
                sendEvent(tagZB_FW_err, 11, String("Network error"));
                webServerHandleClient();
                delay(100);
                webServerHandleClient();
                CCTool.restart();
                return false; // Return early
            }
   
            delay(retryDelay);
            // Ensure the web server can handle events so that the front end can receive error messages promptly.
            webServerHandleClient();
            continue;
        }

        if (totalSize == 0)
        {
            totalSize = http.getSize();
        }

        if (loadedSize == 0)
        {
            if (!CCTool.beginFlash(BEGIN_ZB_ADDR, totalSize))
            {
                http.end();
                printLogMsg("Error initializing flash process");
                retryCount++;
                
                // If it fails early (the first two times), return quickly.
                if (retryCount >= 2)
                {
                    LOGW("Flash init failed after %d retries, aborting", retryCount);
                    printLogMsg("OTA failed: Flash init error");
                    sendEvent(tagZB_FW_err, 11, String("Flash init error"));
                    webServerHandleClient();
                    delay(100);
                    webServerHandleClient();
                    CCTool.restart();
                    return false; // Return early
                }
  
                delay(retryDelay);
                // Ensure the web server can handle events so that the front end can receive error messages promptly.
                webServerHandleClient();
                continue;
            }
            printLogMsg("Begin flash");
        }

        // Use a static buffer instead of allocating on the stack.
        WiFiClient *stream = http.getStreamPtr();
        static int callCounter = 0;
        static int webServerCallCounter = 0;

        while (http.connected() && loadedSize < totalSize)
        {
            size_t size = stream->available();
            if (size > 0)
            {
                int c = stream->readBytes(flashBuffer, std::min(size, sizeof(flashBuffer)));
                if (c <= 0)
                {
                    printLogMsg("Failed to read data from stream");
                    break;
                }

                if (!CCTool.processFlash(flashBuffer, c))
                {
                    printLogMsg("Failed to process flash data");
                    loadedSize = 0;
                    retryCount++;
                    
                    // If it fails early (the first two times), return quickly.
                    if (retryCount >= 2)
                    {
                        LOGW("Flash process failed after %d retries, aborting", retryCount);
                        printLogMsg("OTA failed: Flash process error");
                        sendEvent(tagZB_FW_err, 11, String("Flash process error"));
                        webServerHandleClient();
                        delay(100);
                        webServerHandleClient();
                        stream->stop();
                        http.end();
                        CCTool.restart();
                        return false; // Return early
                    }
    
                    delay(retryDelay);
                    // Ensure the web server can handle events so that the front end can receive error messages promptly.
                    webServerHandleClient();
                    break;
                }

                loadedSize += c;
                float percent = static_cast<float>(loadedSize) / totalSize * 100.0f;
                progressShow(percent);
            }

            esp_task_wdt_reset(); // Reset watchdog
            delay(10); // Yield to the WiFi stack

            callCounter++;
            webServerCallCounter++;

            // Check memory more frequently (approximately 1 second every 100 loops).
            if (callCounter % 100 == 0)
            {
                size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
                
                // If less than 10KB of memory is available, fail immediately.
                if (freeHeap < 10*1024)
                {
                    LOGW("Critical low memory during flash: %d bytes, aborting", freeHeap);
                    printLogMsg("OTA failed: Insufficient memory");
                    sendEvent(tagZB_FW_err, 11, String("Insufficient memory"));
                    webServerHandleClient();
                    delay(100);
                    webServerHandleClient();
                    stream->stop();
                    http.end();
                    CCTool.restart();
                    return false; // Returning early and not continuing.
                }
                else if (freeHeap < 20*1024)  // Log a warning if less than 20KB of memory is available.
                {
                    LOGW("Low memory during flash: %d bytes", freeHeap);
                }
                freeHeapPrint();
                delay(10);
                callCounter = 0;
            }

            // Periodically call webServerHandleClient to ensure the web server can handle events (approximately 0.5 seconds every 50 loops).
            if (webServerCallCounter % 50 == 0)
            {
                webServerHandleClient();
                webServerCallCounter = 0;
            }
        }

        stream->stop();
        http.end();

        if (loadedSize >= totalSize)
        {
            isSuccess = true;
        }
    }

    // If it fails, immediately send an error event and notify the user.
    if (!isSuccess)
    {
        // Send an error event to notify the user of the failure.
        printLogMsg("OTA failed after all retries");
        sendEvent(tagZB_FW_err, 11, String("OTA failed"));
        // Ensure the web server can handle events so that the front end can receive error messages promptly.
        webServerHandleClient();
        delay(100);
        webServerHandleClient();
    }

    http.end();
    CCTool.restart();
    return isSuccess;
}

/*
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

// Функция для извлечения хоста из URL


// Функция для получения размера файла
int getFileSize(const char* url) {

    LOGD("URL %s", url);
    const int httpsPort = 443;
    String host = getHostFromUrl(url);

    // Создание сокета
    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        Serial.println("Failed to create socket");
        return -1;
    }

    // Настройка адреса сервера
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(httpsPort);

    struct hostent* server = gethostbyname(host.c_str());
    if (server == NULL) {
        Serial.println("Failed to resolve hostname");
        close(sockfd);
        return -1;
    }

    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    // Установка соединения
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        Serial.println("Connection failed");
        close(sockfd);
        return -1;
    }

    // Отправка HTTP-запроса
    String request = "HEAD " + String(url) + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (send(sockfd, request.c_str(), request.length(), 0) < 0) {
        Serial.println("Failed to send request");
        close(sockfd);
        return -1;
    }

    // Чтение ответа
    char buffer[1024];
    int bytes;
    int contentLength = -1;
    while ((bytes = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = 0;
        String response(buffer);

        // Поиск заголовка Content-Length
        int index = response.indexOf("Content-Length: ");
        if (index != -1) {
            int endIndex = response.indexOf("\r\n", index);
            if (endIndex != -1) {
                String lengthStr = response.substring(index + 16, endIndex);
                contentLength = lengthStr.toInt();
                break;
            }
        }
    }

    close(sockfd);
    return contentLength;
}

// Функция для загрузки файла
bool downloadFile(const char* url, std::function<void(float)> progressShow, CCTools &CCTool, int &loadedSize, int &totalSize) {
    const int httpsPort = 443;
    String host = getHostFromUrl(url);

    // Создание сокета
    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        Serial.println("Failed to create socket");
        return false;
    }

    // Настройка адреса сервера
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(httpsPort);

    struct hostent* server = gethostbyname(host.c_str());
    if (server == NULL) {
        Serial.println("Failed to resolve hostname");
        close(sockfd);
        return false;
    }

    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    // Установка соединения
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        Serial.println("Connection failed");
        close(sockfd);
        return false;
    }

    // Отправка HTTP-запроса
    String request = "GET " + String(url) + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n";
    if (loadedSize > 0) {
        request += "Range: bytes=" + String(loadedSize) + "-\r\n";
    }
    request += "\r\n";

    if (send(sockfd, request.c_str(), request.length(), 0) < 0) {
        Serial.println("Failed to send request");
        close(sockfd);
        return false;
    }

    // Чтение ответа
    char buffer[1024];
    int bytes;
    bool headersEnded = false;
    while ((bytes = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = 0;
        String response(buffer);

        if (!headersEnded) {
            int headerEnd = response.indexOf("\r\n\r\n");
            if (headerEnd != -1) {
                headersEnded = true;
                response = response.substring(headerEnd + 4);
            } else {
                continue;
            }
        }

        if (headersEnded) {
            int c = response.length();
            if (!CCTool.processFlash((byte*)response.c_str(), c)) {
                loadedSize = 0;
                close(sockfd);
                return false;
            }
            loadedSize += c;
            float percent = static_cast<float>(loadedSize) / totalSize * 100.0f;
            progressShow(percent);
        }
    }

    close(sockfd);
    return true;
}

bool eraseWriteZbUrl(const char *url, std::function<void(float)> progressShow, CCTools &CCTool)
{
    int loadedSize = 0;
    int totalSize = 0;
    int maxRetries = 7;
    int retryCount = 0;
    int retryDelay = 500;
    bool isSuccess = false;

    while (retryCount < maxRetries && !isSuccess)
    {
        if (loadedSize == 0)
        {
            CCTool.eraseFlash();
            sendEvent("ZB_FW_info", 11, String("erase"));
            printLogMsg("Erase completed!");
        }

        if (loadedSize == 0)
        {
            // Получение размера файла
            totalSize = getFileSize(url);
            if (totalSize <= 0) {
                printLogMsg("Failed to get file size");
                retryCount++;
                delay(retryDelay);
                continue;
            }

            if (!CCTool.beginFlash(BEGIN_ZB_ADDR, totalSize))
            {
                printLogMsg("Error initializing flash process");
                retryCount++;
                delay(retryDelay);
                continue;
            }
            printLogMsg("Begin flash");
        }

        if (downloadFile(url, progressShow, CCTool, loadedSize, totalSize)) {
            isSuccess = true;
        } else {
            retryCount++;
            delay(retryDelay);
        }
    }

    CCTool.restart();
    return isSuccess;
}
*/

#include <FS.h>
#include <LittleFS.h>

String extractChipModelFromFilename(const char *filePath)
{
    String fileName = String(filePath);
    
    // Extract filenames from path
    int lastSlash = fileName.lastIndexOf('/');
    if (lastSlash >= 0)
    {
        fileName = fileName.substring(lastSlash + 1);
    }
    
    // Convert to uppercase for comparison
    fileName.toUpperCase();
    
    // Check common chip model patterns
    // Both CC1352P7 and CC2652P7 are mapped to CC2652P7.
    if (fileName.indexOf("CC2652P7") >= 0 || fileName.indexOf("CC1352P7") >= 0)
    {
        return "CC2652P7";
    }
    // Both CC1352P2 and CC2652P2 are mapped to CC2652P2.
    else if (fileName.indexOf("CC2652P2") >= 0 || fileName.indexOf("CC1352P2") >= 0)
    {
        return "CC2652P2";
    }
    else if (fileName.indexOf("CC2652RB") >= 0)
    {
        return "CC2652RB";
    }
    else if (fileName.indexOf("CC2652") >= 0 || fileName.indexOf("CC1352") >= 0)
    {
        // If only CC2652 or CC1352 is available, try a more precise match.
        // Check for clear markings for P2 or P7.
        if (fileName.indexOf("P2") >= 0)
        {
            return "CC2652P2";
        }
        else if (fileName.indexOf("P7") >= 0)
        {
            return "CC2652P7";
        }
        // If no explicit identifier is specified, return the general CC2652.
        return "CC2652";
    }
    
    // If it cannot be extracted from the filename, return an empty string.
    return "";
}

bool validateChipModel(const char *filePath, const char *currentChipModel)
{
    String firmwareChipModel = extractChipModelFromFilename(filePath);
    
    // Check if the current chip model cannot be obtained.
    bool currentChipUnknown = (!currentChipModel || strlen(currentChipModel) == 0);
    
    // Scenario 1: If the chip model cannot be detected, but the firmware file name is clear, upgrade is allowed.
    if (currentChipUnknown && firmwareChipModel.length() > 0)
    {
        LOGW("Cannot detect current chip model, but firmware filename contains chip model: %s, allowing upgrade", firmwareChipModel.c_str());
        return true;
    }
    
    // Scenario 2: If the firmware filename does not contain the chip model, the upgrade is not allowed.
    if (firmwareChipModel.length() == 0)
    {
        if (currentChipUnknown)
        {
            LOGW("Cannot detect current chip model and cannot extract chip model from filename, blocking upgrade");
        }
        else
        {
            LOGW("Cannot extract chip model from filename, blocking upgrade");
        }
        return false;
    }
    
    // Scenario 3: If both have values, perform a strict matching check.
    String currentChip = String(currentChipModel);
    currentChip.toUpperCase();
    
    // Exact match (primary check)）
    if (firmwareChipModel == currentChip)
    {
        return true;
    }
    
    // Handling variants of CC2652P2 (launchpad/other)
    // The current chip is a variant of the CC2652P2, and the firmware is CC2652P2.
    if (currentChip.indexOf("CC2652P2") >= 0 && firmwareChipModel == "CC2652P2")
    {
        return true;
    }
    // The current chip is CC2652P2, and the firmware is a variant of CC2652P2.
    if (currentChip == "CC2652P2" && firmwareChipModel.indexOf("CC2652P2") >= 0)
    {
        return true;
    }
    
    // Handling variants of CC2652P7 (if any).
    if (currentChip.indexOf("CC2652P7") >= 0 && firmwareChipModel == "CC2652P7")
    {
        return true;
    }
    if (currentChip == "CC2652P7" && firmwareChipModel.indexOf("CC2652P7") >= 0)
    {
        return true;
    }
    
    // The general CC2652 firmware can be used with P2 or P7 (backward compatible).
    // However, the firmware for P2 and P7 is not interchangeable.
    if (firmwareChipModel == "CC2652" && (currentChip.indexOf("CC2652P2") >= 0 || currentChip.indexOf("CC2652P7") >= 0))
    {
        LOGW("Firmware chip model is generic CC2652, allowing upgrade");
        return true;
    }
    
    // In all other cases, upgrading between P2 and P7 is not allowed (including upgrading each other).
    return false;
}

bool eraseWriteZbFile(const char *filePath, std::function<void(float)> progressShow, CCTools &CCTool)
{
    File file = LittleFS.open(filePath, "r");
    if (!file)
    {
        char buffer[100];
        snprintf(buffer, sizeof(buffer), "Failed to open file: %s", filePath);
        printLogMsg(buffer);
        return false;
    }

    int totalSize = file.size();
    if (totalSize <= 0)
    {
        printLogMsg("Invalid file size");
        file.close();
        return false;
    }

    // Erase Flash
    if (!CCTool.eraseFlash())
    {
        printLogMsg("Failed to erase flash");
        file.close();
        return false;
    }
    sendEvent(tagZB_FW_info, 11, String("erase"));
    printLogMsg("Erase completed!");

    // Initialize Flash writing
    if (!CCTool.beginFlash(BEGIN_ZB_ADDR, totalSize))
    {
        printLogMsg("Error initializing flash process");
        file.close();
        return false;
    }
    printLogMsg("Begin flash");

    // Use static buffers to avoid allocating large buffers on the stack.
    static byte flashBuffer[CCTools::TRANSFER_SIZE];
    int loadedSize = 0;
    static int callCounter = 0;
    static int webServerCallCounter = 0;

    while (file.available() && loadedSize < totalSize)
    {
        // Check memory
        size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        if (freeHeap < 10000)
        {
            LOGW("Critical low memory during flash: %d bytes, aborting", freeHeap);
            printLogMsg("Flash failed: Insufficient memory");
            sendEvent(tagZB_FW_err, 11, String("Insufficient memory"));
            webServerHandleClient();
            delay(100);
            webServerHandleClient();
            file.close();
            CCTool.restart();
            return false;
        }

        size_t size = file.available();
        int c = file.readBytes(reinterpret_cast<char *>(flashBuffer), std::min(size, sizeof(flashBuffer)));
        
        if (c <= 0)
        {
            printLogMsg("Failed to read data from file");
            break;
        }

        if (!CCTool.processFlash(flashBuffer, c))
        {
            printLogMsg("Failed to process flash data");
            file.close();
            CCTool.restart();
            return false;
        }

        loadedSize += c;
        float percent = static_cast<float>(loadedSize) / totalSize * 100.0f;
        progressShow(percent);

        esp_task_wdt_reset(); // Reset watchdog
        delay(10); // Yield to allow other processes

        callCounter++;
        webServerCallCounter++;

        // Regularly check memory (every 100 cycles).
        if (callCounter % 100 == 0)
        {
            freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            if (freeHeap < 10000)
            {
                LOGW("Critical low memory during flash: %d bytes, aborting", freeHeap);
                printLogMsg("Flash failed: Insufficient memory");
                sendEvent(tagZB_FW_err, 11, String("Insufficient memory"));
                webServerHandleClient();
                delay(100);
                webServerHandleClient();
                file.close();
                CCTool.restart();
                return false;
            }
            callCounter = 0;
        }

        // Periodically call webServerHandleClient (every 50 times in a loop).
        if (webServerCallCounter % 50 == 0)
        {
            webServerHandleClient();
            webServerCallCounter = 0;
        }
    }

    file.close();
    CCTool.restart();
    
    if (loadedSize >= totalSize)
    {
        return true;
    }
    else
    {
        printLogMsg("Flash incomplete");
        return false;
    }
}

void flashZbFile(const char *filePath, const char *originalFilename)
{
    // Verify chip model - Verify using original filename
    const char *filenameForValidation = (originalFilename && strlen(originalFilename) > 0) ? originalFilename : filePath;
    const char *currentChipModel = CCTool.chip.hwRev.c_str();
    if (strlen(currentChipModel) == 0)
    {
        // If the current chip model is unknown, use the saved model.
        currentChipModel = systemCfg.zbHw;
    }
    
    LOGD("Validating chip model - filePath: %s, originalFilename: %s, currentChip: %s", 
         filePath, originalFilename ? originalFilename : "null", currentChipModel);
    
    if (!validateChipModel(filenameForValidation, currentChipModel))
    {
        String firmwareChipModel = extractChipModelFromFilename(filenameForValidation);
        char errorMsg[256];
        snprintf(errorMsg, sizeof(errorMsg), 
                 "Chip model mismatch! Current: %s, Firmware: %s", 
                 currentChipModel, firmwareChipModel.c_str());
        
        printLogMsg(errorMsg);
        LOGD("Chip model validation failed: %s", errorMsg);
        
        // First, send an error status message to let the front end know that the upgrade failed.
        sendEvent(tagZB_FW_info, 11, String("error"));
        webServerHandleClient();
        delay(50);
        
        // Send a formatted error message, which the frontend can parse and display.
        String errorEvent = String("Chip model mismatch: Current=") + String(currentChipModel) + 
                          String(", Firmware=") + firmwareChipModel;
        sendEvent(tagZB_FW_err, 11, errorEvent);
        
        // The web server is processed multiple times to ensure that events are sent to the front end.
        for (int i = 0; i < 10; i++)
        {
            webServerHandleClient();
            delay(50);
        }
        
        vars.zbFlashing = false;
        ledControl.modeLED.mode = LED_OFF;
        return;
    }
    
    ledControl.modeLED.mode = LED_BLINK_3Hz;
    vars.zbFlashing = true;

    Serial2.updateBaudRate(500000);

    freeHeapPrint();
    delay(250);
    
    // Conditional stop WiFi
    if (networkCfg.wifiEnable)
    {
        if (vars.connectedEther)
        {
            LOGD("Both Ethernet and WiFi connected, stopping WiFi to free memory");
            stopWifi();
        }
        else
        {
            LOGD("Only WiFi connected, keeping WiFi for web server");
        }
    }
    
    if (mqttCfg.enable)
    {
        mqttDisconnectCleanup();
    }
    
    // Clean up memory
    heap_caps_free(NULL);
    
    freeHeapPrint();
    delay(250);

    float last_percent = 0;

    const uint8_t eventLen = 11;
    static char percentStr[16];

    auto progressShow = [&last_percent](float percent) mutable
    {
        // Update every 5% to reduce the frequency of sendEvent calls.
        if ((percent - last_percent) > 5.0 || percent < 0.1 || percent == 100)
        {
            LOGI("%.2f%%", percent);
            snprintf(percentStr, sizeof(percentStr), "%.2f", percent);
            sendEvent(tagZB_FW_prgs, eventLen, String(percentStr));
            last_percent = percent;
        }
    };

    printLogMsg("Start Zigbee flashing from file");
    sendEvent(tagZB_FW_info, eventLen, String("start"));

    char logMsg[256];
    snprintf(logMsg, sizeof(logMsg), "ZB flash from file: %s", filePath);
    printLogMsg(logMsg);

    sendEvent(tagZB_FW_file, eventLen, String(filePath));

    if (eraseWriteZbFile(filePath, progressShow, CCTool))
    {
        sendEvent(tagZB_FW_info, eventLen, String("finish"));
        printLogMsg("Flashed successfully");
        
        // Extract device type and set baud rate from filename
        String filenameForExtraction = (originalFilename && strlen(originalFilename) > 0) ? String(originalFilename) : String(filePath);
        String filenameUpper = filenameForExtraction;
        filenameUpper.toUpperCase();  // Convert to uppercase for case-insensitive matching
        
        // Extract device type (case-sensitive)
        bool deviceTypeDetected = false;
        
        if (filenameUpper.indexOf("COORDINATOR") >= 0)
        {
            systemCfg.zbRole = COORDINATOR;
            systemCfg.serialSpeed = 115200;  // coordinator default value is 115200
            deviceTypeDetected = true;
            LOGD("Detected device type: COORDINATOR, baud rate: 115200");
        }
        else if (filenameUpper.indexOf("ROUTER") >= 0)
        {
            systemCfg.zbRole = ROUTER;
            systemCfg.serialSpeed = 115200;  // Router default 115200
            deviceTypeDetected = true;
            LOGD("Detected device type: ROUTER, baud rate: 115200");
        }
        else if (filenameUpper.indexOf("THREAD") >= 0 || filenameUpper.indexOf("_RCP_") >= 0 || filenameUpper.indexOf("_RCP.") >= 0)
        {
            // Both thread and rcp are mapped to OpenHREAD, with a baud rate of 460800.
            systemCfg.zbRole = OPENTHREAD;
            systemCfg.serialSpeed = 460800;  // The thread/rcp baud rate is 460800.
            deviceTypeDetected = true;
            LOGD("Detected device type: OPENTHREAD (thread/rcp), baud rate: 460800");
        }
        
        if (!deviceTypeDetected)
        {
            // If the device type cannot be recognized, keep the current settings.
            LOGW("Cannot detect device type from filename: %s, keeping current settings", filenameForExtraction.c_str());
        }
        
        Serial2.updateBaudRate(systemCfg.serialSpeed);

        // Attempt to extract firmware version information (the date in the last part) from the filename
        // Use originalFilename instead of filePath because filePath is a fixed path /fw.hex
        // const char *filenameForExtraction = (originalFilename && strlen(originalFilename) > 0) ? originalFilename : filePath;
        String filePathStr = filenameForExtraction;
        int lineIndex = filePathStr.lastIndexOf("_");
        int binIndex = filePathStr.lastIndexOf(".bin");
        int hexIndex = filePathStr.lastIndexOf(".hex");
        int extIndex = (binIndex > hexIndex) ? binIndex : hexIndex;

        if (lineIndex > -1 && extIndex > -1 && extIndex > lineIndex)
        {
            String zbFw = filePathStr.substring(lineIndex + 1, extIndex);
            
            // Verify that it is in date format (8-digit number, such as 20250403).
            bool isValidDate = (zbFw.length() == 8);
            if (isValidDate)
            {
                for (int i = 0; i < 8; i++)
                {
                    if (!isdigit(zbFw.charAt(i)))
                    {
                        isValidDate = false;
                        break;
                    }
                }
            }
            
            if (isValidDate)
            {
                strncpy(systemCfg.zbFw, zbFw.c_str(), sizeof(systemCfg.zbFw) - 1);
                systemCfg.zbFw[sizeof(systemCfg.zbFw) - 1] = '\0';
                LOGW("Extracted firmware version from filename: %s", systemCfg.zbFw);
            }
            else
            {
                // If not in date format, version is empty.
                systemCfg.zbFw[0] = '\0';
                LOGW("No valid date found in filename, version set to empty");
            }
        }
        else
        {
            // If it cannot be extracted from the filename, the version will be empty.
            systemCfg.zbFw[0] = '\0';
            LOGW("Cannot extract version from filename, version set to empty");
        }

        // Save configuration (including device type and baud rate).
        // systemCfg.zbSaveinfo = false;
        saveSystemConfig(systemCfg);

        // if (systemCfg.zbRole == COORDINATOR)
        // {
        //     zbFwCheck();
        // }
        sendEvent(tagZB_FW_file, eventLen, String(systemCfg.zbFw));
        
        // Delete firmware files to free up file system space
        if (LittleFS.exists(filePath))
        {
            if (LittleFS.remove(filePath))
            {
                LOGD("Firmware file deleted: %s", filePath);
                printLogMsg("Firmware file deleted");
            }
            else
            {
                LOGW("Failed to delete firmware file: %s", filePath);
            }
        }
        
        delay(500);
        restartDevice();
    }
    else
    {
        Serial2.updateBaudRate(systemCfg.serialSpeed);
        printLogMsg("Failed to flash Zigbee");
        
        // Send error event
        sendEvent(tagZB_FW_err, eventLen, String("Failed!"));
        
        // Delete files if the upgrade fails to avoid wasting space.
        if (LittleFS.exists(filePath))
        {
            if (LittleFS.remove(filePath))
            {
                LOGD("Firmware file deleted after failure: %s", filePath);
            }
        }
        
        // Immediately address the web server issue to ensure events are sent to the front end.
        webServerHandleClient();
        delay(100);
        webServerHandleClient();
    }
    ledControl.modeLED.mode = LED_OFF;
    vars.zbFlashing = false;
    if (mqttCfg.enable && !vars.mqttConn)
    {
        connectToMqtt();
    }
}
