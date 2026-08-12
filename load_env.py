Import("env")
import os

# Reads WIFI_SSID/WIFI_PASSWORD from a project-local, gitignored .env file
# (or from real environment variables, which take precedence) and bakes
# them into the firmware as WIFI_SSID_DEFAULT/WIFI_PASSWORD_DEFAULT.
#
# These are only used as the *initial* NVS default (see
# NetworkService::loadSettings() in src/network.cpp) - once a device's WiFi
# settings are saved via the web UI, the NVS-stored value always wins. This
# just lets a device join a known network on first boot without needing to
# join its AP-fallback hotspot at all.


def load_dotenv(path):
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip().strip('"').strip("'")
    return values


dotenv = load_dotenv(os.path.join(env.get("PROJECT_DIR"), ".env"))

wifi_ssid = os.environ.get("WIFI_SSID", dotenv.get("WIFI_SSID", ""))
wifi_password = os.environ.get("WIFI_PASSWORD", dotenv.get("WIFI_PASSWORD", ""))

if wifi_ssid:
    print("[load_env] Baking in WiFi default from .env (SSID: %s)" % wifi_ssid)


def c_string_literal(value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return '\\"%s\\"' % escaped


env.Append(CPPDEFINES=[
    ("WIFI_SSID_DEFAULT", c_string_literal(wifi_ssid)),
    ("WIFI_PASSWORD_DEFAULT", c_string_literal(wifi_password)),
])
