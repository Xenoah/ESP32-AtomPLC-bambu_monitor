Import("env")  # noqa: F821 — injected by PlatformIO
import os
import shutil

secrets  = os.path.join("src", "AppSecrets.h")
template = secrets + ".template"

if not os.path.exists(secrets):
    if os.path.exists(template):
        shutil.copy(template, secrets)
        print("")
        print("=" * 60)
        print("  Created src/AppSecrets.h from template.")
        print("  Fill in your Wi-Fi and printer credentials,")
        print("  then run the build again.")
        print("=" * 60)
        print("")
    else:
        print("WARNING: src/AppSecrets.h.template not found.")
