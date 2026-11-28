use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use clap::Parser;
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};

struct AudioSender {
    destination: String,
    socket: Option<std::net::UdpSocket>,
    retry_at: Instant,
}

impl AudioSender {
    fn connect(destination: &str) -> Result<std::net::UdpSocket> {
        let socket = std::net::UdpSocket::bind("0.0.0.0:0").context("Failed to bind UDP socket")?;
        socket
            .connect(destination)
            .context("Invalid destination address")?;
        Ok(socket)
    }

    fn new(destination: String) -> Result<Self> {
        let socket = Self::connect(&destination)?;
        Ok(Self {
            destination,
            socket: Some(socket),
            retry_at: Instant::now(),
        })
    }

    fn send_audio_chunks(&mut self, pending: &mut Vec<u8>, packet_size: usize) {
        while pending.len() >= packet_size {
            if self.socket.is_none() {
                if Instant::now() < self.retry_at {
                    pending.clear();
                    return;
                }

                match Self::connect(&self.destination) {
                    Ok(socket) => {
                        self.socket = Some(socket);
                        eprintln!("UDP connection restored: {}", self.destination);
                    }
                    Err(err) => {
                        eprintln!("UDP reconnect error: {err}");
                        self.retry_at = Instant::now() + Duration::from_secs(1);
                        pending.clear();
                        return;
                    }
                }
            }

            let send_result = self
                .socket
                .as_ref()
                .expect("socket was connected above")
                .send(&pending[..packet_size]);
            match send_result {
                Ok(bytes_sent) if bytes_sent == packet_size => {
                    pending.drain(..packet_size);
                }
                Ok(bytes_sent) => {
                    eprintln!(
                        "UDP send error: sent {bytes_sent} of {packet_size} bytes; reconnecting"
                    );
                    self.socket = None;
                    self.retry_at = Instant::now() + Duration::from_secs(1);
                    pending.clear();
                    return;
                }
                Err(err) => {
                    eprintln!("UDP send error: {err}; reconnecting");
                    self.socket = None;
                    self.retry_at = Instant::now() + Duration::from_secs(1);
                    pending.clear();
                    return;
                }
            }
        }
    }
}

#[derive(Parser)]
#[command(name = "rust-wifi-audio-source")]
struct Args {
    /// HOST:PORT format to transmit audio data
    #[arg(default_value = "192.168.0.1:2528")]
    ip_port: String,

    /// Transmission audio sampling rate
    #[arg(short, long, default_value_t = 48000)]
    samplerate: u32,

    /// Transmission audio bits per sample
    #[arg(short, long, default_value_t = 16)]
    bps: u16,

    /// Audio sampling block size
    #[arg(long = "block-size", default_value_t = 128)]
    blocksize: u32,
}

fn find_wasapi_cable_output() -> Result<cpal::Device> {
    let host_id = cpal::available_hosts()
        .into_iter()
        .find(|id| id.name().eq_ignore_ascii_case("wasapi"))
        .context("WASAPI host was not found")?;
    let host = cpal::host_from_id(host_id)?;

    host.input_devices()?
        .find(|device| {
            device
                .description()
                .map(|description| description.to_string().contains("CABLE Output"))
                .unwrap_or(false)
        })
        .context("WASAPI CABLE Output input device was not found")
}

fn select_supported_config(
    device: &cpal::Device,
    samplerate: u32,
    blocksize: u32,
    bps: u16,
) -> Result<cpal::StreamConfig> {
    let wanted_format = if bps == 16 {
        cpal::SampleFormat::I16
    } else {
        cpal::SampleFormat::I32
    };
    device
        .supported_input_configs()
        .context("Failed to enumerate input configurations")?
        .find(|config| {
            config.channels() == 2
                && config.sample_format() == wanted_format
                && config.min_sample_rate() <= samplerate
                && config.max_sample_rate() >= samplerate
        })
        .context("No supported 2-channel input configuration matches the sample rate")?;
    let config = cpal::StreamConfig {
        channels: 2,
        sample_rate: samplerate,
        buffer_size: cpal::BufferSize::Fixed(blocksize),
    };

    Ok(config)
}

