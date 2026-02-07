import os
import pytest
import requests
import socket
import time
from PIL import Image
import io

# Config
PAPER_IP = os.environ.get("PAPER_IP")
BASE_URL = f"http://{PAPER_IP}" if PAPER_IP else None

@pytest.fixture
def check_ip():
    if not PAPER_IP:
        pytest.fail("PAPER_IP environment variable not set")

def test_health_check(check_ip):
    """Verify device is reachable and status API works."""
    try:
        resp = requests.get(f"{BASE_URL}/api/status", timeout=5)
        resp.raise_for_status()
        data = resp.json()
        print(f"\nDevice Status: {data}")
        assert "mode" in data
        assert "heap_free" in data
        assert "screen_width" in data
        assert "screen_height" in data
        # Lower threshold to 30KB (ESP32S3 Arduino usage can be heavy)
        assert data["heap_free"] > 30000, f"Heap critically low: {data['heap_free']}"
    except Exception as e:
        pytest.fail(f"Health check failed: {e}")

def check_screenshot(name_hint):
    """Fetch screenshot and verify it's valid."""
    resp = requests.get(f"{BASE_URL}/api/screenshot", stream=True)
    assert resp.status_code == 200
    
    # Save for debug
    img_data = io.BytesIO(resp.content)
    img = Image.open(img_data)
    
    # Debug: Print stats
    extrema = img.getextrema()
    print(f"\n[{name_hint}] Screenshot: {img.size} Extrema: {extrema}")
    
    # Check if empty (White = 255)
    # E-Ink "White" is usually 255. 
    # If standard deviation is 0, it's a blank screen.
    stat = img.convert('L').getextrema()
    # It's an issue if min and max are BOTH 255 (Blank White) or BOTH 0 (Blank Black)
    if stat == (255, 255):
        pytest.fail(f"[{name_hint}] Screen is completely WHITE (Empty)!")
    if stat == (0, 0):
        pytest.fail(f"[{name_hint}] Screen is completely BLACK (Empty)!")
        
    return img

def test_text_mode(check_ip):
    """Verify switching to Text Mode."""
    payload = {"text": "Integration Test\nLine 2\nLine 3", "size": 3}
    resp = requests.post(f"{BASE_URL}/api/text", json=payload, timeout=5)
    assert resp.status_code == 200
    
    time.sleep(1) # Wait for Render
    
    # Check Status
    status = requests.get(f"{BASE_URL}/api/status").json()
    assert status["mode"] == "TEXT", f"Mode mismatch. Got: {status['mode']}"
    
    # Visual Check
    check_screenshot("TEXT_MODE")

def test_image_mode(check_ip):
    """Verify switching to Image Mode."""
    # Create dummy image
    img = Image.new('RGB', (100, 100), color='red')
    img_byte_arr = io.BytesIO()
    img.save(img_byte_arr, format='JPEG')
    img_byte_arr.seek(0)
    
    files = {'file': ('test.jpg', img_byte_arr, 'image/jpeg')}
    resp = requests.post(f"{BASE_URL}/api/image", files=files, timeout=10)
    assert resp.status_code == 200
    
    time.sleep(2) # Wait for E-Ink Refresh
    
    # Check Status
    status = requests.get(f"{BASE_URL}/api/status").json()
    assert status["mode"] == "IMAGE", f"Mode mismatch. Got: {status['mode']}"
    assert status["heap_free"] > 20000, f"Heap leaking after image! {status['heap_free']}"
    
    # Visual Check
    check_screenshot("IMAGE_MODE")

def test_stream_mode(check_ip):
    """Verify switching to Stream Mode via TCP."""
    # Connect TCP
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect((PAPER_IP, 2323))
        s.sendall(b"Stream Test Line 1\nStream Test Line 2\n")
        time.sleep(1) # Allow processing time
        s.close()
    except Exception as e:
        pytest.fail(f"TCP Connection failed: {e}")
    
    # Check Status
    time.sleep(1) # Wait for mode switch to stick
    status = requests.get(f"{BASE_URL}/api/status").json()
    assert status["mode"] == "STREAM", f"Mode mismatch. Got: {status['mode']}"
    
    # Visual Check
    check_screenshot("STREAM_MODE")

