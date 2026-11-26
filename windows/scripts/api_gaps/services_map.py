#!/usr/bin/env python3
"""services_map.py: which Context.getSystemService names Win.apk answers.

What it does
  1. Reads every `public static final String *_SERVICE` constant of android.content.Context from one or
     more SDK android.jar files (the class file's ConstantValue, no javap needed) and the level each
     field was added at from the api-versions.xml beside each jar (platform/data/api-versions.xml).
  2. Reads the `case` labels of Win.apk's answers:
       - runtime/java/android/content/Context.java, getSystemService(String)'s switch (AOSP's
         ContextImpl.getSystemService's body becomes this one: windows/scripts/AospAppRewrite.java,
         SERVICE_LOOKUPS), and getSystemServiceName(Class)'s explicit class -> name map;
       - windows/java/aosp_views/com/winapk/aospviews/AospViewHost.java, systemService(), which
         Context asks first (window, layout_inflater, input_method, accessibility...).
  3. Prints one line per name: constant, string, level, who answers (viewhost / context / -), and
     with --apks, which of the given APKs carry the name as a string in their classes*.dex string
     pools (a "referenced" signal, not proof of a call). Then the names nobody answers.

  A name a `case` answers can still be null at run time (a reflective constructor that fails, an
  instance a peek returns); the script reads the source, it does not run it.

How to run (Python 3 stdlib only)
  python3 -I windows/scripts/api_gaps/services_map.py \
      --sdk <dir>/android-34 --sdk <dir>/android-35 --sdk <dir>/android-36 \
      [--repo /path/to/win.apk] [--apks <dir with .apk files>] [--tsv out.tsv]
  Each --sdk is a platform directory holding android.jar and data/api-versions.xml.
"""

import argparse
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile

CONTEXT_CLASS = "android/content/Context.class"


# ---- android.jar: Context's String constants -------------------------------------------------

def _read_constant_pool(data, off):
    """Returns (pool, offset after the pool). pool[i] = (tag, value)."""
    count = struct.unpack_from(">H", data, off)[0]
    off += 2
    pool = [None] * count
    i = 1
    while i < count:
        tag = data[off]
        off += 1
        if tag == 1:  # Utf8 (modified UTF-8; constant names and values here are ASCII)
            ln = struct.unpack_from(">H", data, off)[0]
            pool[i] = (tag, data[off + 2:off + 2 + ln].decode("utf-8", "replace"))
            off += 2 + ln
        elif tag in (3, 4):  # Integer, Float
            pool[i] = (tag, data[off:off + 4])
            off += 4
        elif tag in (5, 6):  # Long, Double take two slots
            pool[i] = (tag, data[off:off + 8])
            off += 8
            i += 1
        elif tag in (7, 8, 16, 19, 20):  # Class, String, MethodType, Module, Package
            pool[i] = (tag, struct.unpack_from(">H", data, off)[0])
            off += 2
        elif tag in (9, 10, 11, 12, 17, 18):  # refs, NameAndType, (Invoke)Dynamic
            pool[i] = (tag, struct.unpack_from(">HH", data, off))
            off += 4
        elif tag == 15:  # MethodHandle
            pool[i] = (tag, data[off:off + 3])
            off += 3
        else:
            raise ValueError("unknown constant pool tag %d" % tag)
        i += 1
    return pool, off


