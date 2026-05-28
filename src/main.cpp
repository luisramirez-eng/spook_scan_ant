#include <Arduino.h>
#include <WiFi.h>
#include <Network.h>
#include <NetworkClient.h>
#include <NimBLEDevice.h>
#include <time.h>

// ================= WIFI MULTI =================
struct WiFiCredential {
    const char* ssid;
    const char* password;
};

WiFiCredential wifiList[] = {
    {"Direccion_2.4", "14809414*"},
    {"IZZI-B2C2", "JDZMRJZLR2NL"},
    {"Lobo", "SanPau2025"},
};

const int wifiCount = sizeof(wifiList) / sizeof(wifiList[0]);

// ================= ENDPOINT =================
const char* host = "6.tcp.ngrok.io";
const int port = 22896;

// ================= CONFIG =================
#define BATCH_SIZE 1
#define QUEUE_SIZE 10
#define EMBEDDED_MAC_START_BYTE 20
#define EMBEDDED_MAC_END_BYTE   25
#define EMBEDDED_MAC_LENGTH     6

// ================= DATA =================
struct Beacon {
    char mac[18];
    char embeddedMac[18];
    char name[32];
    int rssi;
    char uuid[20];
    char manufacturer[100];
    unsigned long timestamp;
};

// ================= QUEUE =================

NetworkClient client;

NimBLEScan* pScan;

Beacon buffer[BATCH_SIZE];
char frameBuffer[512];

QueueHandle_t beaconQueue;

// ================= TIME SYNC BASE =================
unsigned long bootMillis = 0;
time_t bootEpoch = 0;

// ================= MAC EMBEDDED =================

String extractEmbeddedMac(
    const std::string& manufacturerData
) {

    // Validar tamaño mínimo
    if (
        manufacturerData.length() <
        (EMBEDDED_MAC_END_BYTE + 1)
    ) {

        Serial.println(
            "Manufacturer data demasiado corto"
        );

        return "";
    }

    char macStr[18] = {0};

    snprintf(
        macStr,
        sizeof(macStr),
        "%02X:%02X:%02X:%02X:%02X:%02X",

        (uint8_t)manufacturerData[20],
        (uint8_t)manufacturerData[21],
        (uint8_t)manufacturerData[22],
        (uint8_t)manufacturerData[23],
        (uint8_t)manufacturerData[24],
        (uint8_t)manufacturerData[25]
    );

    return String(macStr);
}

// ================= BLE CALLBACK =================
class ScanCallbacks : public NimBLEScanCallbacks {

    void onResult(const NimBLEAdvertisedDevice* device) override {

        bool validBeacon = false;

        char dataHex[100] = {0};

        String embeddedMac = "";

        // ================= MANUFACTURER DATA =================
        if (device->haveManufacturerData()) {

            std::string data =
                device->getManufacturerData();

            if (data.length() >= 4) {

                uint8_t* d =
                    (uint8_t*)data.data();

                // ===== FILTRO MINEW =====
                if (
                    d[0] == 0x39 &&
                    d[1] == 0x06
                ) {

                    validBeacon = true;

                    // ===== EXTRAER EMBEDDED MAC SOLO SI EXISTE =====
                    if (
                        data.length() >=
                        (EMBEDDED_MAC_END_BYTE + 1)
                    ) {

                        embeddedMac =
                            extractEmbeddedMac(data);
                    }

                    size_t index = 0;

                    for (size_t i = 0; i < data.length(); i++) {

                        index += snprintf(
                            dataHex + index,
                            sizeof(dataHex) - index,
                            "%02X",
                            (uint8_t)data[i]
                        );

                        if (index >= sizeof(dataHex)) {
                            break;
                        }
                    }

                    Serial.println("\n=== BEACON ManufacturerData ===");
                }
            }
        }

        // ================= SERVICE DATA =================
        if (!validBeacon && device->haveServiceData()) {

            std::string serviceData =
                device->getServiceData();

            if (serviceData.length() >= 2) {

                uint8_t* sd =
                    (uint8_t*)serviceData.data();

                // ===== FILTRO CARD =====
                if (
                    sd[0] == 0x06 &&
                    sd[1] == 0x63
                ) {

                    validBeacon = true;

                    size_t index = 0;

                    for (size_t i = 0; i < serviceData.length(); i++) {

                        index += snprintf(
                            dataHex + index,
                            sizeof(dataHex) - index,
                            "%02X",
                            (uint8_t)serviceData[i]
                        );

                        if (index >= sizeof(dataHex)) {
                            break;
                        }
                    }

                    Serial.println("\n=== BEACON ServiceData ===");
                }
            }
        }

        // ================= DESCARTAR =================
        if (!validBeacon) {
            return;
        }

        // ================= INFO =================
        std::string mac =
            device->getAddress().toString();

        String finalMac;

        if (embeddedMac.length() > 0) {
            finalMac = embeddedMac; 
        } else {
            finalMac = mac.c_str();
        }

        Serial.print("MAC BLE: ");
        Serial.println(mac.c_str());

        if (embeddedMac.length() > 0) {

            Serial.print("Embedded MAC: ");
            Serial.println(embeddedMac);

        } else {

            Serial.println(
                "Beacon sin Embedded MAC"
            );
        }

        Serial.print("RSSI: ");
        Serial.println(device->getRSSI());

        Serial.print("Name: ");
        Serial.println(device->getName().c_str());

        if (device->haveServiceData()) {

            Serial.print("Service UUID: ");

            Serial.println(
                device->getServiceDataUUID()
                    .toString()
                    .c_str()
            );
        }

        Serial.print("DATA: ");
        Serial.println(dataHex);

        // ================= GUARDAR =================
        Beacon beacon;

        snprintf(
            beacon.mac,
            sizeof(beacon.mac),
            "%s",
            finalMac.c_str()
        );

        snprintf(
            beacon.embeddedMac,
            sizeof(beacon.embeddedMac),
            "%s",
            embeddedMac.c_str()
        );

        snprintf(
            beacon.name,
            sizeof(beacon.name),
            "%s",
            device->getName().c_str()
        );

        beacon.rssi = device->getRSSI();

        snprintf(
            beacon.uuid,
            sizeof(beacon.uuid),
            "%s",
            device->haveServiceData()
                ? device->getServiceDataUUID()
                    .toString()
                    .c_str()
                : "N/A"
        );

        snprintf(
            beacon.manufacturer,
            sizeof(beacon.manufacturer),
            "%s",
            dataHex
        );

        beacon.timestamp = millis();

        // ================= QUEUE =================
        if (
            xQueueSend(
                beaconQueue,
                &beacon,
                0
            ) != pdTRUE
        ) {

            Serial.println("Queue llena");
        }
    }
};