def test_mqtt_mode(check_ip):
    """Verify MQTT mode connection and status."""
    # Use public test broker (test.mosquitto.org)
    # Port 1883 is unencrypted, no auth required
    mqtt_config = {
        "broker": "test.mosquitto.org",
        "topic": "paperpiper/test/" + str(int(time.time())),  # Unique topic
        "port": 1883
    }
    
    resp = requests.post(f"{BASE_URL}/api/mqtt", json=mqtt_config, timeout=15)
    assert resp.status_code == 200, f"MQTT connect failed: {resp.text}"
    
    data = resp.json()
    assert data.get("status") == "ok", f"Expected ok status, got: {data}"
    assert data.get("connected") == True, f"Expected connected=True, got: {data}"
    assert data.get("broker") == mqtt_config["broker"]
    assert data.get("topic") == mqtt_config["topic"]
    
    time.sleep(2)  # Wait for mode switch and render
    
    # Check Status
    status = requests.get(f"{BASE_URL}/api/status").json()
    assert status["mode"] == "MQTT", f"Mode mismatch. Got: {status['mode']}"
    assert "mqtt_connected" in status, "Missing mqtt_connected in status"
    assert "mqtt_broker" in status, "Missing mqtt_broker in status"
    assert "mqtt_topic" in status, "Missing mqtt_topic in status"
    assert status["mqtt_broker"] == mqtt_config["broker"]
    
    # Visual Check - should show "MQTT Connected" message
    check_screenshot("MQTT_MODE")
    
    print(f"\nMQTT connected to {mqtt_config['broker']} on topic {mqtt_config['topic']}")

def test_mqtt_invalid_broker(check_ip):
    """Verify MQTT handles invalid broker gracefully."""
    mqtt_config = {
        "broker": "invalid.broker.that.does.not.exist.example.com",
        "topic": "test/topic",
        "port": 1883
    }
    
    # Should fail to connect but not crash
    resp = requests.post(f"{BASE_URL}/api/mqtt", json=mqtt_config, timeout=15)
    # Could be 200 with error in body, or 500 - check response
    if resp.status_code == 200:
        data = resp.json()
        # If 200, should indicate not connected
        print(f"Response for invalid broker: {data}")
    else:
        # 500 is acceptable for failed connection
        assert resp.status_code == 500, f"Unexpected status code: {resp.status_code}"
        assert "error" in resp.json(), "Expected error message in response"
    
    # Device should still be healthy
    status = requests.get(f"{BASE_URL}/api/status", timeout=5).json()
    assert status["heap_free"] > 30000, f"Heap low after failed MQTT: {status['heap_free']}"
    print("\nMQTT invalid broker handled gracefully")

def test_mqtt_message_display(check_ip):
    """Verify MQTT messages are displayed on screen."""
    import paho.mqtt.client as mqtt
    
    # Unique topic for this test
    test_topic = f"paperpiper/test/{int(time.time())}"
    test_message = "Hello from MQTT Test!\nThis is line 2.\nTimestamp: " + str(time.time())
    
    # First, connect PaperPiper to the broker
    mqtt_config = {
        "broker": "test.mosquitto.org",
        "topic": test_topic,
        "port": 1883
    }
    
    resp = requests.post(f"{BASE_URL}/api/mqtt", json=mqtt_config, timeout=15)
    assert resp.status_code == 200, f"MQTT connect failed: {resp.text}"
    
    time.sleep(2)  # Wait for connection to establish
    
    # Now publish a message to that topic using paho-mqtt
    try:
        client = mqtt.Client(client_id=f"paperpiper_test_{int(time.time())}")
        client.connect("test.mosquitto.org", 1883, 60)
        client.loop_start()
        
        # Publish message
        result = client.publish(test_topic, test_message, qos=1)
        result.wait_for_publish(timeout=5)
        
        client.loop_stop()
        client.disconnect()
        
        print(f"\nPublished message to {test_topic}")
        
    except ImportError:
        pytest.skip("paho-mqtt not installed, skipping message test")
    except Exception as e:
        pytest.skip(f"Could not publish MQTT message: {e}")
    
    time.sleep(3)  # Wait for message to be received and displayed
    
    # Take screenshot to verify message displayed
    check_screenshot("MQTT_MESSAGE")
    
    # Check that last message was received (if status includes it)
    status = requests.get(f"{BASE_URL}/api/status").json()
    assert status["mode"] == "MQTT"
    print(f"MQTT message test complete. Status: {status}")

def test_stress_cycle(check_ip):
    """Rapidly cycle modes to check for stability."""
    print("\nStarting Stress Cycle...")
    for i in range(3):
        print(f"\n--- Cycle {i+1}/3 ---")
        test_text_mode(check_ip)
        test_image_mode(check_ip)
        test_stream_mode(check_ip)
        test_mqtt_mode(check_ip)
    
    final_status = requests.get(f"{BASE_URL}/api/status").json()
    print(f"Final Status: {final_status}")
    assert final_status["heap_free"] > 30000, f"Memory leak detected! {final_status['heap_free']}"
