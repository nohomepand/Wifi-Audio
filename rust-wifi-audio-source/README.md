# rust-wifi-audio-source

Rust implementation of the Wi-Fi audio source. It captures 2-channel PCM audio
from the VB-Audio CABLE Output input on Windows WASAPI and sends it by UDP.

## Run

```powershell
cargo run -- 192.168.0.1:2528 --samplerate 48000 --bps 16 --block-size 128
```