def context_string_constants(jar_path):
    """{field name: value} for Context's public static final String fields that have a value."""
    with zipfile.ZipFile(jar_path) as z:
        data = z.read(CONTEXT_CLASS)
    pool, off = _read_constant_pool(data, 8)
    off += 6  # access, this, super
    n_if = struct.unpack_from(">H", data, off)[0]
    off += 2 + 2 * n_if
    n_fields = struct.unpack_from(">H", data, off)[0]
    off += 2
    out = {}
    for _ in range(n_fields):
        acc, name_i, desc_i, n_attr = struct.unpack_from(">HHHH", data, off)
        off += 8
        value = None
        for _ in range(n_attr):
            an_i, alen = struct.unpack_from(">HI", data, off)
            if pool[an_i][1] == "ConstantValue":
                cv = struct.unpack_from(">H", data, off + 6)[0]
                if pool[cv][0] == 8:
                    value = pool[pool[cv][1]][1]
            off += 6 + alen
        public_static_final = (acc & 0x0019) == 0x0019
        if public_static_final and pool[desc_i][1] == "Ljava/lang/String;" and value is not None:
            out[pool[name_i][1]] = value
    return out


def context_field_levels(api_versions_xml):
    """{field name: level added} for android/content/Context, from api-versions.xml."""
    levels = {}
    for _, el in ET.iterparse(api_versions_xml):
        if el.tag == "class" and el.get("name") == "android/content/Context":
            cls_since = el.get("since", "1")
            for f in el.findall("field"):
                levels[f.get("name")] = f.get("since", cls_since)
            break
        if el.tag == "class":
            el.clear()
    return levels


