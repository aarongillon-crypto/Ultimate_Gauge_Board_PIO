# PlatformIO post-build script: copy the freshly built firmware into releases/
# as version-stamped artifacts, so every build leaves a keepable binary behind
# and we never forget the manual copy.
#
#   releases/vX.Y.Z.bin          <- app/OTA image  (firmware.bin)
#   releases/vX.Y.Z.factory.bin  <- full flash img (firmware.factory.bin)
#
# Version comes from FIRMWARE_VERSION in src/app_state.h (single source of
# truth). releases/ is gitignored — these stay local, no binary bloat in git.
#
# This is registered post: so it runs AFTER the platform's factory-merge
# post-action, meaning firmware.factory.bin already exists when we copy.
import os
import re
import shutil

Import("env")  # noqa: F821 (SCons construction environment)

PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
RELEASES_DIR = os.path.join(PROJECT_DIR, "releases")
APP_STATE_H = os.path.join(PROJECT_DIR, "src", "app_state.h")


def read_version():
    """Pull FIRMWARE_VERSION "X.Y.Z" out of app_state.h."""
    try:
        with open(APP_STATE_H, "r", encoding="utf-8") as f:
            m = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', f.read())
            if m:
                return m.group(1)
    except OSError as e:
        print("copy_release: could not read app_state.h: %s" % e)
    return None


def after_build(source, target, env):
    version = read_version()
    if not version:
        print("copy_release: FIRMWARE_VERSION not found — skipping release copy")
        return

    os.makedirs(RELEASES_DIR, exist_ok=True)
    build_dir = env.subst("$BUILD_DIR")
    progname = env.subst("$PROGNAME")

    artifacts = [
        (progname + ".bin", "v%s.bin" % version),
        (progname + ".factory.bin", "v%s.factory.bin" % version),
    ]
    for src_name, dst_name in artifacts:
        src = os.path.join(build_dir, src_name)
        dst = os.path.join(RELEASES_DIR, dst_name)
        if os.path.isfile(src):
            shutil.copy2(src, dst)
            print("copy_release: releases/%s" % dst_name)
        else:
            # factory bin only exists for full builds; OTA-only builds skip it
            print("copy_release: %s not built — skipped" % src_name)


# Fire after the .bin is built (and after the platform's factory-merge, which
# is registered earlier and so runs first).
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", after_build)  # noqa: F821
