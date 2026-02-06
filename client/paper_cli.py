import argparse
import requests
import sys
import os

def main():
    parser = argparse.ArgumentParser(description="M5Stack PaperS3 Remote Display Client")
    subparsers = parser.add_subparsers(dest="command", required=True)
    
    # Text command
    text_parser = subparsers.add_parser("text", help="Send text to display")
    text_parser.add_argument("payload", nargs="?", help="Text to display")
    text_parser.add_argument("--size", type=int, default=3, help="Text size (default: 3)")
    text_parser.add_argument("--ip", help="IP address (overrides PAPER_IP env var)")
    
    # Image command
    img_parser = subparsers.add_parser("image", help="Send image")
    img_parser.add_argument("payload", nargs="?", help="Image file path (optional, reads from stdin if omitted)")
    img_parser.add_argument("--ip", help="IP address (overrides PAPER_IP env var)")
    
    # Stream command (Raw TCP)
    stream_parser = subparsers.add_parser("stream", help="Stream text line-by-line (tail -f)")
    stream_parser.add_argument("--ip", help="IP address (overrides PAPER_IP env var)")
    
    # Map command
    map_parser = subparsers.add_parser("map", help="Display a map at given coordinates or location")
    map_parser.add_argument("--lat", type=float, help="Latitude")
    map_parser.add_argument("--lon", type=float, help="Longitude")
    map_parser.add_argument("--location", type=str, help="Location name to geocode (e.g. 'Berlin, Germany')")
    map_parser.add_argument("--zoom", type=int, help="Zoom level (0-18, default based on location type)")
    map_parser.add_argument("--ip", help="IP address (overrides PAPER_IP env var)")
    map_parser.add_argument("--api-key", help="Stadia Maps API key (or set STADIA_API_KEY env var)")
    
    # MQTT command
    mqtt_parser = subparsers.add_parser("mqtt", help="Subscribe to MQTT topic and display messages")
    mqtt_parser.add_argument("--topic", required=True, help="MQTT topic to subscribe to")
    mqtt_parser.add_argument("--broker", required=True, help="MQTT broker hostname or IP")
    mqtt_parser.add_argument("--port", type=int, default=1883, help="MQTT broker port (default: 1883)")
    mqtt_parser.add_argument("--username", help="MQTT username (optional)")
    mqtt_parser.add_argument("--password", help="MQTT password (optional)")
    mqtt_parser.add_argument("--ip", help="IP address (overrides PAPER_IP env var)")

    args = parser.parse_args()
    
    # Resolve IP
    ip = args.ip or os.environ.get("PAPER_IP")
    if not ip:
        print("Error: Device IP must be provided via --ip or PAPER_IP environment variable.")
        sys.exit(1)
    
    base_url = f"http://{ip}/api"
    
    if args.command == "text":
        content = args.payload
        if not content:
            if not sys.stdin.isatty():
                import io
                input_stream = io.TextIOWrapper(sys.stdin.buffer, encoding='utf-8')
                content = input_stream.read().strip()
            else:
                print("Error: Provide text as argument or via stdin.")
                sys.exit(1)

        data = {
            "text": content,
            "size": args.size,
            "clear": True
        }
        try:
            print(f"Sending text to {base_url}/text...")
            resp = requests.post(f"{base_url}/text", json=data, timeout=5)
            resp.raise_for_status()
            print("Success!")
        except Exception as e:
            print(f"Error: {e}")

    elif args.command == "image":
        img_data = None
        
        # 1. Try reading from file argument
        if args.payload:
            if os.path.isfile(args.payload):
                try:
                    with open(args.payload, "rb") as f:
                        img_data = f.read()
                except Exception as e:
                    print(f"Error reading file: {e}")
                    sys.exit(1)
            else:
                print(f"Error: File not found: {args.payload}")
                sys.exit(1)
        
        # 2. Try reading from Stdin
        elif not sys.stdin.isatty():
             print(f"Reading image from stdin...")
             img_data = sys.stdin.buffer.read()
        
        else:
            print("Error: Provide an image file or pipe data.")
            print("Usage: python paper_cli.py image photo.jpg")
            print("   OR: cat photo.jpg | python paper_cli.py image")
            sys.exit(1)

        try:
            # Try importing Pillow
            try:
                from PIL import Image, ImageOps
                import io
                HAS_PILLOW = True
            except ImportError:
                HAS_PILLOW = False
                print("ERROR: Pillow library not found!", file=sys.stderr)
                print("The M5Stack PaperS3 requires 960x540 images.", file=sys.stderr)
                print("Please install Pillow to auto-resize your images:", file=sys.stderr)
                print("    pip install Pillow", file=sys.stderr)
                print("", file=sys.stderr)
                print("If you really want to send a raw image without resizing, use --force-raw", file=sys.stderr)
                
                # Check for force flag (hacky manual arg check)
                if "--force-raw" not in sys.argv:
                    sys.exit(1)
                print("Proceeding with raw upload (--force-raw detected)...", file=sys.stderr)

            if not img_data:
                print("Error: Empty input.")
                sys.exit(1)

            if HAS_PILLOW:
                try:
                    # Load Image
                    img = Image.open(io.BytesIO(img_data))
                    
                    # Target Resolution (M5PaperS3) - Use Max Dimension Box
                    # This allows preservation of resolution for both Portrait (540x960) and Landscape (960x540)
                    TARGET_W, TARGET_H = 960, 960
                    
                    # Convert to RGB (handle RGBA/P etc)
                    if img.mode != 'RGB':
                        img = img.convert('RGB')

                    # Resize logic: "Contain" (Fit within max dimensions, NO padding)
                    # This ensures the image takes up its natural space without arbitrary white bars
                    print(f"Processing: Original {img.size} -> Max {TARGET_W}x{TARGET_H}")
                    img = ImageOps.contain(img, (TARGET_W, TARGET_H), method=Image.Resampling.LANCZOS)
                    
                    # Convert back to bytes (JPEG)
                    out_io = io.BytesIO()
                    img.save(out_io, format='JPEG', quality=85)
                    img_data = out_io.getvalue()
                    print(f"Formatted size: {len(img_data)} bytes")
                except Exception as e:
                    print(f"Warning: Image processing failed ({e}). Sending raw data.", file=sys.stderr)
            
            print(f"Sending {len(img_data)} bytes to {base_url}/image...")
            files = {'file': ('image.jpg', img_data, 'application/octet-stream')}
            resp = requests.post(f"{base_url}/image", files=files, timeout=30)
            resp.raise_for_status()
            print("Success!")
        except Exception as e:
            print(f"Error: {e}")

    # Stream Mode
    elif args.command == "stream":
        import socket
        
        print(f"Connecting to Stream at {ip}:2323...")
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect((ip, 2323))
            print("Connected! Type or pipe text (Ctrl+C to stop).")
            
            while True:
                # Read from Stdin line by line (ideal for tail -f)
                line = sys.stdin.readline()
                if not line:
                    break # EOF
                
                s.sendall(line.encode('utf-8'))
                # No flush needed for generic socket usually
                
        except KeyboardInterrupt:
            print("\nDisconnected.")
        except Exception as e:
            print(f"Error: {e}")
        finally:
            s.close()

    # Map Mode
    elif args.command == "map":
        # Get API key
        api_key = args.api_key or os.environ.get("STADIA_API_KEY")
        if not api_key:
            print("Error: Stadia Maps API key required.", file=sys.stderr)
            print("Set via --api-key or STADIA_API_KEY environment variable.", file=sys.stderr)
            print("Get a free key at: https://client.stadiamaps.com/signup/", file=sys.stderr)
            sys.exit(1)
        
        # Resolve coordinates - either from --lat/--lon or --location
        lat, lon, zoom = args.lat, args.lon, args.zoom
        location_name = None
        
        if args.location:
            # Geocode the location using Nominatim (OpenStreetMap)
            print(f"Geocoding '{args.location}'...")
            try:
                geocode_url = "https://nominatim.openstreetmap.org/search"
                geocode_params = {
                    "q": args.location,
                    "format": "json",
                    "limit": 1
                }
                geocode_headers = {
                    "User-Agent": "PaperS3-Streamer/1.0"
                }
                geocode_resp = requests.get(geocode_url, params=geocode_params, headers=geocode_headers, timeout=10)
                geocode_resp.raise_for_status()
                results = geocode_resp.json()
                
                if not results:
                    print(f"Error: Location '{args.location}' not found.", file=sys.stderr)
                    sys.exit(1)
                
                result = results[0]
                location_name = result.get("display_name", args.location)
                
                # Calculate center from bounding box for better visual centering
                if "boundingbox" in result:
                    bbox = result["boundingbox"]  # [south, north, west, east]
                    lat = (float(bbox[0]) + float(bbox[1])) / 2  # center latitude
                    lon = (float(bbox[2]) + float(bbox[3])) / 2  # center longitude
                else:
                    # Fall back to point coordinates if no bounding box
                    lat = float(result["lat"])
                    lon = float(result["lon"])
                
                # Calculate appropriate zoom based on location type/bounding box
                if zoom is None:
                    # Use bounding box to determine zoom if available
                    if "boundingbox" in result:
                        bbox = result["boundingbox"]  # [south, north, west, east]
                        lat_diff = float(bbox[1]) - float(bbox[0])
                        lon_diff = float(bbox[3]) - float(bbox[2])
                        max_diff = max(lat_diff, lon_diff)
                        
                        # Approximate zoom from bounding box size
                        if max_diff > 10:
                            zoom = 5
                        elif max_diff > 5:
                            zoom = 7
                        elif max_diff > 1:
                            zoom = 10
                        elif max_diff > 0.1:
                            zoom = 13
                        elif max_diff > 0.01:
                            zoom = 15
                        else:
                            zoom = 16
                    else:
                        zoom = 14  # Default for named locations
                
                print(f"Found: {location_name}")
                print(f"Coordinates: {lat}, {lon} (zoom: {zoom})")
                
            except requests.exceptions.RequestException as e:
                print(f"Error: Geocoding failed: {e}", file=sys.stderr)
                sys.exit(1)
        
        elif lat is None or lon is None:
            print("Error: Either --lat and --lon, or --location must be provided.", file=sys.stderr)
            sys.exit(1)
        
        # Default zoom if not set
        if zoom is None:
            zoom = 15
        
        # Query device for current screen dimensions (handles rotation)
        try:
            print(f"Querying device status...")
            status_resp = requests.get(f"{base_url}/status", timeout=5)
            status_resp.raise_for_status()
            status = status_resp.json()
            width = status.get("screen_width", 960)
            height = status.get("screen_height", 540)
            print(f"Device screen: {width}x{height}")
        except Exception as e:
            print(f"Warning: Could not query device ({e}). Using default 960x540.", file=sys.stderr)
            width, height = 960, 540
        
        # Build Stadia Static Maps URL
        # Use @2x for sharper rendering on e-ink (request double resolution, send full size)
        url = (
            f"https://tiles.stadiamaps.com/static/stamen_toner.png"
            f"?center={lat},{lon}"
            f"&zoom={zoom}"
            f"&size={width}x{height}@2x"
            f"&markers={lat},{lon}"
            f"&api_key={api_key}"
        )
        
        print(f"Fetching map at ({lat}, {lon}) zoom {zoom}...")
        
        try:
            # Fetch map image
            resp = requests.get(url, timeout=30)
            resp.raise_for_status()
            img_data = resp.content
            
            # Process for e-ink display
            try:
                from PIL import Image, ImageEnhance
                import io
                
                img = Image.open(io.BytesIO(img_data))
                
                # Resize from @2x back to device dimensions
                # The @2x gives us sharper source, but we need to fit device screen
                if img.size != (width, height):
                    print(f"Resizing map from {img.size} to {width}x{height}...")
                    img = img.resize((width, height), Image.Resampling.LANCZOS)
                
                # Convert to grayscale (Stamen Toner is already B&W but ensure clean conversion)
                img = img.convert('L')
                
                # Enhance contrast for e-ink
                enhancer = ImageEnhance.Contrast(img)
                img = enhancer.enhance(1.2)
                
                # Convert back to RGB for JPEG encoding
                img = img.convert('RGB')
                
                # Save as JPEG
                out_io = io.BytesIO()
                img.save(out_io, format='JPEG', quality=90)
                img_data = out_io.getvalue()
                
            except ImportError:
                print("Warning: Pillow not installed. Sending map without enhancement.", file=sys.stderr)
            
            # Send to device
            print(f"Sending map ({len(img_data)} bytes) to {base_url}/image...")
            files = {'file': ('map.jpg', img_data, 'application/octet-stream')}
            resp = requests.post(f"{base_url}/image", files=files, timeout=30)
            resp.raise_for_status()
            print("Success! Map displayed.")
            
        except requests.exceptions.HTTPError as e:
            if e.response.status_code == 401:
                print("Error: Invalid API key.", file=sys.stderr)
            else:
                print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)
        except Exception as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)

    # MQTT Mode
    elif args.command == "mqtt":
        data = {
            "broker": args.broker,
            "topic": args.topic,
            "port": args.port,
        }
        
        if args.username:
            data["username"] = args.username
        if args.password:
            data["password"] = args.password
        
        print(f"Connecting device to MQTT broker {args.broker}:{args.port}...")
        print(f"Subscribing to topic: {args.topic}")
        
        try:
            resp = requests.post(f"{base_url}/mqtt", json=data, timeout=10)
            resp.raise_for_status()
            result = resp.json()
            
            if result.get("connected"):
                print("Success! Device connected to MQTT broker.")
                print(f"Broker: {result.get('broker')}")
                print(f"Topic: {result.get('topic')}")
                print("\nDevice will now display messages published to this topic.")
            else:
                print("Warning: Connection status unclear.", file=sys.stderr)
                print(f"Response: {result}")
                
        except requests.exceptions.HTTPError as e:
            print(f"Error: {e}", file=sys.stderr)
            try:
                error_detail = e.response.json()
                print(f"Detail: {error_detail.get('error', 'Unknown')}", file=sys.stderr)
            except:
                pass
            sys.exit(1)
        except Exception as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)

if __name__ == "__main__":
    main()
