Import("env")
import os

# For native tests, compile src files
if "native" in env.get("PIOENV"):
    env.Append(CPPPATH=["src"])
    # Add library include paths
    env.Append(CPPPATH=[
        os.path.join(env.get("PROJECT_LIBDEPS_DIR"), env.get("PIOENV"), "ArduinoJson", "src")
    ])
    env.BuildSources(
        os.path.join("$BUILD_DIR", "src_core"),
        "src/core"
    )
