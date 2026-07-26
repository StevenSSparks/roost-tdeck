Import("env")
import os

# For native tests, compile src files
if "native" in env.get("PIOENV"):
    env.Append(CPPPATH=["src"])
    env.BuildSources(
        os.path.join("$BUILD_DIR", "src_core"),
        "src/core"
    )