// ================= TASK BLE =================
void taskBLE(void *pv) {

    NimBLEDevice::init("");

    pScan = NimBLEDevice::getScan();

    pScan->setScanCallbacks(new ScanCallbacks());

    pScan->setActiveScan(false);

    pScan->setInterval(2000);
    pScan->setWindow(50);

    Serial.println(" BLE iniciado");

    // ESCANEO CONTINUO
    pScan->start(0, false, true);

    while (true) {

        Serial.println("🔍 BLE RUNNING");

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ================= TASK PROCESS =================
void taskProcess(void *pv) {
    int count  = 0;

    while (true) {

        Beacon b;

        if (xQueueReceive(beaconQueue, &b, pdMS_TO_TICKS(500))) {

            Serial.print("QUEUE: ");
            Serial.println(uxQueueMessagesWaiting(beaconQueue));

            buffer[count++] = b;

            if (count >= BATCH_SIZE) {

                Serial.println("\n Batch listo");

                for (int i = 0; i < count; i++) {

                    // ===== TIMESTAMP =====
                    time_t currentTime =
                        bootEpoch +
                        (
                            (buffer[i].timestamp - bootMillis)
                            / 1000
                        );

                    struct tm *t = localtime(&currentTime);

                    char datetime[25];

                    strftime(
                        datetime,
                        sizeof(datetime),
                        "%Y-%m-%dT%H:%M:%S",
                        t
                    );

                    // ===== FRAME ASCII =====
                    snprintf(
                        frameBuffer,
                        sizeof(frameBuffer),

                        "SPKBLE$;%s;%s;%d;%s;(%.6f,%.6f);#SPKBLE",

                        buffer[i].mac,
                        buffer[i].name,
                        buffer[i].rssi,
                        datetime,
                        19.394150,
                        -99.172231
                    );

                    Serial.println("\nFRAME:");
                    Serial.println(frameBuffer);

                    // ===== TCP =====
                    NetworkClient client;

                    Serial.println("Conectando TCP...");

                    if (client.connect(host, port)) {

                        Serial.println("TCP conectado");

                        client.println(frameBuffer);

                        Serial.println("FRAME ENVIADO");

                        client.stop();

                        Serial.println("TCP cerrado");

                    } else {

                        Serial.println("Error TCP");
                    }

                    delay(300);
                }

                count = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ================= WIFI =================
void connectWiFi() {

    WiFi.mode(WIFI_STA);

    WiFi.setSleep(false);

    bool connected = false;

    while (!connected) {

        for (int i = 0; i < wifiCount; i++) {

            Serial.println();

            Serial.print("Intentando conectar a: ");

            Serial.println(wifiList[i].ssid);

            WiFi.begin(
                wifiList[i].ssid,
                wifiList[i].password
            );

            int retries = 0;

            while (
                WiFi.status() != WL_CONNECTED &&
                retries < 20
            ) {

                delay(500);

                Serial.print(".");

                retries++;
            }

            if (WiFi.status() == WL_CONNECTED) {

                Serial.println("\n WiFi conectado");

                Serial.print("SSID: ");
                Serial.println(wifiList[i].ssid);

                Serial.print("IP: ");
                Serial.println(WiFi.localIP());

                connected = true;

                break;
            }

            Serial.println("\n No se pudo conectar");

            WiFi.disconnect(true);

            delay(1000);
        }

        if (!connected) {

            Serial.println("\n Ninguna red disponible");

            Serial.println("Reintentando en 5 segundos...\n");

            delay(5000);
        }
    }
}

// ================= SETUP =================
void setup() {

    Serial.begin(115200);

    delay(2000);

    Serial.println("\n=== SITRACK BLE GATEWAY ===");

    // WIFI
    connectWiFi();

    // NTP
    configTime(-21600, 0, "pool.ntp.org");

    struct tm timeinfo;

    while (!getLocalTime(&timeinfo)) {

        delay(500);

        Serial.println("Esperando NTP...");
    }

    bootEpoch = mktime(&timeinfo);

    bootMillis = millis();

    // QUEUE
    beaconQueue =
        xQueueCreate(QUEUE_SIZE, sizeof(Beacon));

    // TASK BLE
    xTaskCreatePinnedToCore(
        taskBLE,
        "BLE_TASK",
        4096,
        NULL,
        2,
        NULL,
        0
    );

    // TASK PROCESS
    xTaskCreatePinnedToCore(
        taskProcess,
        "PROCESS_TASK",
        16384,
        NULL,
        1,
        NULL,
        1
    );
}

// ================= LOOP =================
void loop() {

    // RECONEXION WIFI
    if (WiFi.status() != WL_CONNECTED) {

        Serial.println("WiFi desconectado");

        connectWiFi();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
}