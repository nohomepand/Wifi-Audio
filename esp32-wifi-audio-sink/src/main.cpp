#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <preference_wrapper.hpp>
#include <thread_helper.hpp>
#include <cli_helper.hpp>

class AppPreference {
public:
    AppPreference(const char* ns) : ns(ns) {}

    void init() {
        ssid.init(ns, "ssid", "Set Wifi SSID!");
        pass.init(ns, "pass", "Set Wifi Passphrase!");
        port.init(ns, "port", 2528);

        ledpin.init(ns, "ledpin", 8);
        invert_led.init(ns, "invert_led", false);

        bclk.init(ns, "bclk", 4);
        ws_or_lrc.init(ns, "ws_or_lrc", 5);
        dout.init(ns, "dout", 6);

        i2s_samplerate.init(ns, "i2s_samplerate", 48000);
        i2s_bps.init(ns, "i2s_bps", I2S_BITS_PER_SAMPLE_16BIT);
        // i2s_swaplr.init(ns, "i2s_swaplr", false);
    }

    DEFINE_NVS_PROP(String, ssid);
    DEFINE_NVS_PROP(String, pass);
    DEFINE_NVS_PROP(uint16_t, port);

    DEFINE_NVS_PROP(int, ledpin);
    DEFINE_NVS_PROP(bool, invert_led);
    
    DEFINE_NVS_PROP(int, bclk);
    DEFINE_NVS_PROP(int, ws_or_lrc);
    DEFINE_NVS_PROP(int, dout);

    DEFINE_NVS_PROP(uint32_t, i2s_samplerate);
    DEFINE_NVS_PROP(i2s_bits_per_sample_t, i2s_bps);
    // DEFINE_NVS_PROP(bool, i2s_swaplr);
private:
    const char* ns;
} apppref("wifi-audio-sink");

constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

bool WifiConnected = false;
void _setWifiConnected(bool value) {
    if (value)
        digitalWrite((int) apppref.ledpin, (bool) apppref.invert_led ? HIGH : LOW);
    else
        analogWrite((int) apppref.ledpin, 0x7f);
    WifiConnected = value;
}
WiFiUDP udp;
class UDPReceiveTask : public rtos::Task {
private:
    uint8_t packetBuffer[512 * 3];
public:
    UDPReceiveTask() : rtos::Task("udprcv", 4096U, 2) {}

    void feed() {
        if (!WifiConnected) {
            sleepMs(250);
            _setWifiConnected(WiFi.status() == WL_CONNECTED);
            if (WifiConnected) {
                udp.begin(apppref.port);
                Serial.printf("\n接続完了! IPアドレス: %s\n", WiFi.localIP().toString().c_str());
            }
            return;
        }
        int packetSize = udp.parsePacket();
        if (packetSize > 0) {
            int len = udp.read(packetBuffer, sizeof(packetBuffer));
            if (len > 0) {
                size_t bytes_written = 0;
                // 受信したPCMデータを直接I2S DMAへ書き込み
                // i2s_write(I2S_PORT, packetBuffer, len, &bytes_written, pdMS_TO_TICKS(100));
                i2s_write(I2S_PORT, packetBuffer, len, &bytes_written, portMAX_DELAY);
            }
        }
    }
protected:
    void run() override {
        while(true) {
            feed();
            vTaskDelay(1);
        }
    }
};

void setupI2S() {
    // 低遅延向けにDMAバッファを小さく設定 (128サンプル × 4バッファ = 約10.6ms分)
    i2s_config_t i2s_config = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
            .sample_rate = apppref.i2s_samplerate,
            .bits_per_sample = apppref.i2s_bps,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
            .dma_buf_count = 4,
            .dma_buf_len = 128,
            .use_apll = false,
            .tx_desc_auto_clear = true, // データ途切れ時に無音を出力してノイズを防ぐ
            .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
            .mck_io_num = I2S_PIN_NO_CHANGE,
            .bck_io_num = apppref.bclk,
            .ws_io_num = apppref.ws_or_lrc,
            .data_out_num = apppref.dout,
            .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_PORT);
}

std::vector<rtos::Task*> tasks;
CLIHelper appcli("> ", false, Serial, 512);