# The class each name hands out on Android (Context's javadoc for the constant, SystemServiceRegistry).
# Used for getSystemService(Class): whether Win.apk's getSystemServiceName(Class) gives the name back,
# and with --apks whether an app's dex names the class itself (a stronger signal than the string).
MANAGER_CLASS = {
    "ACCESSIBILITY_SERVICE": "android.view.accessibility.AccessibilityManager",
    "ACCOUNT_SERVICE": "android.accounts.AccountManager",
    "ACTIVITY_SERVICE": "android.app.ActivityManager",
    "ADVANCED_PROTECTION_SERVICE": "android.security.advancedprotection.AdvancedProtectionManager",
    "ALARM_SERVICE": "android.app.AlarmManager",
    "APPWIDGET_SERVICE": "android.appwidget.AppWidgetManager",
    "APP_FUNCTION_SERVICE": "android.app.appfunctions.AppFunctionManager",
    "APP_OPS_SERVICE": "android.app.AppOpsManager",
    "APP_SEARCH_SERVICE": "android.app.appsearch.AppSearchManager",
    "AUDIO_SERVICE": "android.media.AudioManager",
    "BATTERY_SERVICE": "android.os.BatteryManager",
    "BIOMETRIC_SERVICE": "android.hardware.biometrics.BiometricManager",
    "BLOB_STORE_SERVICE": "android.app.blob.BlobStoreManager",
    "BLUETOOTH_SERVICE": "android.bluetooth.BluetoothManager",
    "BUGREPORT_SERVICE": "android.os.BugreportManager",
    "CAMERA_SERVICE": "android.hardware.camera2.CameraManager",
    "CAPTIONING_SERVICE": "android.view.accessibility.CaptioningManager",
    "CARRIER_CONFIG_SERVICE": "android.telephony.CarrierConfigManager",
    "CLIPBOARD_SERVICE": "android.content.ClipboardManager",
    "COMPANION_DEVICE_SERVICE": "android.companion.CompanionDeviceManager",
    "CONNECTIVITY_DIAGNOSTICS_SERVICE": "android.net.ConnectivityDiagnosticsManager",
    "CONNECTIVITY_SERVICE": "android.net.ConnectivityManager",
    "CONSUMER_IR_SERVICE": "android.hardware.ConsumerIrManager",
    "CONTACT_KEYS_SERVICE": "android.provider.E2eeContactKeysManager",
    "CREDENTIAL_SERVICE": "android.credentials.CredentialManager",
    "CROSS_PROFILE_APPS_SERVICE": "android.content.pm.CrossProfileApps",
    "DEVICE_LOCK_SERVICE": "android.devicelock.DeviceLockManager",
    "DEVICE_POLICY_SERVICE": "android.app.admin.DevicePolicyManager",
    "DISPLAY_HASH_SERVICE": "android.view.displayhash.DisplayHashManager",
    "DISPLAY_SERVICE": "android.hardware.display.DisplayManager",
    "DOMAIN_VERIFICATION_SERVICE": "android.content.pm.verify.domain.DomainVerificationManager",
    "DOWNLOAD_SERVICE": "android.app.DownloadManager",
    "DROPBOX_SERVICE": "android.os.DropBoxManager",
    "EUICC_SERVICE": "android.telephony.euicc.EuiccManager",
    "FILE_INTEGRITY_SERVICE": "android.security.FileIntegrityManager",
    "FINGERPRINT_SERVICE": "android.hardware.fingerprint.FingerprintManager",
    "GAME_SERVICE": "android.app.GameManager",
    "GRAMMATICAL_INFLECTION_SERVICE": "android.app.GrammaticalInflectionManager",
    "HARDWARE_PROPERTIES_SERVICE": "android.os.HardwarePropertiesManager",
    "HEALTHCONNECT_SERVICE": "android.health.connect.HealthConnectManager",
    "INPUT_METHOD_SERVICE": "android.view.inputmethod.InputMethodManager",
    "INPUT_SERVICE": "android.hardware.input.InputManager",
    "IPSEC_SERVICE": "android.net.IpSecManager",
    "JOB_SCHEDULER_SERVICE": "android.app.job.JobScheduler",
    "KEYGUARD_SERVICE": "android.app.KeyguardManager",
    "KEYSTORE_SERVICE": "android.security.keystore.KeyStoreManager",
    "LAUNCHER_APPS_SERVICE": "android.content.pm.LauncherApps",
    "LAYOUT_INFLATER_SERVICE": "android.view.LayoutInflater",
    "LOCALE_SERVICE": "android.app.LocaleManager",
    "LOCATION_SERVICE": "android.location.LocationManager",
    "MEDIA_COMMUNICATION_SERVICE": "android.media.MediaCommunicationManager",
    "MEDIA_METRICS_SERVICE": "android.media.metrics.MediaMetricsManager",
    "MEDIA_PROJECTION_SERVICE": "android.media.projection.MediaProjectionManager",
    "MEDIA_QUALITY_SERVICE": "android.media.quality.MediaQualityManager",
    "MEDIA_ROUTER_SERVICE": "android.media.MediaRouter",
    "MEDIA_SESSION_SERVICE": "android.media.session.MediaSessionManager",
    "MIDI_SERVICE": "android.media.midi.MidiManager",
    "NETWORK_STATS_SERVICE": "android.app.usage.NetworkStatsManager",
    "NFC_SERVICE": "android.nfc.NfcManager",
    "NOTIFICATION_SERVICE": "android.app.NotificationManager",
    "NSD_SERVICE": "android.net.nsd.NsdManager",
    "OVERLAY_SERVICE": "android.content.om.OverlayManager",
    "PEOPLE_SERVICE": "android.app.people.PeopleManager",
    "PERFORMANCE_HINT_SERVICE": "android.os.PerformanceHintManager",
    "PERSISTENT_DATA_BLOCK_SERVICE": "android.service.persistentdata.PersistentDataBlockManager",
    "POWER_SERVICE": "android.os.PowerManager",
    "PRINT_SERVICE": "android.print.PrintManager",
    "PROFILING_SERVICE": "android.os.ProfilingManager",
    "RESTRICTIONS_SERVICE": "android.content.RestrictionsManager",
    "ROLE_SERVICE": "android.app.role.RoleManager",
    "SATELLITE_SERVICE": "android.telephony.satellite.SatelliteManager",
    "SEARCH_SERVICE": "android.app.SearchManager",
    "SECURITY_STATE_SERVICE": "android.os.SecurityStateManager",
    "SENSOR_SERVICE": "android.hardware.SensorManager",
    "SHORTCUT_SERVICE": "android.content.pm.ShortcutManager",
    "STATUS_BAR_SERVICE": "android.app.StatusBarManager",
    "STORAGE_SERVICE": "android.os.storage.StorageManager",
    "STORAGE_STATS_SERVICE": "android.app.usage.StorageStatsManager",
    "SYSTEM_HEALTH_SERVICE": "android.os.health.SystemHealthManager",
    "TELECOM_SERVICE": "android.telecom.TelecomManager",
    "TELEPHONY_IMS_SERVICE": "android.telephony.ims.ImsManager",
    "TELEPHONY_SERVICE": "android.telephony.TelephonyManager",
    "TELEPHONY_SUBSCRIPTION_SERVICE": "android.telephony.SubscriptionManager",
    "TETHERING_SERVICE": "android.net.TetheringManager",
    "TEXT_CLASSIFICATION_SERVICE": "android.view.textclassifier.TextClassificationManager",
    "TEXT_SERVICES_MANAGER_SERVICE": "android.view.textservice.TextServicesManager",
    "TV_AD_SERVICE": "android.media.tv.ad.TvAdManager",
    "TV_INPUT_SERVICE": "android.media.tv.TvInputManager",
    "TV_INTERACTIVE_APP_SERVICE": "android.media.tv.interactive.TvInteractiveAppManager",
    "UI_MODE_SERVICE": "android.app.UiModeManager",
    "USAGE_STATS_SERVICE": "android.app.usage.UsageStatsManager",
    "USB_SERVICE": "android.hardware.usb.UsbManager",
    "USER_SERVICE": "android.os.UserManager",
    "VIBRATOR_MANAGER_SERVICE": "android.os.VibratorManager",
    "VIBRATOR_SERVICE": "android.os.Vibrator",
    "VIRTUAL_DEVICE_SERVICE": "android.companion.virtual.VirtualDeviceManager",
    "VPN_MANAGEMENT_SERVICE": "android.net.VpnManager",
    "WALLPAPER_SERVICE": "android.app.WallpaperManager",
    "WIFI_AWARE_SERVICE": "android.net.wifi.aware.WifiAwareManager",
    "WIFI_P2P_SERVICE": "android.net.wifi.p2p.WifiP2pManager",
    "WIFI_RTT_RANGING_SERVICE": "android.net.wifi.rtt.WifiRttManager",
    "WIFI_SERVICE": "android.net.wifi.WifiManager",
    "WINDOW_SERVICE": "android.view.WindowManager",
}

