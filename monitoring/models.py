import secrets

from django.db import models
from django.utils import timezone


def generate_api_key():
    return secrets.token_urlsafe(24)


class Device(models.Model):
    """One ESP32. Each device has its own API key, so the server knows who sent a reading."""

    name = models.CharField(max_length=60, unique=True)
    location = models.CharField(max_length=120, blank=True)
    api_key = models.CharField(max_length=64, unique=True, default=generate_api_key, editable=False)
    is_active = models.BooleanField(default=True, help_text="Untick to stop accepting data from this device")
    created_at = models.DateTimeField(default=timezone.now)

    class Meta:
        ordering = ["name"]

    def __str__(self):
        return self.name


class SensorReading(models.Model):
    NORMAL = "NORMAL"
    WARNING = "WARNING"
    DANGER = "DANGER"
    STATUS_CHOICES = [(NORMAL, "Normal"), (WARNING, "Warning"), (DANGER, "Danger")]

    device = models.ForeignKey(Device, null=True, blank=True, on_delete=models.CASCADE, related_name="readings")
    sensor_value = models.PositiveIntegerField(help_text="Raw ADC value, 0-4095")
    water_status = models.CharField(max_length=10, choices=STATUS_CHOICES)
    is_sample = models.BooleanField(default=False, help_text="True for demo data only")
    created_at = models.DateTimeField(default=timezone.now, db_index=True)

    class Meta:
        ordering = ["-created_at", "-id"]

    def __str__(self):
        return f"{self.created_at:%Y-%m-%d %H:%M:%S} {self.water_status} ({self.sensor_value})"


class Alert(models.Model):
    WARNING = "WARNING"
    DANGER = "DANGER"
    LEVEL_CHOICES = [(WARNING, "Warning"), (DANGER, "Danger")]

    reading = models.ForeignKey(SensorReading, on_delete=models.CASCADE, related_name="alerts")
    level = models.CharField(max_length=10, choices=LEVEL_CHOICES)
    message = models.CharField(max_length=200)
    is_sample = models.BooleanField(default=False)
    created_at = models.DateTimeField(default=timezone.now, db_index=True)

    class Meta:
        ordering = ["-created_at", "-id"]

    def __str__(self):
        return f"{self.level} @ {self.created_at:%Y-%m-%d %H:%M:%S}"
