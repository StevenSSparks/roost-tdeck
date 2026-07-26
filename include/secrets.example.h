#pragma once
// TEMPLATE — copy to include/secrets.h (git-ignored) and fill in your values,
// OR (macOS owner) store them in the login keychain and let scripts/gen_secrets.py
// pull them at build time. Never commit include/secrets.h.

// --- WiFi (2.4 GHz network) ---
#define DEFAULT_WIFI_SSID ""
#define DEFAULT_WIFI_PASS ""

// --- AI providers (set the one(s) you use; leave others empty) ---
#define ANTHROPIC_API_KEY ""     // https://console.anthropic.com
#define OPENAI_API_KEY    ""     // https://platform.openai.com/api-keys
#define GEMINI_API_KEY    ""     // https://aistudio.google.com/app/apikey
#define OLLAMA_HOST       ""     // e.g. "192.168.52.50:11434" (local LLM, no key)

// Default provider on first boot: "anthropic" | "openai" | "gemini" | "ollama"
#define DEFAULT_AI_PROVIDER "anthropic"

// --- Maps (optional) ---
#define GEOAPIFY_KEY ""          // https://myprojects.geoapify.com
