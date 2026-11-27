# SMARTFLOOD – ESP32 + Django

```
Water sensor -> ESP32 -> Wi-Fi -> Django API -> SQLite -> HTML/CSS/JS dashboard
```

## 1. Folder structure
```
smartflood/
├── manage.py
├── requirements.txt
├── smartflood/        settings.py  urls.py  wsgi.py
├── monitoring/        models.py views.py urls.py admin.py
│   ├── templates/monitoring/  base, dashboard, history, alerts, device, about
│   ├── static/monitoring/     css/style.css  js/dashboard.js
│   └── management/commands/load_sample_data.py
└── esp32/smartflood.ino
```

## 2. The contract (ESP32 and Django must match)
| Item | Value |
|---|---|
| URL | `POST /api/sensor-data/` (`SERVER_URL` in the .ino) |
| Header | `X-API-Key: <device key>` (each device has its own key from **Manage Devices**; paste it into `API_KEY` in that ESP32's .ino) |
| Body | `{"sensor_value": 450, "water_status": "WARNING"}` (`sensor_value` integer 0-4095; status NORMAL/WARNING/DANGER) |
| Success | HTTP 201 `{"status":"ok","id":1,"alert_created":true,...}` |
| Errors | 401 bad key, 400 bad data (`{"status":"error","message":"..."}`), 405 wrong method |

Dashboard APIs (read-only): `GET /api/latest/`, `GET /api/history/?limit=100`, `GET /api/alerts/?limit=50`.
These are public so the dashboard works without login; anyone with the link can view readings.

## 2b. Adding devices (multiple ESP32s)
1. Run `python manage.py makemigrations monitoring` then `python manage.py migrate` (adds the Device table).
2. Open `/manage-devices/` and log in with your `createsuperuser` account.
3. Fill in **Device name** and **Location**, click **Add device**. Use the **Copy** buttons on that page for the Server URL and API key, then enter them in the ESP32 setup hotspot (section 3b).
4. The dashboard shows all devices as cards; use the **Device** dropdown to switch the charts, history and alerts to one device.
5. **Disable** stops accepting data from a device; **New key** replaces a leaked key; **Delete** removes the device and its readings.
The old global `SMARTFLOOD_API_KEY` is no longer used.

## 3. Hardware
| Part | ESP32 pin |
|---|---|
| Water sensor AO | GPIO 34 (ADC1; works with Wi-Fi on) |
| Water sensor VCC / GND | **3V3** / GND |
| Green LED | GPIO 25 -> 220 ohm -> LED -> GND |
| Yellow LED | GPIO 26 -> 220 ohm -> LED -> GND |
| Red LED | GPIO 27 -> 220 ohm -> LED -> GND |
| Active buzzer | GPIO 14 -> 1k ohm -> NPN base (2N2222/BC547/S8050); emitter GND; collector -> buzzer "-"; buzzer "+" -> 3V3 |

Pins are exactly the suggested ones; nothing was changed.

Safety notes:
- Power the sensor from **3V3**. Its analog output follows its supply, so 5 V supply could put more than 3.3 V on GPIO 34 and damage the ESP32.
- LEDs need the 220 ohm resistors (about 5 mA each at 3.3 V).
- A bare active buzzer can draw more than a GPIO should supply, so use the transistor. If you have a 3-pin buzzer module with a built-in driver, wire VCC->3V3, GND->GND, I/O->GPIO 14 and skip the transistor.
- Sensor probes corrode when constantly powered. For long tests, unplug it between sessions.

### 3b. Changing Wi-Fi, server URL or API key (ESP32 setup hotspot)
No re-flashing needed. The ESP32 can open its own Wi-Fi hotspot with a settings page.

**When it opens**
- automatically on first boot (nothing saved yet),
- automatically if it cannot join the saved Wi-Fi at boot (wrong password, router moved, new location),
- any time you type **`setup`** in the Serial Monitor message box and press Send (115200 baud, line ending "Newline"),
- any time you **hold the BOOT button for 3 seconds**.

**How to use it**
1. On your phone/PC join Wi-Fi **`SmartFloodSetUp`**. Default password: **`smartflood123`** (change `AP_PASSWORD` in the sketch).
2. The setup page opens by itself; if not, browse to **http://192.168.4.1**.
3. Pick a network from the scanned list (or type the SSID), enter the Wi-Fi password, the **Server URL** (e.g. `http://192.168.1.50:8000/api/sensor-data/` or your PythonAnywhere URL) and the **Device API key** (from Manage Devices).
4. Press **Save & restart**. Settings are stored in the ESP32's flash and survive power loss.

Notes
- Blank password/API-key fields keep the saved value (a *new* SSID with a blank password is treated as an open network).
- The page also shows live status: Wi-Fi state, ESP32 IP, last server reply (e.g. 201 / 401), sensor value.
- **Erase saved settings** restores the defaults in the sketch and reopens the hotspot.
- The sensor, LEDs and buzzer keep working while the hotspot is open. If Wi-Fi is already configured, the hotspot closes itself after 10 minutes without use.
- Hotspot is WPA2-protected; the API key is sent to the ESP32 over that link only.

### Calibrate the sensor
1. Upload the sketch, open Serial Monitor at **115200** baud.
2. Dry sensor: note the value (this is your NORMAL baseline).
3. Dip the sensor to the level where you want WARNING; note the value. Repeat for DANGER.
4. Set `WARNING_THRESHOLD` and `DANGER_THRESHOLD` in the sketch, re-upload.
5. Always keep the same submersion method when testing. This gives raw readings, **not centimeters**; converting to cm needs your own measured calibration table.

## 4. Local setup (Windows; macOS/Linux use `python3` and `source venv/bin/activate`)
1. Install Python 3.10+ from python.org (tick "Add Python to PATH").
2. In a terminal inside the `smartflood` folder:
```
python -m venv venv
venv\Scripts\activate
pip install -r requirements.txt
python manage.py makemigrations monitoring
python manage.py migrate
python manage.py createsuperuser
python manage.py runserver 0.0.0.0:8000
```
3. Open http://127.0.0.1:8000 – you should see "Waiting for ESP32 data."
4. Optional demo: `python manage.py load_sample_data` (shows a yellow SAMPLE DATA banner); remove with `python manage.py load_sample_data --clear`.
5. Admin: http://127.0.0.1:8000/admin/

### Test the API without the ESP32
Windows PowerShell (use `curl.exe`):
```
curl.exe -X POST http://127.0.0.1:8000/api/sensor-data/ -H "Content-Type: application/json" -H "X-API-Key: <DEVICE-KEY-FROM-MANAGE-DEVICES>" -d "{\"sensor_value\": 450, \"water_status\": \"WARNING\"}"
```
macOS/Linux:
```
curl -X POST http://127.0.0.1:8000/api/sensor-data/ -H "Content-Type: application/json" -H "X-API-Key: <DEVICE-KEY-FROM-MANAGE-DEVICES>" -d '{"sensor_value": 450, "water_status": "WARNING"}'
```
Expect HTTP 201. A wrong key must give 401. The dashboard updates within 5 seconds.

### Connect the ESP32 locally
1. Install Arduino IDE 2.x. File > Preferences > Additional boards manager URLs: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`. Tools > Board > Boards Manager > install **esp32 by Espressif**.
2. Select board **ESP32 Dev Module** and the right COM port, open `esp32/smartflood.ino`.
3. Find your PC's LAN IP (`ipconfig` -> IPv4 Address, e.g. 192.168.1.50). **Do not use localhost** – inside the ESP32 it means the ESP32 itself.
4. Upload as-is. You do **not** need to edit Wi-Fi, server URL or API key in the code: enter them through the setup hotspot (section 3b). (The constants in the sketch are only defaults.)
5. Upload (hold BOOT if it says "Connecting..."). Open Serial Monitor at 115200.
6. The ESP32 and PC must be on the same network (a 2.4 GHz Wi-Fi; ESP32 does not support 5 GHz). If it fails: allow Python through Windows Firewall (private network), and avoid guest/campus Wi-Fi with client isolation (a phone hotspot works well).
7. Serial Monitor should show `HTTP 201`. Test the outputs: self-test blinks green, yellow, red and beeps at boot; then dip the sensor to see yellow, then red + beeping.
8. To change the Wi-Fi, server URL or device key later, use the setup hotspot (section 3b); no re-flashing needed.

## 5. Deploy on PythonAnywhere
1. Create an account, open a **Bash console**, upload your project (zip via Files tab, or `git clone`).
2. Create the environment (match the Python version you choose for the web app):
```
cd ~/smartflood
python3.10 -m venv ~/venv     # use the version shown in the Web tab
source ~/venv/bin/activate
pip install -r requirements.txt
```
3. Web tab > Add a new web app > **Manual configuration** (not the Django wizard) > pick the same Python version.
4. Set **Virtualenv** to `/home/YOURNAME/venv`, **Source code** to `/home/YOURNAME/smartflood`.
5. Edit the WSGI file (link on the Web tab) and replace its contents with:
```python
import os, sys
path = "/home/YOURNAME/smartflood"
if path not in sys.path:
    sys.path.insert(0, path)

os.environ["DJANGO_DEBUG"] = "0"
os.environ["DJANGO_SECRET_KEY"] = "paste-a-long-random-string"
os.environ["DJANGO_ALLOWED_HOSTS"] = "YOURNAME.pythonanywhere.com"
os.environ["SMARTFLOOD_API_KEY"] = "paste-a-long-random-key"
os.environ["SMARTFLOOD_DEVICE_NAME"] = "SMARTFLOOD-ESP32-01"
os.environ["SMARTFLOOD_LOCATION"] = "Your monitoring location"
os.environ["DJANGO_TIME_ZONE"] = "Asia/Manila"   # change to yours
os.environ["DJANGO_SETTINGS_MODULE"] = "smartflood.settings"

from django.core.wsgi import get_wsgi_application
application = get_wsgi_application()
```
6. In the Bash console, the same variables must be set when you run management commands:
```
export DJANGO_DEBUG=0 DJANGO_SECRET_KEY=... SMARTFLOOD_API_KEY=... DJANGO_ALLOWED_HOSTS=YOURNAME.pythonanywhere.com
python manage.py migrate
python manage.py collectstatic --noinput
python manage.py createsuperuser
```
7. Web tab > **Static files**: URL `/static/` -> directory `/home/YOURNAME/smartflood/staticfiles`.
8. Force HTTPS toggle on the Web tab (optional), then **Reload**.
9. ESP32: in the setup hotspot (section 3b) set Server URL to `https://YOURNAME.pythonanywhere.com/api/sensor-data/` and paste the device key. Test with curl first.

### Hosting limits to check
- PythonAnywhere's free accounts restrict **outbound** connections from your code to a whitelist. SMARTFLOOD only receives requests, so this should not matter, but confirm on PythonAnywhere's current help pages and test with curl before relying on it.
- Free web apps are limited in CPU time and may require a periodic "keep running" renewal on the Web tab; check the current plan limits.
- The sketch uses `setInsecure()` for HTTPS: traffic is encrypted but the server certificate is not verified. For stronger security, load the server's root certificate with `setCACert()`.
- The ESP32 sends every 5 s (about 17,000 rows/day). Raise `SEND_INTERVAL_MS` (e.g. 15000) for long runs on the free plan.
- SQLite is fine for a school project.
