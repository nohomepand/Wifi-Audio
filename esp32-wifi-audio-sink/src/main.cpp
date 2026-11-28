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
        ap_mode.init(ns, "ap_mode", false);

        ssid.init(ns, "ssid", "Set Wifi SSID!");
        pass.init(ns, "pass", "Set Wifi Passphrase!");

        ap_ssid.init(ns, "ap_ssid", "Set AP SSID!");
        ap_hide_ssid.init(ns, "ap_hide_ssid", true);
        ap_pass.init(ns, "ap_pass", "Set AP Passphrase!");
        ap_ip_net.init(ns, "ap_ip_net", "10.1.1.1/24");

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

    DEFINE_NVS_PROP(bool, ap_mode);

    DEFINE_NVS_PROP(String, ssid);
    DEFINE_NVS_PROP(String, pass);
    
    DEFINE_NVS_PROP(String, ap_ssid);
    DEFINE_NVS_PROP(bool, ap_hide_ssid);
    DEFINE_NVS_PROP(String, ap_pass);
    DEFINE_NVS_PROP(String, ap_ip_net); // syntax x.y.z.w(/<network>)

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
IPAddress NULL_IPADDRESS{0, 0, 0, 0};
bool WifiConnected = false;
IPAddress WifiIPAddress;
WiFiUDP udp;
uint8_t packetBuffer[512 * 10]; // 足りる？

void _setWifiConnected(bool value, const IPAddress& addr) {
    if (value) {
        digitalWrite((int) apppref.ledpin, (bool) apppref.invert_led ? HIGH : LOW);
        WifiIPAddress = addr;
    } else
        analogWrite((int) apppref.ledpin, 0x7f);
    WifiConnected = value;
}

class UDPReceiveTask : public rtos::Task {
private:
    // uint8_t packetBuffer[512 * 3]; // 足りる？
public:
    UDPReceiveTask() : rtos::Task("udprcv", 4096U, 2) {
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
        if (WiFi.status() == WL_CONNECTED) {
            _setWifiConnected(true, WiFi.localIP());
            udp.begin(apppref.port);
            Serial.printf("\n接続完了 IPアドレス: %s\n", WifiIPAddress.toString().c_str());
        }
    }

    bool feed() {
        if (!WifiConnected) {
            sleepMs(250);
            if (WiFi.status() == WL_CONNECTED) {
                _setWifiConnected(true, WiFi.localIP());
                udp.begin(apppref.port);
                Serial.printf("\n接続完了! IPアドレス: %s\n", WiFi.localIP().toString().c_str());
            }
            return false;
        }
        int packetSize = udp.parsePacket();
        if (packetSize > 0) {
            int len = udp.read(packetBuffer, sizeof(packetBuffer));
            if (len > 0) {
                size_t bytes_written = 0;
                // 受信したPCMデータを直接I2S DMAへ書き込み
                // i2s_write(I2S_PORT, packetBuffer, len, &bytes_written, pdMS_TO_TICKS(100));
                i2s_write(I2S_PORT, packetBuffer, len, &bytes_written, portMAX_DELAY);
                return true;
            }
        }
        return false;
    }
protected:
    void run() override {
        while(true) {
            if (!feed())
                vTaskDelay(1);
        }
    }
};

class APUDPReceiveTask : public rtos::Task {
private:
    // uint8_t packetBuffer[512 * 3]; // 足りる？

