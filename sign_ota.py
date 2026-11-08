# PlatformIO post-build hook: sign firmware.bin the way idf.py does with
# CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES (Secure Boot V2 RSA-3072 block), so
# the OTA check in main/ota.c accepts it. PlatformIO's espidf builder does not
# sign. The key is keys/ota_signing_key.pem (gitignored).
import os
import subprocess

Import("env")  # noqa: F821  (provided by SCons)

KEY = os.path.join(env.subst("$PROJECT_DIR"), "keys", "ota_signing_key.pem")  # noqa: F821
ESPSECURE = os.path.join(
    env.PioPlatform().get_package_dir("tool-esptoolpy"), "espsecure.py"  # noqa: F821
)


def espsecure(*args):
    return subprocess.run(
        [env.subst("$PYTHONEXE"), ESPSECURE, *args],  # noqa: F821
        capture_output=True, text=True,
    )


def sign(target, source, env):
    if not os.path.isfile(KEY):
        env.Exit("sign_ota: %s missing; OTA images must be signed" % KEY)
    image = str(target[0])
    if espsecure("verify_signature", "--version", "2", "--keyfile", KEY, image).returncode == 0:
        return  # already signed
    signed = image + ".signed"
    r = espsecure("sign_data", "--version", "2", "--keyfile", KEY, "--output", signed, image)
    if r.returncode != 0:
        env.Exit("sign_ota: signing failed:\n" + r.stdout + r.stderr)
    os.replace(signed, image)
    print("sign_ota: signed %s" % image)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", sign)  # noqa: F821
