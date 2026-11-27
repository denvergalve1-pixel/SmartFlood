import math
from datetime import timedelta

from django.core.management.base import BaseCommand
from django.utils import timezone

from monitoring.models import Alert, Device, SensorReading

SAMPLE_DEVICE = "Sample Device (demo)"


class Command(BaseCommand):
    help = "Create SAMPLE readings (flagged is_sample=True) to test the dashboard. Use --clear to remove them."

    def add_arguments(self, parser):
        parser.add_argument("--clear", action="store_true", help="Delete all sample data and exit.")

    def handle(self, *args, **options):
        if options["clear"]:
            SensorReading.objects.filter(is_sample=True).delete()  # alerts cascade
            Device.objects.filter(name=SAMPLE_DEVICE).delete()
            self.stdout.write(self.style.SUCCESS("Sample data removed."))
            return

        device, _ = Device.objects.get_or_create(name=SAMPLE_DEVICE, defaults={"location": "Demo only"})
        now = timezone.now()
        previous = None
        count = 60
        for i in range(count):
            when = now - timedelta(minutes=(count - i) * 2)
            value = int(1500 + 1500 * math.sin(i / 9.0)) + 750
            value = max(0, min(4095, value))
            status = "DANGER" if value >= 2400 else "WARNING" if value >= 1200 else "NORMAL"
            reading = SensorReading.objects.create(
                device=device, sensor_value=value, water_status=status, is_sample=True, created_at=when
            )
            if status != "NORMAL" and status != previous:
                Alert.objects.create(
                    reading=reading,
                    level=status,
                    message=f"[SAMPLE] Water level reached {status} (sensor value {value}).",
                    is_sample=True,
                    created_at=when,
                )
            previous = status
        self.stdout.write(self.style.SUCCESS(f"Created {count} SAMPLE readings."))