# Public managers that have no public name constant and are reached only through getSystemService(Class)
# (their constants are @hide): class -> the name Android binds them to.
CLASS_ONLY = {
    "android.view.autofill.AutofillManager": "autofill",
    "android.view.contentcapture.ContentCaptureManager": "content_capture",
    "android.view.translation.TranslationManager": "translation",
    "android.view.translation.UiTranslationManager": "ui_translation",
}


# Win.apk's Context's own constants that the public SDK hides (the case labels use them).
HIDDEN_NAMES = {
    "AUTOFILL_MANAGER_SERVICE": "autofill",
    "AUTOFILL_SERVICE": "autofill",
    "ROLLBACK_SERVICE": "rollback",
    "CONTENT_CAPTURE_MANAGER_SERVICE": "content_capture",
}


def winapk_service_name(cls, class_map, const_values):
    """Win.apk's Context.getSystemServiceName(Class), mimicked: the explicit map, else the simple
    name with "Manager" stripped and its first letter lowercased."""
    if cls in class_map:
        return const_values.get(class_map[cls], class_map[cls])
    n = cls.rsplit(".", 1)[-1]
    if n.endswith("Manager"):
        n = n[:-7]
    return n[:1].lower() + n[1:]


def is_service_constant(name):
    # The getSystemService names are the *_SERVICE constants (AUTOFILL_MANAGER_SERVICE,
    # TEXT_SERVICES_MANAGER_SERVICE included). Nothing else in Context ends that way.
    return name.endswith("_SERVICE")