    // x.y.z.w(/net)
    std::pair<bool, std::pair<IPAddress, IPAddress>> _parse_ip_net(const String& ip_net) {
        auto slashIdx = ip_net.indexOf('/');
        IPAddress localAndGatewayIP;
        int netmaskbitsLen = 24;

        if (slashIdx == -1) {
            if (!localAndGatewayIP.fromString(ip_net)) {
                return {false, {NULL_IPADDRESS, NULL_IPADDRESS}};
            }
        } else {
            if (!localAndGatewayIP.fromString(ip_net.substring(0, slashIdx))) {
                return {false, {NULL_IPADDRESS, NULL_IPADDRESS}};
            }
            // 1. slashIdx + 1 で '/' の次の文字から取得する
            String maskStr = ip_net.substring(slashIdx + 1);
            if (maskStr.length() == 0) {
                return {false, {NULL_IPADDRESS, NULL_IPADDRESS}};
            }
            netmaskbitsLen = maskStr.toInt();
            // 3. 範囲外チェック（0〜32以外、または数値変換失敗時など）
            if (netmaskbitsLen < 0 || netmaskbitsLen > 32) {
                return {false, {NULL_IPADDRESS, NULL_IPADDRESS}};
            }
        }

        // 3. 0 と 32 を安全に処理
        uint32_t netmaskIPBits = 0;
        if (netmaskbitsLen == 32) {
            netmaskIPBits = 0xffffffffUL;
        } else if (netmaskbitsLen > 0) {
            netmaskIPBits = 0xffffffffUL << (32 - netmaskbitsLen);
        }

        IPAddress netmaskIP {
            (uint8_t) ((netmaskIPBits >> 24) & 0xff),
            (uint8_t) ((netmaskIPBits >> 16) & 0xff),
            (uint8_t) ((netmaskIPBits >> 8) & 0xff),
            (uint8_t) (netmaskIPBits & 0xff),
        };
        return {true, {localAndGatewayIP, netmaskIP}};
    }

public:
    APUDPReceiveTask() : rtos::Task("APudprcv", 4096, 2) {
        auto parsedIPNET = _parse_ip_net(apppref.ap_ip_net);
        _setWifiConnected(false, NULL_IPADDRESS);
        if (!parsedIPNET.first) {
            Serial.printf("IP/NET の構文にエラーがあります: [%s]\n", apppref.ap_ip_net);
        } else {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_AP);
            if (!WiFi.softAP(apppref.ap_ssid.c_str(), apppref.ap_pass.c_str(), 1, (bool) apppref.ap_hide_ssid ? 1 : 0)) {
                Serial.println("SoftAPの起動に失敗しました");
            } else {
                delay(100); // APインターフェース初期化待ち
                Serial.printf("SoftAP 構成開始 local_IP=[%s] gateway=[%s] subnet=[%s]\n", parsedIPNET.second.first.toString().c_str(), parsedIPNET.second.first.toString().c_str(), parsedIPNET.second.second.toString().c_str());
                if (!WiFi.softAPConfig(parsedIPNET.second.first, parsedIPNET.second.first, parsedIPNET.second.second)) {
                    Serial.println("SoftAPの構成に失敗しました");
                } else {
                    esp_wifi_set_ps(WIFI_PS_NONE);
                    _setWifiConnected(true, WiFi.softAPIP());
                    udp.begin(apppref.port);
                    Serial.printf("SoftAP IP address=[%s]\n", WifiIPAddress.toString().c_str());
                }
            }
        }
    }
    
    bool feed() {
        if (!WifiConnected) {
            return false;
        }
        int packetSize = udp.parsePacket();
        if (packetSize > 0) {
            int len = udp.read(packetBuffer, sizeof(packetBuffer));
            if (len > 0) {
                size_t bytes_written = 0;
                // 受信したPCMデータを直接I2S DMAへ書き込み
                i2s_write(I2S_PORT, packetBuffer, len, &bytes_written, portMAX_DELAY);
                return true;
            }
        }
        return false;
    }
