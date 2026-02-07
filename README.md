# Paper Piper

A wireless E-Ink display system for the [M5Stack PaperS3](https://shop.m5stack.com/products/m5paper-s3). Turn your PaperS3 into a remote display that can show text, images, maps, live streams, and MQTT messages.

## Features

- **Five Display Modes**: Text, Image, Stream, Map, and MQTT
- **Auto-Rotation**: Integrated IMU rotates content when you rotate the device
- **Auto-Shutdown**: Powers off after 3 minutes of inactivity to save battery
- **Content Retention**: E-ink naturally retains displayed content when device powers off
- **Touch Gestures**: Swipe to navigate pages, change font size, or toggle UI
- **REST API**: Simple HTTP endpoints for easy integration
- **Unified Header**: Consistent status bar showing IP, mode, battery icon with charge level

## Installation

### Requirements
- [PlatformIO](https://platformio.org/) (CLI or IDE)
- M5Stack PaperS3 connected via USB

### Setup

1. Clone this repository
2. Copy the secrets template and add your WiFi credentials:
   ```bash
   cp src/secrets.h.example src/secrets.h
   # Edit src/secrets.h with your SSID and password
   ```
3. Build and upload:
   ```bash
   pio run -t upload
   ```

The device will connect to WiFi and display its IP address on the welcome screen.

### Python Client Setup

```bash
pip install requests pillow
```

Set your device IP (or use `--ip` flag with each command):
```bash
# Linux/Mac
export PAPER_IP="192.168.1.100"

# Windows PowerShell
$env:PAPER_IP="192.168.1.100"
```

---

## Display Modes

### Text Mode

Display static text with automatic pagination. Supports word-wrap and multiple pages for long content.

**Using the Python client:**
```bash
# Simple message
python client/paper_cli.py text "Hello, World!"

# Multi-line text
python client/paper_cli.py text "Line 1\nLine 2\nLine 3"

# Display a file
cat book.txt | python client/paper_cli.py text

# Windows PowerShell
Get-Content book.txt -Encoding UTF8 | python client/paper_cli.py text
```

**Using curl:**
```bash
# JSON body
curl -X POST http://192.168.1.100/api/text \
  -H "Content-Type: application/json" \
  -d '{"text": "Hello from curl!", "size": 2}'

# Form data
curl -X POST http://192.168.1.100/api/text \
  -d "text=Hello from curl!"
```

**Gestures in Text Mode:**
- Swipe left/right: Navigate pages
- Swipe up/down: Increase/decrease font size
- Tap: Toggle header/footer UI

---

### Image Mode

Display JPEG or PNG images. Images are automatically scaled to fit the screen while maintaining aspect ratio.

**Using the Python client:**
```bash
# Send an image file
cat photo.jpg | python client/paper_cli.py image

# The client auto-converts and optimizes for e-ink
```

**Using curl:**
```bash
curl -X POST http://192.168.1.100/api/image \
  -F "file=@photo.jpg"
```

**Tips:**
- High-contrast images work best on e-ink
- The device handles both landscape and portrait orientations
- Images are centered and scaled to fit

---

### Stream Mode

Real-time text streaming via TCP, similar to `tail -f`. New lines appear at the bottom and scroll up. Perfect for logs, monitoring, or live data.

**Using the Python client:**
```bash
# Stream ping output
ping google.com | python client/paper_cli.py stream

# Stream a log file
tail -f /var/log/syslog | python client/paper_cli.py stream

# Stream any command output
dmesg -w | python client/paper_cli.py stream
```

**Using netcat (nc):**
```bash
# Connect directly to the stream port
echo "Hello from netcat" | nc 192.168.1.100 2323

# Stream continuous data
tail -f /var/log/syslog | nc 192.168.1.100 2323

# Interactive session
nc 192.168.1.100 2323
# Type lines and press Enter to send
```

**Gestures in Stream Mode:**
- Swipe up/down: Increase/decrease font size
- Tap: Toggle header UI

**Note:** The device stays awake while receiving data. If no data is received for 3 minutes, the device will sleep (retaining the last content on screen).

---

### Map Mode

Display a map centered on GPS coordinates using [Stadia Maps](https://stadiamaps.com/) with the Stamen Toner style (high-contrast black & white, ideal for e-ink).

**Setup:**
1. Get a free API key at https://client.stadiamaps.com/signup/
2. Set the environment variable:
   ```bash
   # Linux/Mac
   export STADIA_API_KEY="your-api-key"
   
   # Windows PowerShell
   $env:STADIA_API_KEY="your-api-key"
   ```

**Using the Python client:**
```bash
# Search by location name (uses OpenStreetMap geocoding)
python client/paper_cli.py map --location "Berlin, Germany"
python client/paper_cli.py map --location "Eiffel Tower, Paris"
python client/paper_cli.py map --location "Central Park, New York"

# Display by coordinates - Times Square, NYC
python client/paper_cli.py map --lat 40.758 --lon -73.9855

# London with custom zoom level
python client/paper_cli.py map --lat 51.5074 --lon -0.1278 --zoom 12

# Sydney Opera House, zoomed in
python client/paper_cli.py map --lat -33.8568 --lon 151.2153 --zoom 16

# Pass API key directly
python client/paper_cli.py map --location "Tokyo" --api-key "your-key"
```

**Features:**
- **Location search**: Use `--location` to search by name (cities, landmarks, addresses)
- Automatic zoom level based on location type (city, neighborhood, landmark)
- Automatic marker at the center coordinates
- Zoom levels 0-18 (override with `--zoom`)
- Map adapts to device orientation (portrait/landscape)
- @2x resolution for sharp text on e-ink

---

### MQTT Mode

Subscribe to an MQTT topic and display messages as they arrive. The display updates automatically when new messages are published. Great for IoT dashboards, alerts, or notifications.

**Using the Python client:**
```bash
# Subscribe to a topic (no authentication)
python client/paper_cli.py mqtt --broker "test.mosquitto.org" --topic "test/paper"

# With authentication
python client/paper_cli.py mqtt \
  --broker "mqtt.example.com" \
  --topic "home/sensors/#" \
  --username "user" \
  --password "pass"

# Custom port
python client/paper_cli.py mqtt \
  --broker "192.168.1.50" \
  --port 1884 \
  --topic "alerts/critical"
```

**Using curl (to configure MQTT):**
```bash
curl -X POST http://192.168.1.100/api/mqtt \
  -H "Content-Type: application/json" \
  -d '{
    "broker": "test.mosquitto.org",
    "topic": "test/paper",
    "port": 1883
  }'
```

**Testing with mosquitto_pub:**
```bash
# After subscribing, publish a test message
mosquitto_pub -h test.mosquitto.org -t "test/paper" -m "Hello from MQTT!"
```

**Features:**
- Wildcard topics supported (e.g., `sensors/#`, `home/+/temperature`)
- Auto-reconnect on connection loss
- Messages displayed with pagination (swipe to navigate)
- Optional username/password authentication

**Note:** The device stays awake while receiving messages. If no messages are received for 3 minutes, the device will sleep (retaining the last message on screen).

---

## Power & Content Retention

E-ink displays naturally retain their content without power. Paper Piper takes advantage of this:

- **When the device times out** (3 minutes of inactivity): The content is redrawn without the header/footer UI, a "Sleeping..." indicator appears at the bottom, and the device powers off. Your content remains visible on the e-ink display.

- **When you press the power button**: The e-ink display keeps whatever was on screen when power was cut.

- **When you wake the device**: Touch the screen or press the power button. The welcome screen appears, ready for new content.

This means you can send an image, map, or text to the device, and it will remain visible indefinitely as a "poster" even after the device powers off - no battery drain.

---

## API Reference

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | Device status (mode, memory, screen size, rotation) |
| `/api/screenshot` | GET | Current display as BMP image |
| `/api/text` | POST | Display text content |
| `/api/image` | POST | Display image (multipart upload) |
| `/api/mqtt` | POST | Configure MQTT subscription |
| Port `2323` | TCP | Raw stream connection |

### Status Response Example
```json
{
  "mode": "TEXT",
  "heap_free": 3104303,
  "screen_width": 960,
  "screen_height": 540,
  "rotation": 1,
  "wifi_rssi": -62
}
```

### Custom Headers

When sending images via `/api/image`, you can include the `X-Content-Type` header to indicate the content type:

```bash
# The CLI automatically sets this for maps
curl -X POST http://192.168.1.100/api/image \
  -H "X-Content-Type: map" \
  -F "file=@map.jpg"
```

This displays "MAP" instead of "IMAGE" in the header bar.

---

## Gesture Controls

| Gesture | Text Mode | Stream Mode | Image Mode |
|---------|-----------|-------------|------------|
| Swipe Left | Next page | - | - |
| Swipe Right | Previous page | - | - |
| Swipe Up | Larger font | Larger font | - |
| Swipe Down | Smaller font | Smaller font | - |
| Tap | Toggle UI | Toggle UI | Toggle UI |

Footer buttons (when UI visible): `|<<` `<` `Page` `>` `>>|`

---

## Testing

Run the integration test suite:
```bash
pip install pytest requests pillow

# Set device IP and run tests
PAPER_IP=192.168.1.100 pytest -s
```

---

## Credits

Built with:
- [M5Unified](https://github.com/m5stack/M5Unified) - M5Stack device library
- [ArduinoJson](https://arduinojson.org/) - JSON parsing
- [PubSubClient](https://pubsubclient.knolleary.net/) - MQTT client
- [PlatformIO](https://platformio.org/) - Build system

This project was generated using [OpenCode](https://opencode.ai/) with [Claude Opus](https://www.anthropic.com/claude). Code reviewed by Ryan Bateman.

## License

MIT License - see [LICENSE](LICENSE) for details.
