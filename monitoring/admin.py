from django.contrib import admin

from .models import Alert, Device, SensorReading


@admin.register(Device)
class DeviceAdmin(admin.ModelAdmin):
    list_display = ("name", "location", "is_active", "created_at")
    readonly_fields = ("api_key",)


@admin.register(SensorReading)
class SensorReadingAdmin(admin.ModelAdmin):
    list_display = ("created_at", "device", "sensor_value", "water_status", "is_sample")
    list_filter = ("device", "water_status", "is_sample")
    date_hierarchy = "created_at"


@admin.register(Alert)
class AlertAdmin(admin.ModelAdmin):
    list_display = ("created_at", "level", "message", "is_sample")
    list_filter = ("level", "is_sample")
    date_hierarchy = "created_at"