# ---- Win.apk's sources: the case labels ------------------------------------------------------

def method_body(src, signature_regex):
    """The text of the first method whose header matches, braces balanced (strings ignored)."""
    m = re.search(signature_regex, src)
    if not m:
        return ""
    i = src.index("{", m.end() - 1)
    depth = 0
    j = i
    in_str = None
    while j < len(src):
        c = src[j]
        if in_str:
            if c == "\\":
                j += 1
            elif c == in_str:
                in_str = None
        elif c in "\"'":
            in_str = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
        j += 1
    return src[i:]


def strip_comments(src):
    """Java source without its comments, strings kept: one left-to-right scan, so a `/*` inside a
    line comment ("lib/* failed") or a `//` inside a string does not start a comment."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            i = src.find("\n", i)
            i = n if i < 0 else i
        elif src.startswith("/*", i):
            i = src.find("*/", i + 2)
            i = n if i < 0 else i + 2
            out.append(" ")
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def context_cases(repo):
    path = os.path.join(repo, "runtime/java/android/content/Context.java")
    with open(path, encoding="utf-8") as f:
        src = strip_comments(f.read())  # first: an apostrophe in a comment would open a "string"
    body = method_body(src, r"public Object getSystemService\(String name\)\s*\{")
    # case FOO_SERVICE: ... result = <expr>; break;
    cases = {}
    for m in re.finditer(r"case\s+([A-Z0-9_]+)\s*:(.*?)(?=\bcase\s+[A-Z0-9_]+\s*:|\bdefault\s*:)", body, re.S):
        expr = re.findall(r"result\s*=\s*([^;]+);", m.group(2))
        cases[m.group(1)] = " ".join(e.strip() for e in expr if e.strip() != "null") or "?"
    names = method_body(src, r"public String getSystemServiceName\(Class<\?> serviceClass\)\s*\{")
    class_map = dict(re.findall(r'case\s+"([\w.$]+)"\s*:\s*return\s+([A-Z0-9_]+)\s*;', names))
    return cases, class_map


def viewhost_cases(repo):
    path = os.path.join(repo, "windows/java/aosp_views/com/winapk/aospviews/AospViewHost.java")
    with open(path, encoding="utf-8") as f:
        src = strip_comments(f.read())
    body = method_body(src, r"public Object systemService\(Context context, String name\)\s*\{")
    return set(re.findall(r"case\s+Context\.([A-Z0-9_]+)\s*:", body))


# ---- APKs: the dex string pools --------------------------------------------------------------

def _uleb128(data, off):
    result = shift = 0
    while True:
        b = data[off]
        off += 1
        result |= (b & 0x7F) << shift
        if b < 0x80:
            return result, off
        shift += 7


def dex_strings(dex):
    size, ids_off = struct.unpack_from("<II", dex, 0x38)
    out = set()
    for k in range(size):
        p = struct.unpack_from("<I", dex, ids_off + 4 * k)[0]
        _, p = _uleb128(dex, p)
        end = dex.index(b"\0", p)
        out.add(dex[p:end].decode("utf-8", "replace"))
    return out


def apk_strings(apk):
    strings = set()
    with zipfile.ZipFile(apk) as z:
        for n in z.namelist():
            if re.fullmatch(r"classes\d*\.dex", n):
                strings |= dex_strings(z.read(n))
    return strings


# ---- main ------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--sdk", action="append", required=True,
                    help="platform dir with android.jar and data/api-versions.xml (repeat)")
    ap.add_argument("--repo", default=os.path.abspath(os.path.join(os.path.dirname(__file__), "../../..")))
    ap.add_argument("--apks", help="directory of .apk files to scan for the names")
    ap.add_argument("--tsv", help="also write the table as tab-separated values here")
    args = ap.parse_args()

    names = {}    # constant -> value
    levels = {}   # constant -> level added
    for sdk in args.sdk:
        consts = context_string_constants(os.path.join(sdk, "android.jar"))
        lv = context_field_levels(os.path.join(sdk, "data", "api-versions.xml"))
        for c, v in consts.items():
            if is_service_constant(c):
                names.setdefault(c, v)
                if c in lv:
                    levels[c] = lv[c]

    ctx, class_map = context_cases(args.repo)
    vh = viewhost_cases(args.repo)

    # Per APK: the name string in the dex string pool, and the manager class's type descriptor.
    str_hits, cls_hits = {}, {}
    if args.apks:
        for f in sorted(os.listdir(args.apks)):
            if not f.endswith(".apk"):
                continue
            app = re.sub(r"_\d+\.apk$", "", f)
            s = apk_strings(os.path.join(args.apks, f))
            for c, v in names.items():
                if v in s:
                    str_hits.setdefault(c, []).append(app)
                cls = MANAGER_CLASS.get(c)
                if cls and "L" + cls.replace(".", "/") + ";" in s:
                    cls_hits.setdefault(c, []).append(app)
            for cls in CLASS_ONLY:
                if "L" + cls.replace(".", "/") + ";" in s:
                    cls_hits.setdefault(cls, []).append(app)

    rows = []
    for c in sorted(names, key=lambda k: (float(levels.get(k, "1")), k)):
        who = "viewhost" if c in vh else ("context" if c in ctx else "-")
        answer = "ViewHost" if c in vh else ctx.get(c, "null (default)")
        cls = MANAGER_CLASS.get(c, "?")
        typed = winapk_service_name(cls, class_map, names) if cls != "?" else "?"
        typed_ok = "ok" if typed == names[c] else "MISS(%s)" % typed
        rows.append((c, names[c], levels.get(c, "?"), who, answer, cls, typed_ok,
                     ",".join(cls_hits.get(c, [])), ",".join(str_hits.get(c, []))))

    print("%-32s %-24s %-5s %-9s %-14s %s" % ("constant", "name", "since", "answers", "typed", "apps (class|string)"))
    for r in rows:
        print("%-32s %-24s %-5s %-9s %-14s %s | %s" % (r[0], r[1], r[2], r[3], r[6], r[7], r[8]))
    if args.tsv:
        with open(args.tsv, "w", encoding="utf-8") as f:
            f.write("constant\tname\tsince\tanswers\texpr\tclass\ttyped\tapps_class\tapps_string\n")
            for r in rows:
                f.write("\t".join(r) + "\n")

    missing = [r for r in rows if r[3] == "-"]
    print("\n%d names, %d answered (ViewHost %d, Context %d), %d answered by nobody (null):"
          % (len(rows), len(rows) - len(missing), sum(r[3] == "viewhost" for r in rows),
             sum(r[3] == "context" for r in rows), len(missing)))
    for r in missing:
        print("  %-32s %-24s since %-4s class: %s | string: %s" % (r[0], r[1], r[2], r[7] or "-", r[8] or "-"))
    typed_miss = [r for r in rows if r[3] != "-" and r[6] != "ok"]
    if typed_miss:
        print("\nAnswered by name but not by getSystemService(Class):")
        for r in typed_miss:
            print("  %-32s %s -> %s" % (r[0], r[5], r[6]))
    print("\nClass-only managers (no public name constant):")
    for cls, name in CLASS_ONLY.items():
        value_of = dict(names, **HIDDEN_NAMES)
        who = ("viewhost" if any(value_of.get(k) == name for k in vh)
               else "context" if any(value_of.get(k) == name for k in ctx) else "-")
        typed = winapk_service_name(cls, class_map, names)
        print("  %-48s %-16s typed->%-16s answers %-8s apps: %s" % (cls, name, typed, who, ",".join(cls_hits.get(cls, [])) or "-"))
    stray = sorted(set(ctx) - set(names))
    if stray:
        print("\nContext cases that are not public SDK names: " + ", ".join(stray))


if __name__ == "__main__":
    sys.exit(main())
