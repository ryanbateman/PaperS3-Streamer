# M5Stack PaperS3 Remote Display

A wireless E-Ink display system for the M5Stack PaperS3.  
It connects to your WiFi, receives Text or Images via a REST API, and displays them. Assumes a DHCP network.

## Features

- **Wireless**: Connects via WiFi (DHCP).
- **REST API**: Simple HTTP endpoints for Text and Images.
- **Auto-Rotation**: Integrated IMU (accelerometer) rotates content automatically.
- **Auto-Shutdown**: Powers off after 3 minutes of inactivity to save battery.
- **Pagination**: Automatically splits long text into multiple pages.
- **Gesture Control**:
    - **Swipe Left/Right**: Next / Previous Page.
    - **Swipe Up**: Increase Font Size (Text & Stream).
    - **Swipe Down**: Decrease Font Size.
- **UI & Navigation**:
    - Tap screen to toggle Header (IP/Bat) and Footer (Nav Buttons) for distraction-free reading.
    - Status bar shows Page Count, Battery %, and IP Address.

## Installation / Flashing

1. Install **PlatformIO**.
2. Open this folder.
3. Copy `src/secrets.h.example` to `src/secrets.h` and fill in your WiFi credentials:
   ```bash
   cp src/secrets.h.example src/secrets.h
   # Edit src/secrets.h with your SSID and password
   ```
4. Upload to M5Stack PaperS3:
   ```bash
   pio run -t upload
   ```

## Usage (Client)

A Python client is provided in `client/paper_cli.py`.

### Send Text
```bash
# Set IP (Optional, or use --ip flag)
$env:PAPER_IP="192.168.1.XXX"

# Send simple string
python client/paper_cli.py text "Hello World"

# Send File (Piping) - Forces UTF-8
Get-Content my_book.txt -Encoding UTF8 | python client/paper_cli.py text
```

### Send Image
Supported formats: JPG, PNG, BMP (auto-converted).  
**Requirement**: Install Pillow for auto-formatting: `pip install Pillow`

```bash
# Pipe binary data
cat photo.jpg | python client/paper_cli.py image
```

## API Reference

- **GET** `/api/status`
    - Returns JSON with mode, memory stats, WiFi RSSI, screen dimensions, rotation.
- **GET** `/api/screenshot`
    - Returns current display as BMP image.
- **POST** `/api/text`
    - Body: `{"text": "...", "size": 2, "clear": true}`
- **POST** `/api/image`
    - Body: Multipart file upload (`name="file"`).
- **POST** `/api/mqtt`
    - Body: `{"broker": "mqtt.example.com", "topic": "sensors/#", "port": 1883, "username": "", "password": ""}`
    - Subscribes to MQTT topic and displays messages.
- **TCP Stream** `Port 2323`
    - Raw TCP socket for line-by-line streaming.
- **Map Mode** (via CLI)
    - Uses Stadia Maps Static API with Stamen Toner style.
    - Requires `STADIA_API_KEY` environment variable or `--api-key` flag.

## Stream Mode
Connect to port `2323` via TCP to stream text line-by-line (like `tail -f`).
Lines appear from the bottom up and wrap automatically.
```bash
# Example: Stream Ping
ping google.com | python client/paper_cli.py stream
```

## Map Mode
Display a map centered on specified coordinates using Stamen Toner style (high-contrast B&W, optimized for e-ink).

**Requirement**: Get a free Stadia Maps API key at https://client.stadiamaps.com/signup/

```bash
# Set API key (or use --api-key flag)
$env:STADIA_API_KEY="your-api-key"

# Display map of Times Square, NYC
python client/paper_cli.py map --lat 40.758 --lon -73.9855

# With custom zoom level (0-18, default 15)
python client/paper_cli.py map --lat 51.5074 --lon -0.1278 --zoom 12
```

A marker is automatically placed at the center coordinates.

## MQTT Mode
Subscribe to an MQTT topic and display messages as they arrive. The display updates automatically when new messages are published.

```bash
# Subscribe to a topic (no auth)
python client/paper_cli.py mqtt --broker "mqtt.example.com" --topic "home/sensors/temperature"

# With authentication
python client/paper_cli.py mqtt --broker "mqtt.example.com" --topic "alerts/#" --username "user" --password "pass"

# Custom port
python client/paper_cli.py mqtt --broker "192.168.1.50" --port 1884 --topic "test/topic"
```

The device will display a "waiting for messages" screen until the first message arrives. Messages are displayed like text mode with pagination support.

## Testing
To run the automated test suite, ensure you have python `pytest` and `requests` installed:
```bash
pip install pytest requests pillow
```
Then run with the device IP:
```bash
# Windows
$env:PAPER_IP="192.168.1.XXX"
pytest -s

# Linux/Mac
PAPER_IP=192.168.1.XXX pytest -s
```

## Credits
Built with [M5Unified](https://github.com/m5stack/M5Unified), [ArduinoJson](https://arduinojson.org/), and [PubSubClient](https://pubsubclient.knolleary.net/) on [PlatformIO](https://platformio.org/).

## License
MIT License - see [LICENSE](LICENSE) for details.
