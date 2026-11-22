import argparse
import dataclasses
import socket
import sounddevice as sd
import numpy as np

@dataclasses.dataclass
class Setting():
    HOST_IP: str = "192.168.0.1"
    HOST_PORT: int = 2528
    SAMPLERATE: int = 48000
    BPS: int = 16
    BLOCKSIZE: int = 128 # 128サンプル = 約2.67msごとに送信（512バイト）

    def parse_host_ip_port(self, ip_port: str):
        ip, *_sub = ip_port.split(":", 1)
        port = int(_sub[0]) if _sub else self.__class__.HOST_PORT
        self.HOST_IP = ip
        self.HOST_PORT = port

    @property
    def host_ip_port(self):
        return f"{self.HOST_IP}:{self.HOST_PORT}"

_setting = Setting()

class Command():
    __parser: argparse.ArgumentParser = None # type: ignore

    def __init__(self, parser = None):
        self.__parser = parser or argparse.ArgumentParser()

    def setup(self):
        self.__parser.add_argument(
            "ip_port",
            default=_setting.host_ip_port,
            help="HOST:PORT format to transmit audio data (defaults=%(default)s)",
        )
        self.__parser.add_argument(
            "--samplerate", "-s",
            dest="samplerate",
            type=int,
            default=_setting.SAMPLERATE,
            choices=[44100, 48000],
            help="Transmission audio sampling rate (defaults=%(default)s)",
        )
        self.__parser.add_argument(
            "--bps", "-b",
            dest="bps",
            type=int,
            default=_setting.BPS,
            choices=[16, 32],
            help="Transmission audio bps (defaults=%(default)s)",
        )
        self.__parser.add_argument(
            "--block-size", "-bs",
            dest="blocksize",
            type=int,
            default=_setting.BLOCKSIZE,
            choices=[128, 128 * 2, 128 * 3, 128 * 4],
            help="Audio sampling block size (defaults=%(default)s)",
        )
        return self

    def action(self, args=None):
        kwargs = {
            k: v
            for k, v in vars(self.__parser.parse_args(args=args)).items()
                if not k.startswith("_")
        }
        return self.__handle(**kwargs)

    def __handle(
            self,
            ip_port: str = "192.168.0.1:2528",
            samplerate: int = 48000,
            bps: int = 16,
            blocksize: int = 128,
            **_rest):
        _setting.parse_host_ip_port(ip_port)
        _setting.SAMPLERATE = samplerate
        _setting.BPS = bps
        _setting.BLOCKSIZE = blocksize

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def find_wasapi_vbcable():
    # 低遅延なWASAPIホストAPIを検索
    wasapi_idx = None
    for idx, api in enumerate(sd.query_hostapis()):
        if "WASAPI" in api['name']:
            wasapi_idx = idx
            break
    if wasapi_idx is None:
        raise RuntimeError("WASAPIが見つかりません。")

    # WASAPI配下の CABLE Output を検索
    for idx, dev in enumerate(sd.query_devices()):
        if dev['hostapi'] == wasapi_idx and "CABLE Output" in dev['name'] and dev['max_input_channels'] > 0:
            return idx
    raise RuntimeError("WASAPIの CABLE Output が見つかりません。")

def audio_callback(indata: np.ndarray, frames, time, status):
    # コンソール出力（print）は遅延の原因になるため省き、最速でUDP送出
    sock.sendto(indata.tobytes(), (_setting.HOST_IP, _setting.HOST_PORT))

def main():
    Command().setup().action()
    dev_idx = find_wasapi_vbcable()
    print(f"低遅延モード起動: [{dev_idx}] {sd.query_devices(dev_idx)['name']}")
    print(f"送信先: {_setting.host_ip_port} (48kHz / 2.67ms単位)")

    with sd.InputStream(device=dev_idx,
                        channels=2,
                        samplerate=_setting.SAMPLERATE,
                        dtype='int16' if _setting.BPS == 16 else 'int32',
                        blocksize=_setting.BLOCKSIZE,
                        latency='low',  # OSに最小レイテンシを要求
                        callback=audio_callback):
        while True:
            sd.sleep(1000)

if __name__ == "__main__":
    main()
