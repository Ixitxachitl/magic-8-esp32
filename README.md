# Magic 8 Ball – ESP32

An LLM-powered Magic 8 Ball running on the **Waveshare ESP32-S3-Touch-AMOLED-1.75C** development board. Tap the screen or speak your question to receive a mystical AI-generated answer, displayed on a 3D animated icosahedron on the 466×466 AMOLED display.

When offline or without an API key, it falls back to the original 20 Magic 8 Ball responses.

## Hardware

| Component | Detail |
|-----------|--------|
| MCU | ESP32-S3 (240 MHz, PSRAM) |
| Display | CO5300 466×466 QSPI AMOLED |
| Touch | CST9217 capacitive |
| Audio Codec | ES8311 (speaker DAC + microphone ADC) |
| PMIC | AXP2101 (battery, charging) |

## Features

- **LLM-powered answers** – sends a prompt to any OpenAI-compatible API; defaults to [Groq](https://groq.com/) free tier
- **Classic fallback** – all 20 original Magic 8 Ball responses when offline
- **Voice input** – ask your question out loud; energy-based VAD detects speech start/end automatically
- **Speech-to-text** – transcribes audio via Whisper API (Groq or OpenAI); model selectable from the portal
- **Text-to-speech** – speaks the answer aloud; four engines supported (see below)
- **Tap to ask** – touch the ball on screen
- **3D animated UI** – rotating icosahedron with fade transitions and thinking animation
- **WiFi config portal** – captive portal AP for first-time setup; all settings editable live without reflashing
- **Power button** – single press shows battery level; double press re-enters AP setup mode

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- USB-C cable connected to the Waveshare board
- A free API key from [Groq](https://console.groq.com/) (or any OpenAI-compatible provider)

### First Boot

1. **Build and flash:**
   ```
   pio run -e esp32s3 --target upload --upload-port COM3
   ```

2. **Connect to the setup AP:**
   The display will show `Connect WiFi: Magic8Ball-Setup`. Join that WiFi network on your phone or laptop.

3. **Open the config portal:**
   Navigate to `http://192.168.4.1` in a browser. Enter your WiFi credentials, add your API keys, and configure providers.

4. **Ask away:**
   After saving, tap the ball or speak your question.

### Serial Monitor

```
pio device monitor -b 115200
```

## Project Structure

```
├── include/
│   ├── pin_config.h        # GPIO pin definitions
│   ├── lv_conf.h           # LVGL configuration
│   ├── magic8ball_ui.h     # UI API
│   ├── llm_client.h        # LLM API client
│   ├── mic_recorder.h      # I2S microphone + VAD
│   ├── stt_client.h        # Speech-to-text (Whisper) client
│   ├── tone_player.h       # Boot tone player
│   ├── tts_groq.h          # TTS via Groq Orpheus
│   ├── tts_elevenlabs.h    # TTS via ElevenLabs
│   ├── tts_sam.h           # TTS via offline SAM
│   ├── audio_hal.h         # Shared I2S / ES8311 helpers
│   └── config_portal.h     # WiFi config portal
├── src/
│   ├── main.cpp            # Entry point, hardware init, main loop
│   ├── magic8ball_ui.c     # LVGL 3D icosahedron interface
│   ├── llm_client.cpp      # Async LLM API client (FreeRTOS task)
│   ├── mic_recorder.cpp    # ES8311 ADC recording + VAD
│   ├── stt_client.cpp      # Whisper multipart upload (FreeRTOS task)
│   ├── tone_player.cpp     # Startup tone
│   ├── tts_groq.cpp        # Groq / OpenAI TTS playback
│   ├── tts_elevenlabs.cpp  # ElevenLabs TTS playback
│   ├── tts_sam.cpp         # Offline SAM TTS
│   ├── es8311.c            # ES8311 codec driver
│   ├── es7210.cpp          # ES7210 ADC driver
│   └── config_portal.cpp   # WiFi AP + web config server
├── lib/
│   └── ESP8266SAM/         # Offline software speech synthesizer
├── platformio.ini
└── README.md
```

## Configuration

All settings are stored in NVS flash, persist across reboots, and take effect immediately when saved from the portal (no reboot required).

### API Keys

Enter keys for each service you plan to use. Only services with a key will appear in the provider dropdowns.

| Service | Used for |
|---------|----------|
| Groq | LLM, STT (Whisper), TTS (Orpheus) |
| Gemini | LLM |
| OpenAI | LLM, STT (Whisper), TTS |
| ElevenLabs | TTS |
| Custom | Any OpenAI-compatible LLM endpoint |

### LLM Settings

| Setting | Default | Description |
|---------|---------|-------------|
| Provider | `groq` | `groq`, `gemini`, `openai`, or `custom` |
| Model | `llama-3.3-70b-versatile` | Click **Scan** to fetch available models |
| System Prompt | *(Magic 8 Ball persona)* | Customise the 8 Ball's personality |

### STT Settings

| Setting | Default | Description |
|---------|---------|-------------|
| Provider | `groq` | `groq` or `openai` |
| Model | `whisper-large-v3-turbo` | Click **Scan** to fetch available Whisper models |

Audio is recorded at 16 kHz, downsampled to 8 kHz before upload to halve payload size.

### TTS Settings

| Engine | Quality | Requires |
|--------|---------|----------|
| SAM (offline) | Robotic, no network | Nothing |
| Groq Orpheus | Natural, low latency | Groq API key |
| OpenAI TTS | Natural | OpenAI API key |
| ElevenLabs | High quality, many voices | ElevenLabs API key |

To reconfigure at any time, **double-press** the power button to re-enter AP setup mode. A **single press** shows the current battery level.

## Supported LLM Providers

| Provider | Free Tier | Notes |
|----------|-----------|-------|
| [Groq](https://groq.com/) | Yes | Also supports STT and TTS |
| [Google Gemini](https://aistudio.google.com/) | Yes | OpenAI-compatible endpoint |
| [OpenAI](https://platform.openai.com/) | No | Also supports STT and TTS |
| Custom | — | Any OpenAI-compatible `/chat/completions` endpoint |

## License

This project is licensed under the [GNU General Public License v2.0](LICENSE).
