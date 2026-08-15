Import("env")
import os
import re
import shutil

# After every build, copies the produced firmware.bin into firmware/ as
# noiselight-<FIRMWARE_VERSION>.bin, so a ready-to-flash binary for each
# released version is always sitting in the repo - no PlatformIO/Arduino
# IDE needed to get one, just the WebUI's browser-based OTA uploader
# (see src/web.cpp, the /update endpoint).
#
# FIRMWARE_VERSION in include/config.h is the single source of truth for
# the version string; we scrape it with a regex rather than duplicating
# the literal here or in platformio.ini.

VERSION_RE = re.compile(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"')


def get_firmware_version(project_dir):
    config_path = os.path.join(project_dir, "include", "config.h")
    try:
        with open(config_path) as f:
            contents = f.read()
    except OSError as e:
        print("[copy_firmware_bin] Warning: could not read %s (%s) - skipping firmware copy" % (config_path, e))
        return None

    match = VERSION_RE.search(contents)
    if not match:
        print("[copy_firmware_bin] Warning: FIRMWARE_VERSION not found in %s - skipping firmware copy" % config_path)
        return None

    return match.group(1)


def copy_firmware(source, target, env):
    project_dir = env.get("PROJECT_DIR")
    version = get_firmware_version(project_dir)
    if not version:
        return

    firmware_dir = os.path.join(project_dir, "firmware")
    os.makedirs(firmware_dir, exist_ok=True)

    dest = os.path.join(firmware_dir, "noiselight-%s.bin" % version)
    shutil.copyfile(str(target[0]), dest)
    print("[copy_firmware_bin] Copied firmware to %s" % dest)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware)