fn wait_for_stop(stream: cpal::Stream) -> Result<()> {
    let running = Arc::new(AtomicBool::new(true));
    let signal = Arc::clone(&running);
    ctrlc::set_handler(move || {
        signal.store(false, Ordering::SeqCst);
    })?;

    stream.play()?;
    while running.load(Ordering::SeqCst) {
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
    Ok(())
}

fn main() -> Result<()> {
    let args = Args::parse();
    if !matches!(args.samplerate, 44100 | 48000) {
        anyhow::bail!("--samplerate must be either 44100 or 48000");
    }
    if !matches!(args.bps, 16 | 32) {
        anyhow::bail!("--bps must be either 16 or 32");
    }
    if !matches!(args.blocksize, 128 | 256 | 384 | 512) {
        anyhow::bail!("--block-size must be 128, 256, 384, or 512");
    }

    let sender = AudioSender::new(args.ip_port.clone())?;

    let device = find_wasapi_cable_output()?;
    let device_name = device
        .description()
        .map(|description| description.to_string())
        .unwrap_or_else(|_| "<unknown>".to_string());
    let config = select_supported_config(&device, args.samplerate, args.blocksize, args.bps)?;
    let latency_ms = args.blocksize as f64 * 1000.0 / args.samplerate as f64;

    println!("低遅延モード起動: {device_name}");
    println!(
        "送信先: {} ({:.0}kHz / {:.2}ms単位)",
        args.ip_port,
        args.samplerate as f64 / 1000.0,
        latency_ms
    );

    if args.bps == 16 {
        let stream = device.build_input_stream(
            &config,
            {
                let mut sender = sender;
                let mut pending = Vec::with_capacity(args.blocksize as usize * 4);
                let packet_size = args.blocksize as usize * 2 * std::mem::size_of::<i16>();
                move |data: &[i16], _: &cpal::InputCallbackInfo| {
                    let bytes = unsafe {
                        std::slice::from_raw_parts(
                            data.as_ptr().cast::<u8>(),
                            data.len() * std::mem::size_of::<i16>(),
                        )
                    };
                    pending.extend_from_slice(bytes);
                    sender.send_audio_chunks(&mut pending, packet_size);
                }
            },
            |err| eprintln!("Audio stream error: {err}"),
            None,
        )?;
        wait_for_stop(stream)
    } else {
        let stream = device.build_input_stream(
            &config,
            {
                let mut sender = sender;
                let mut pending = Vec::with_capacity(args.blocksize as usize * 8);
                let packet_size = args.blocksize as usize * 2 * std::mem::size_of::<i32>();
                move |data: &[i32], _: &cpal::InputCallbackInfo| {
                    let bytes = unsafe {
                        std::slice::from_raw_parts(
                            data.as_ptr().cast::<u8>(),
                            data.len() * std::mem::size_of::<i32>(),
                        )
                    };
                    pending.extend_from_slice(bytes);
                    sender.send_audio_chunks(&mut pending, packet_size);
                }
            },
            |err| eprintln!("Audio stream error: {err}"),
            None,
        )?;
        wait_for_stop(stream)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reconnects_socket_before_sending_pending_audio() {
        let receiver = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
        receiver
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        let destination = receiver.local_addr().unwrap().to_string();
        let mut sender = AudioSender::new(destination).unwrap();
        sender.socket = None;
        sender.retry_at = Instant::now();
        let mut pending = vec![1, 2, 3, 4];

        sender.send_audio_chunks(&mut pending, 4);

        assert!(pending.is_empty());
        let mut received = [0; 4];
        assert_eq!(receiver.recv(&mut received).unwrap(), received.len());
        assert_eq!(received, [1, 2, 3, 4]);
    }
}
