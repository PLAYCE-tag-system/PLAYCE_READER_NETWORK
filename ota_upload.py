"""Select the Wi-Fi interface and the Python permitted by this PC's firewall."""

import os
import socket
from pathlib import Path

Import("env")

# Keep the OTA password out of espota's debug options dump.
env.Replace(
    UPLOADERFLAGS=[
        flag for flag in env["UPLOADERFLAGS"] if flag not in ("--debug", "--progress")
    ]
)

target = env.subst("$UPLOAD_PORT")
if target:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect((target, 3232))
        host_ip = probe.getsockname()[0]
    env.Append(UPLOADERFLAGS=["-I", host_ip, "-P", "3233"])

platformio_python = Path(os.environ["USERPROFILE"]) / ".platformio" / "python3" / "python.exe"
if platformio_python.is_file():
    env.Replace(
        UPLOADCMD='"{}" "$UPLOADER" $UPLOADERFLAGS -f $SOURCE'.format(platformio_python)
    )
