from django.contrib.auth import views as auth_views
from django.urls import path

from . import views

urlpatterns = [
    # Pages
    path("", views.dashboard, name="dashboard"),
    path("history/", views.history, name="history"),
    path("alerts/", views.alerts, name="alerts"),
    path("device/", views.device, name="device"),
    path("about/", views.about, name="about"),
    path("manage-devices/", views.manage_devices, name="manage-devices"),
    path("login/", auth_views.LoginView.as_view(template_name="monitoring/login.html", extra_context={"page": "login"}), name="login"),
    path("logout/", auth_views.LogoutView.as_view(), name="logout"),
    # API
    path("api/sensor-data/", views.sensor_data, name="api-sensor-data"),  # ESP32 POSTs here
    path("api/latest/", views.api_latest, name="api-latest"),
    path("api/history/", views.api_history, name="api-history"),
    path("api/alerts/", views.api_alerts, name="api-alerts"),
    path("api/devices/", views.api_devices, name="api-devices"),
]
