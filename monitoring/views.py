import json
import socket

from django.conf import settings
from django.contrib import messages
from django.contrib.auth.decorators import login_required
from django.db import transaction
from django.http import JsonResponse
from django.shortcuts import redirect, render
from django.urls import reverse
from django.utils import timezone
from django.views.decorators.csrf import csrf_exempt
from django.views.decorators.http import require_GET, require_POST

from .models import Alert, Device, SensorReading, generate_api_key

VALID_STATUSES = {SensorReading.NORMAL, SensorReading.WARNING, SensorReading.DANGER}
ADC_MAX = 4095  # ESP32 12-bit ADC


# ------------------------------------------------------------------ helpers
def _error(message, status):
    return JsonResponse({"status": "error", "message": message}, status=status)


def _reading_dict(r):
    return {
        "id": r.id,
        "sensor_value": r.sensor_value,
        "water_status": r.water_status,
        "is_sample": r.is_sample,
        "created_at": r.created_at.isoformat(),
    }


def _alert_dict(a):
    return {
        "id": a.id,
        "level": a.level,
        "message": a.message,
        "device": a.reading.device.name if a.reading.device else None,
        "is_sample": a.is_sample,
        "created_at": a.created_at.isoformat(),
    }


def _device_dict(d):
    return {"id": d.id, "name": d.name, "location": d.location, "is_active": d.is_active}


def _limit(request, default, maximum):
    try:
        value = int(request.GET.get("limit", default))
    except (TypeError, ValueError):
        value = default
    return max(1, min(value, maximum))


def _devices_and_selected(request):
    """All devices, plus the one chosen with ?device=<id> (default: the first device)."""
    devices = list(Device.objects.all())
    wanted = request.GET.get("device", "")
    selected = None
    if wanted.isdigit():
        selected = next((d for d in devices if d.id == int(wanted)), None)
    if selected is None and devices:
        selected = devices[0]
    return devices, selected


def _connection(latest):
    """(connected, seconds_since_last) for a device's latest reading."""
    if latest is None:
        return False, None
    seconds = int((timezone.now() - latest.created_at).total_seconds())
    return (not latest.is_sample) and seconds <= settings.SMARTFLOOD_OFFLINE_AFTER_SECONDS, seconds


