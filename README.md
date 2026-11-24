# ESP Audio player

* Requirements

    * Windows / Mac
    * VB Audio Cable (オーディオキャプチャ用)
    * VSCode + PlatformIO
    * uv or Python3.15
    * WiFi 環境
        * 2026/11/22 から SoftAPモードにも対応
    * ESP32系のマイコン (ESP32-S3 Super Miniなど, WiFi使えれば安価なものでもよさそう)
    * MAX98357A や PCM5102A などの外部 I2S DACブレークアウトボード

## Install & setup VB-Audio Virtual Cable

1. https://vb-audio.com/Cable/ から環境にあったドライバをダウンロード
2. 管理者として実行してインストール
3. Windowsの「サウンドの詳細設定（mmsys.cpl）」を開く
   * 「再生」タブの CABLE Input と、「録音」タブの CABLE Output の両方のプロパティ ＞「詳細」タブを開く
   * 既定の形式を 「2チャネル、16ビット、48000 Hz（DVDの音質）」 に統一
   * 同じ画面の「排他モード」の2つのチェックボックスはオンのままでよいらしい
4. VBCABLE_ControlPanel を管理者として実行
    * 上部メニューの Options ＞ Internal Sampling Rate を 48000 Hz に設定
    * Options ＞ Max Latency をデフォルト（7168 smp）から 1024 smp または 1536 smp に下げてアプリを再起動
    * これでドライバ内部の遅延が約150msから約20〜30msに激減するらしい

## Audio sink (ESP32 + I2S)

1. ESP32系のマイコンに I2S DACブレークアウトボードを接続
2. ESP32を PCに接続
3. VSCode で esp32-wifi-audio-sink フォルダを開く
4. PlatformIO をアクティベートしてUpload
    * ボード設定は platformio.iniの env:esp32-s3-super-mini を参照
5. シリアル接続で ESP32に接続(115200 baud, no stop-bits, no parity)
6. WiFi設定
    * SSID -> `ssid [SSID]`
    * Passphrase -> `pass [PASSPHRASE]`
7. I2S用のピン設定を入力 `help` コマンド参照
    * LCR(WS) ピン -> `ws [ピン番号]`
    * BCLK ピン -> `bclk [ピン番号]`
    * DOUT ピン -> `dout [ピン番号]`
8. ESP32をリセット
    * `reset` コマンドまたは物理リセットボタン
9. IPアドレスを控える


## Audio source (Python)

1. VSCode で python-wifi-audio-source フォルダを開く
2. Python環境を作る
    * uvであれば
        1. `uv venv -p 3.15 .venv/` 
        2. `.venv/Scripts/activate`
        3. `uv sync`
    * 素のPythonであれば
        1. `python -m venv .venv`
        2. `.venv/Scripts/activate`
        3. `python -m pip install -r requirements.txt`
3. 実行
    * uvであれば
        1. `uv run python-wifi-audio-source [ESP32のIPアドレス]:2528`
    * 素のPythonであれば
        1. `python ./src/python_wifi_audio_source/__init__.py [ESP32のIPアドレス]:2528`


## Audio source (rust)

1. VSCode で rust-wifi-audio-source フォルダを開く
2. `cargo build` する
3. `cargo run -- [ESP32のIPアドレス]:2528` を実行
