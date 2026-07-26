Import("env")
import subprocess, os, sys

ITEMS = {
    "ANTHROPIC_API_KEY": "anthropic_api_key_sparkshost",
    "GEOAPIFY_KEY": "gtoapify_key_sfehost",
    "DEFAULT_WIFI_PASS": "SSS-FAMILY",  # keychain service holding the home-WiFi password
}
# Fixed (not a secret): the home-WiFi SSID. SSS-FAMILY is the 2.4GHz network the
# ESP32-S3 connects to (SSS-MAIN refuses auth on the ESP32 — WPA3/MAC/enterprise).
FIXED = { "DEFAULT_WIFI_SSID": "SSS-FAMILY" }

def keychain(item):
    try:
        out = subprocess.check_output(
            ["security", "find-generic-password", "-s", item, "-w"],
            stderr=subprocess.DEVNULL)
        return out.decode().strip()
    except Exception:
        return None

out_path = os.path.join(env["PROJECT_INCLUDE_DIR"], "secrets.h")
lines = ["#pragma once", "// GENERATED — do not edit, do not commit."]
missing = []
for macro, item in ITEMS.items():
    val = keychain(item)
    if val is None:
        missing.append(item); val = ""
    lines.append(f'#define {macro} "{val}"')
for macro, val in FIXED.items():
    lines.append(f'#define {macro} "{val}"')

if missing:
    sys.stderr.write(
        "gen_secrets: keychain items not found: " + ", ".join(missing) +
        "\n  Add them, or build without DEV_SECRETS to type keys on-device.\n")
    # Non-fatal: allow non-DEV builds. DEV_SECRETS builds will read empty keys.
with open(out_path, "w") as f:
    f.write("\n".join(lines) + "\n")
print("gen_secrets: wrote", out_path)
