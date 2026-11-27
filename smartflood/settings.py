"""
SMARTFLOOD settings.

Everything that differs between your laptop and PythonAnywhere is read from
environment variables, so the same file works in both places.
"""
import os
from pathlib import Path

from django.core.exceptions import ImproperlyConfigured

BASE_DIR = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------- core
DEBUG = os.environ.get("DJANGO_DEBUG", "1") == "1"  # set DJANGO_DEBUG=0 online

SECRET_KEY = os.environ.get("DJANGO_SECRET_KEY", "dev-only-insecure-key-change-me")

# Comma-separated. Local example: "localhost,127.0.0.1,192.168.1.50"
# PythonAnywhere example: "yourusername.pythonanywhere.com"
# In local DEBUG mode any host is accepted so the ESP32 can reach your PC by IP.
_hosts = os.environ.get("DJANGO_ALLOWED_HOSTS", "*" if DEBUG else "")
ALLOWED_HOSTS = [h.strip() for h in _hosts.split(",") if h.strip()]

# ---------------------------------------------------------------- SMARTFLOOD
# Each ESP32 now has its OWN API key, created on the "Manage Devices" page.

if not DEBUG and SECRET_KEY.startswith("dev-only"):
    raise ImproperlyConfigured("Set DJANGO_SECRET_KEY when DJANGO_DEBUG=0.")

# Dashboard shows "Disconnected" if no reading arrived within this many seconds.
SMARTFLOOD_OFFLINE_AFTER_SECONDS = int(os.environ.get("SMARTFLOOD_OFFLINE_AFTER", "30"))

# ---------------------------------------------------------------- apps
INSTALLED_APPS = [
    "django.contrib.admin",
    "django.contrib.auth",
    "django.contrib.contenttypes",
    "django.contrib.sessions",
    "django.contrib.messages",
    "django.contrib.staticfiles",
    "monitoring",
]

MIDDLEWARE = [
    "django.middleware.security.SecurityMiddleware",
    "django.contrib.sessions.middleware.SessionMiddleware",
    "django.middleware.common.CommonMiddleware",
    "django.middleware.csrf.CsrfViewMiddleware",
    "django.contrib.auth.middleware.AuthenticationMiddleware",
    "django.contrib.messages.middleware.MessageMiddleware",
    "django.middleware.clickjacking.XFrameOptionsMiddleware",
]

ROOT_URLCONF = "smartflood.urls"

TEMPLATES = [
    {
        "BACKEND": "django.template.backends.django.DjangoTemplates",
        "DIRS": [],
        "APP_DIRS": True,
        "OPTIONS": {
            "context_processors": [
                "django.template.context_processors.debug",
                "django.template.context_processors.request",
                "django.contrib.auth.context_processors.auth",
                "django.contrib.messages.context_processors.messages",
            ],
        },
    },
]

WSGI_APPLICATION = "smartflood.wsgi.application"

# ---------------------------------------------------------------- database
DATABASES = {
    "default": {
        "ENGINE": "django.db.backends.sqlite3",
        "NAME": BASE_DIR / "db.sqlite3",
    }
}

# ---------------------------------------------------------------- misc
AUTH_PASSWORD_VALIDATORS = [
    {"NAME": "django.contrib.auth.password_validation.UserAttributeSimilarityValidator"},
    {"NAME": "django.contrib.auth.password_validation.MinimumLengthValidator"},
    {"NAME": "django.contrib.auth.password_validation.CommonPasswordValidator"},
    {"NAME": "django.contrib.auth.password_validation.NumericPasswordValidator"},
]

LANGUAGE_CODE = "en-us"
TIME_ZONE = os.environ.get("DJANGO_TIME_ZONE", "UTC")  # e.g. "Asia/Manila"
USE_I18N = True
USE_TZ = True

STATIC_URL = "static/"
STATIC_ROOT = BASE_DIR / "staticfiles"  # `collectstatic` target (PythonAnywhere)

DEFAULT_AUTO_FIELD = "django.db.models.BigAutoField"

LOGIN_URL = "login"
LOGIN_REDIRECT_URL = "manage-devices"
LOGOUT_REDIRECT_URL = "dashboard"
