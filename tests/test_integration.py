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
        assert "retain" in data, "Missing 'retain' field in status"
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

def test_retain_mode(check_ip):
    """Verify retain mode API works."""
    # Enable retain
    resp = requests.post(f"{BASE_URL}/api/retain", json={"retain": True}, timeout=5)
    assert resp.status_code == 200
    data = resp.json()
    assert data["retain"] == True, f"Retain should be True, got: {data}"
    
    # Verify via status
    status = requests.get(f"{BASE_URL}/api/status").json()
    assert status["retain"] == True, f"Status should show retain=True"
    
    # Disable retain
    resp = requests.post(f"{BASE_URL}/api/retain", json={"retain": False}, timeout=5)
    assert resp.status_code == 200
    data = resp.json()
    assert data["retain"] == False, f"Retain should be False, got: {data}"
    
    # Test toggle (no body)
    resp = requests.post(f"{BASE_URL}/api/retain", timeout=5)
    assert resp.status_code == 200
    data = resp.json()
    assert data["retain"] == True, f"Toggle should enable retain, got: {data}"
    
    # Toggle again
    resp = requests.post(f"{BASE_URL}/api/retain", timeout=5)
    assert resp.status_code == 200
    data = resp.json()
    assert data["retain"] == False, f"Toggle should disable retain, got: {data}"
    
    print("\nRetain mode API working correctly")

def test_stress_cycle(check_ip):
    """Rapidly cycle modes to check for stability."""
    print("\nStarting Stress Cycle...")
    for i in range(3):
        test_text_mode(check_ip)
        test_image_mode(check_ip)
        test_stream_mode(check_ip)
    
    final_status = requests.get(f"{BASE_URL}/api/status").json()
    print(f"Final Status: {final_status}")
    assert final_status["heap_free"] > 30000, f"Memory leak detected! {final_status['heap_free']}"