protected:
    void run() override {
        while(true) {
            if (!feed())
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
            .dma_buf_count = 8,
            .dma_buf_len = 256,
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
    _setWifiConnected(false, NULL_IPADDRESS);

    // I2S初期化
    setupI2S();

    {
        appcli.registerHandler("reset", [](String args) {
            ESP.restart();
        }, "Reset");
        appcli.registerHandler("help", [](String args) {
            Serial.println(appcli.getHelpString());
        }, "Show this message");
        
        appcli.registerHandler("status", [](String args) {
            Serial.printf("Wifi ... %s\n",WifiConnected ? WifiIPAddress.toString().c_str() : "(not connected)");
            Serial.printf("Port=[%d]\n", (uint16_t) apppref.port);
            Serial.printf("WiFi Mode=[%s]\n", (bool) apppref.ap_mode ? "AP Mode" : "STA(Client) Mode");
            Serial.printf("SSID=[%s]\n", apppref.ssid.c_str());
            Serial.printf("Pass=[%s]\n", apppref.pass.c_str());
            Serial.printf("AP SSID=[%s] hide=[%s]\n", apppref.ap_ssid.c_str(), (bool) apppref.ap_hide_ssid ? "HIDDEN" : "visible");
            Serial.printf("AP Pass=[%s]\n", apppref.ap_pass.c_str());
            Serial.printf("AP IP/NET=[%s]\n", apppref.ap_ip_net.c_str());
            Serial.printf("LED pin=[%d] [%s]\n", (int) apppref.ledpin, (bool) apppref.invert_led ? "inverted" : "normal");
            Serial.printf("BCLK=[%d]\n", (int) apppref.bclk);
            Serial.printf("WS/LRC=[%d]\n", (int) apppref.ws_or_lrc);
            Serial.printf("DOUT=[%d]\n", (int) apppref.dout);
            Serial.printf("Samplerate=[%d]\n", (uint32_t) apppref.i2s_samplerate);
            Serial.printf("Bis per sample=[%d]\n", (i2s_bits_per_sample_t) apppref.i2s_bps);
        }, "Show current settings");

        appcli.registerHandler("ap-mode", [](String args) {
            apppref.ap_mode = args.equals("true");
            Serial.printf("Wifi mode set to [%s]\n", (bool) apppref.ap_mode ? "AP Mode" : "STA(Client) Mode");
        }, "Set or unset AP mode <true or false>");

        appcli.registerHandler("ssid", [](String args) {
            args.trim();
            apppref.ssid = args;
            Serial.printf("Wifi SSID set to [%s]\n", apppref.ssid.c_str());
            // Preferences prefs;
            // prefs.begin("wifi-audio-sink", false);
            // size_t written = prefs.putString("ssid", args);
            // prefs.end();
            // prefs.begin("wifi-audio-sink", true);
            // String saved = prefs.getString("ssid", "");
            // prefs.end();
            // Serial.printf("Wifi SSID set to [%s], saved=[%s]\n", apppref.ssid.c_str(), saved.c_str());
            // if (written == 0)
            //     Serial.println("NVS write failed");
        }, "Set Wifi SSID");
        appcli.registerHandler("pass", [](String args) {
            args.trim();
            apppref.pass = args;
            Serial.printf("Wifi Passphrase set to [%s]\n", args.c_str());
            // Preferences prefs;
            // prefs.begin("wifi-audio-sink", false);
            // size_t written = prefs.putString("pass", args);
            // prefs.end();
            // Serial.printf("Wifi Passphrase set, NVS bytes written=[%u]\n", (unsigned)written);
        }, "Set Wifi Passphrase");

        appcli.registerHandler("ap-ssid", [](String args) {
            args.trim();
            apppref.ap_ssid = args;
            Serial.printf("AP SSID set to [%s]\n", args.c_str());
        }, "Set AP mode SSID");
        appcli.registerHandler("ap-pass", [](String args) {
            args.trim();
            apppref.ap_pass = args;
            Serial.printf("AP Passphrase set to [%s]\n", args.c_str());
        }, "Set AP mode Passphrase");
        appcli.registerHandler("ap-ipnet", [](String args) {
            args.trim();
            apppref.ap_ip_net = args;
            Serial.printf("AP IP/NET set to [%s]\n", args.c_str());
        }, "Set AP mode IP/NET(x.y.z.w(/net) format)");

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

    if ((bool) apppref.ap_mode) {
        tasks.push_back(new APUDPReceiveTask());
    } else {
        tasks.push_back(new UDPReceiveTask());
    }
    for (auto it : tasks)
        it->start();
}

void loop() {
    appcli.feedInput();
    delay(1);
}
