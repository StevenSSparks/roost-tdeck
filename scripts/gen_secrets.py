Import("env")
import subprocess, os, sys

ITEMS = {
    "ANTHROPIC_API_KEY": "anthropic_api_key_sparkshost",
    "OPENAI_API_KEY": "OPENAI_API_KEY",     # optional (add to keychain if you use it)
    "GEMINI_API_KEY": "GEMINI_API_KEY",     # optional
    "GEOAPIFY_KEY": "gtoapify_key_sfehost",
    "DEFAULT_WIFI_PASS": "SSS-FAMILY",       # keychain service holding the home-WiFi password
}
# Fixed (not secrets): defaults baked in. SSS-FAMILY is the 2.4GHz network the
# ESP32-S3 connects to (SSS-MAIN refuses auth on the ESP32 — WiFi7/WPA3).
FIXED = {
    "DEFAULT_WIFI_SSID": "SSS-FAMILY",
    "OLLAMA_HOST": "ai.senzall.net:11434",   # local Ollama (host:port)
    "DEFAULT_AI_PROVIDER": "anthropic",
    "SSH_USER": "roost",                     # ssh <user>@<device-ip> (editable in Settings)
    "SSH_PASS": "roostos",
}

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

# Distribution-friendly: if the macOS keychain has none of the items (i.e. this is
# not the owner's Mac) but the user already created include/secrets.h by hand
# (copied from secrets.example.h and filled in), DO NOT clobber it.
if len(missing) == len(ITEMS) and os.path.exists(out_path):
    print("gen_secrets: keychain empty; keeping your existing include/secrets.h")
else:
    if missing:
        sys.stderr.write(
            "gen_secrets: keychain items not found: " + ", ".join(missing) +
            "\n  Either add them to the keychain, or copy include/secrets.example.h to\n"
            "  include/secrets.h and fill in your values by hand.\n")
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("gen_secrets: wrote", out_path)