void setup() {
    Serial.begin(115200);
    delay(1000);
    apppref.init();

    // LED status
    pinMode((int) apppref.ledpin, OUTPUT);
    _setWifiConnected(false);

    // I2S初期化
    setupI2S();

    // Wi-Fi接続と省電力モードの完全無効化
    WiFi.mode(WIFI_STA);
    WiFi.begin(apppref.ssid.c_str(), apppref.pass.c_str());
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    Serial.print("Wi-Fi接続中");
    for (auto i = 0; WiFi.status() != WL_CONNECTED && i < 30; i++) {
        delay(250);
        Serial.print(".");
    }
    _setWifiConnected(WiFi.status() == WL_CONNECTED);
    if (WifiConnected) {
        udp.begin(apppref.port);
        Serial.printf("\n接続完了! IPアドレス: %s\n", WiFi.localIP().toString().c_str());
    } else
        Serial.println("\n接続失敗");

    {
        appcli.registerHandler("reset", [](String args) {
            ESP.restart();
        }, "Reset");
        appcli.registerHandler("help", [](String args) {
            Serial.println(appcli.getHelpString());
        }, "Show this message");
        
        appcli.registerHandler("status", [](String args) {
            Serial.printf("SSID=[%s]\n", apppref.ssid.c_str());
            Serial.printf("Pass=[%s]\n", apppref.pass.c_str());
            Serial.printf("Wifi ... %s\n", WifiConnected ? WiFi.localIP().toString().c_str() : "(not connected)");
            Serial.printf("Port=[%d]\n", (uint16_t) apppref.port);
            Serial.printf("LED pin=[%d] [%s]\n", (int) apppref.ledpin, (bool) apppref.invert_led ? "inverted" : "normal");
            Serial.printf("BCLK=[%d]\n", (int) apppref.bclk);
            Serial.printf("WS/LRC=[%d]\n", (int) apppref.ws_or_lrc);
            Serial.printf("DOUT=[%d]\n", (int) apppref.dout);
            Serial.printf("Samplerate=[%d]\n", (uint32_t) apppref.i2s_samplerate);
            Serial.printf("Bis per seconds=[%d]\n", (i2s_bits_per_sample_t) apppref.i2s_bps);
        }, "Show current settings");
        appcli.registerHandler("ssid", [](String args) {
            args.trim();
            apppref.ssid = args;
            Preferences prefs;
            prefs.begin("wifi-audio-sink", false);
            size_t written = prefs.putString("ssid", args);
            prefs.end();
            prefs.begin("wifi-audio-sink", true);
            String saved = prefs.getString("ssid", "");
            prefs.end();
            Serial.printf("Wifi SSID set to [%s], saved=[%s]\n", apppref.ssid.c_str(), saved.c_str());
            if (written == 0)
                Serial.println("NVS write failed");
        }, "Set Wifi SSID");
        appcli.registerHandler("pass", [](String args) {
            args.trim();
            apppref.pass = args;
            Preferences prefs;
            prefs.begin("wifi-audio-sink", false);
            size_t written = prefs.putString("pass", args);
            prefs.end();
            Serial.printf("Wifi Passphrase set, NVS bytes written=[%u]\n", (unsigned)written);
        }, "Set Wifi Passphrase");
        appcli.registerHandler("port", [](String args) {
            apppref.port = (uint32_t) args.toInt();
            Serial.printf("Port set to [%d]\n", (uint32_t) apppref.port);
        }, "Set UDP Port");

        appcli.registerHandler("ledpin", [](String args) {
            apppref.ledpin = (int) args.toInt();
            Serial.printf("LED pin set to [%d]\n", (int) apppref.ledpin);
        }, "Set LED status pin");
        appcli.registerHandler("invled", [](String args) {
            apppref.invert_led = args.equals("true");
            Serial.printf("LED status inverted mode is set to [%s]\n", (bool) apppref.invert_led ? "true" : "false");
        }, "Set LED status inverted mode(true or false)");

        appcli.registerHandler("bclk", [](String args) {
            apppref.bclk = (int) args.toInt();
            Serial.printf("BCLK pin set to [%d]\n", (int) apppref.bclk);
        }, "Set BCLK pin");
        appcli.registerHandler("ws", [](String args) {
            apppref.ws_or_lrc = (int) args.toInt();
            Serial.printf("WS/LRC pin set to [%d]\n", (int) apppref.ws_or_lrc);
        }, "Set WS/LRC pin");
        appcli.registerHandler("dout", [](String args) {
            apppref.dout = (int) args.toInt();
            Serial.printf("DOUT pin set to [%d]\n", (int) apppref.dout);
        }, "Set DOUT pin");
        appcli.registerHandler("samplerate", [](String args) {
            long val = args.toInt();
            if (val <= 44100) {
                val = 44100;
            } else {
                val = 48000;
            }
            apppref.i2s_samplerate = val;
            Serial.printf("Samplerate set to [%d]\n", (uint32_t) apppref.i2s_samplerate);
        }, "Set I2S Sampling rate (44100 or 48000)");
        appcli.registerHandler("bps", [](String args) {
            long val = args.toInt();
            if (val <= I2S_BITS_PER_SAMPLE_16BIT) {
                val = I2S_BITS_PER_SAMPLE_16BIT;
            } else {
                val = I2S_BITS_PER_SAMPLE_32BIT;
            }
            apppref.i2s_bps = (i2s_bits_per_sample_t) val;
            Serial.printf("Bits per seconds set to [%d]\n", (i2s_bits_per_sample_t) apppref.i2s_bps);
        }, "Set I2S Bits per seconds (16 or 32)");
    }

    tasks.push_back(new UDPReceiveTask());
    for (auto it : tasks)
        it->start();
}

void loop() {
    appcli.feedInput();
    delay(1);
}