# ------------------------------------------------------------------ ESP32 API
@csrf_exempt  # the ESP32 has no CSRF cookie; it is authenticated by its device API key instead
@require_POST
def sensor_data(request):
    """POST /api/sensor-data/  with header X-API-Key (the DEVICE's key) and JSON body
    {"sensor_value": 450, "water_status": "WARNING"}"""
    supplied = request.headers.get("X-API-Key", "")
    device = Device.objects.filter(api_key=supplied).first() if supplied else None
    if device is None or not device.is_active:
        return _error("Invalid or missing API key.", 401)

    try:
        payload = json.loads(request.body.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return _error("Body must be valid JSON.", 400)
    if not isinstance(payload, dict):
        return _error("JSON body must be an object.", 400)

    value = payload.get("sensor_value")
    if isinstance(value, bool) or not isinstance(value, int):
        return _error("sensor_value must be an integer.", 400)
    if not 0 <= value <= ADC_MAX:
        return _error(f"sensor_value must be between 0 and {ADC_MAX}.", 400)

    status = payload.get("water_status")
    if not isinstance(status, str) or status.upper() not in VALID_STATUSES:
        return _error("water_status must be NORMAL, WARNING or DANGER.", 400)
    status = status.upper()

    with transaction.atomic():
        previous = SensorReading.objects.filter(device=device, is_sample=False).first()
        reading = SensorReading.objects.create(device=device, sensor_value=value, water_status=status)
        alert = None
        # One alert per status change (per device), not one every 5 seconds.
        if status != SensorReading.NORMAL and (previous is None or previous.water_status != status):
            alert = Alert.objects.create(
                reading=reading,
                level=status,
                message=f"Water level reached {status} (sensor value {value}).",
            )

    return JsonResponse(
        {
            "status": "ok",
            "id": reading.id,
            "device": device.name,
            "sensor_value": reading.sensor_value,
            "water_status": reading.water_status,
            "alert_created": alert is not None,
            "received_at": reading.created_at.isoformat(),
        },
        status=201,
    )


# ------------------------------------------------------------------ dashboard API
@require_GET
def api_latest(request):
    _, device = _devices_and_selected(request)
    latest = SensorReading.objects.filter(device=device).first() if device else None
    connected, seconds_since = _connection(latest)
    return JsonResponse(
        {
            "status": "ok",
            "has_data": latest is not None,
            "reading": _reading_dict(latest) if latest else None,
            "connected": connected,
            "seconds_since_last": seconds_since,
            "offline_after_seconds": settings.SMARTFLOOD_OFFLINE_AFTER_SECONDS,
            "device": _device_dict(device) if device else None,
        }
    )


@require_GET
def api_history(request):
    _, device = _devices_and_selected(request)
    limit = _limit(request, default=60, maximum=1000)
    rows = list(SensorReading.objects.filter(device=device)[:limit]) if device else []
    rows.reverse()  # oldest -> newest, ready for a time-series chart
    return JsonResponse({"status": "ok", "count": len(rows), "readings": [_reading_dict(r) for r in rows]})


@require_GET
def api_alerts(request):
    _, device = _devices_and_selected(request)
    limit = _limit(request, default=50, maximum=500)
    qs = Alert.objects.filter(reading__device=device).select_related("reading__device") if device else Alert.objects.none()
    rows = list(qs[:limit])
    return JsonResponse({"status": "ok", "count": len(rows), "alerts": [_alert_dict(a) for a in rows]})


@require_GET
def api_devices(request):
    """Overview of every device (no API keys)."""
    out = []
    for d in Device.objects.all():
        latest = d.readings.first()
        connected, seconds_since = _connection(latest)
        item = _device_dict(d)
        item.update(
            {
                "connected": connected and d.is_active,
                "seconds_since_last": seconds_since,
                "reading": _reading_dict(latest) if latest else None,
            }
        )
        out.append(item)
    return JsonResponse({"status": "ok", "devices": out})


# ------------------------------------------------------------------ pages
def _ctx(request, page, **extra):
    devices, selected = _devices_and_selected(request)
    return {"page": page, "devices": devices, "selected_device": selected, **extra}


def dashboard(request):
    return render(request, "monitoring/dashboard.html", _ctx(request, "dashboard"))


def history(request):
    return render(request, "monitoring/history.html", _ctx(request, "history"))


def alerts(request):
    return render(request, "monitoring/alerts.html", _ctx(request, "alerts"))


def device(request):
    return render(request, "monitoring/device.html", _ctx(request, "device"))


def about(request):
    return render(request, "monitoring/about.html", _ctx(request, "about"))


def _lan_ip():
    """This PC's LAN address (e.g. 192.168.1.50), or "" if it cannot be found. Sends no data."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("10.255.255.255", 1))  # UDP connect only picks a route; nothing is sent
            return sock.getsockname()[0]
    except OSError:
        return ""


@login_required
def manage_devices(request):
    """Add / delete devices and see their API keys. Login required, because keys are secrets."""
    if request.method == "POST":
        action = request.POST.get("action")
        if action == "add":
            name = request.POST.get("name", "").strip()[:60]
            location = request.POST.get("location", "").strip()[:120]
            if not name:
                messages.error(request, "Please enter a device name.")
            elif Device.objects.filter(name__iexact=name).exists():
                messages.error(request, f"A device named \u201c{name}\u201d already exists.")
            else:
                Device.objects.create(name=name, location=location)
                messages.success(request, f"Device \u201c{name}\u201d added. Open its setup steps below to send the key to the ESP32.")
        else:
            device_id = request.POST.get("device_id", "")
            target = Device.objects.filter(pk=int(device_id)).first() if device_id.isdigit() else None
            if target is None:
                messages.error(request, "Device not found.")
            elif action == "delete":
                target.delete()  # its readings and alerts are deleted too
                messages.success(request, f"Device \u201c{target.name}\u201d and its readings were deleted.")
            elif action == "regenerate":
                target.api_key = generate_api_key()
                target.save(update_fields=["api_key"])
                messages.success(request, f"New API key created for \u201c{target.name}\u201d. Send the new key to the ESP32 through its setup hotspot (no re-flashing).")
            elif action == "toggle":
                target.is_active = not target.is_active
                target.save(update_fields=["is_active"])
                messages.success(request, f"\u201c{target.name}\u201d is now {'enabled' if target.is_active else 'disabled'}.")
        return redirect("manage-devices")

    server_url = request.build_absolute_uri(reverse("api-sensor-data"))
    # The ESP32 cannot reach "127.0.0.1" / "localhost" (that would be itself), so swap in this PC's LAN IP.
    lan_ip = _lan_ip()
    for local in ("127.0.0.1", "localhost"):
        server_url = server_url.replace(local, lan_ip or "YOUR-PC-IP")
    return render(request, "monitoring/manage_devices.html", _ctx(request, "manage", server_url=server_url))
