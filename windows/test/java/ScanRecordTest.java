// ScanRecordTest -- checks android.bluetooth.le.ScanRecord's parser, which
// turns the advertising bytes the host's LE scan delivers into what apps read
// (flags, service UUIDs, the name, TX power, service and manufacturer data),
// against records as phones and beacons send them, and ScanFilter.matches
// over the result.
//
//   javac -cp "<runtime>/lib/android-framework.jar" -d out windows/test/java/ScanRecordTest.java
//   java  -cp "<runtime>/lib/android-framework.jar;out" ScanRecordTest
//
// Exits 0 when every check passes. Pure parsing: no radio, store or native
// runtime is needed.

import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.os.ParcelUuid;

import java.util.Arrays;
import java.util.UUID;

public final class ScanRecordTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "[PASS] " : "[FAIL] ") + what);
        if (!ok) failures++;
    }

    private static byte[] bytes(int... v) {
        byte[] b = new byte[v.length];
        for (int i = 0; i < v.length; i++) b[i] = (byte) v[i];
        return b;
    }

    public static void main(String[] args) {
        // A heart-rate monitor: flags, a complete 16-bit service list, the
        // complete name, TX power, then (scan response) service data.
        byte[] hrm = bytes(
                0x02, 0x01, 0x06,
                0x05, 0x03, 0x0D, 0x18, 0x0F, 0x18,
                0x04, 0x09, 'H', 'R', 'M',
                0x02, 0x0A, 0xF6,
                0x05, 0x16, 0x0F, 0x18, 0x64, 0x00);
        ScanRecord r = ScanRecord.parseFromBytes(hrm);
        check(r != null, "parses");
        check(r.getAdvertiseFlags() == 0x06, "flags");
        check(r.getServiceUuids() != null && r.getServiceUuids().size() == 2
                && r.getServiceUuids().get(0).equals(ParcelUuid.fromString("0000180D-0000-1000-8000-00805F9B34FB"))
                && r.getServiceUuids().get(1).equals(ParcelUuid.fromString("0000180F-0000-1000-8000-00805F9B34FB")),
                "16-bit service UUIDs in the base UUID");
        check("HRM".equals(r.getDeviceName()), "complete name");
        check(r.getTxPowerLevel() == -10, "tx power is signed");
        check(Arrays.equals(r.getServiceData(ParcelUuid.fromString("0000180F-0000-1000-8000-00805F9B34FB")),
                bytes(0x64, 0x00)), "service data by UUID");
        check(r.getManufacturerSpecificData() != null && r.getManufacturerSpecificData().size() == 0,
                "no manufacturer data: empty, not null");
        check(r.getBytes() == hrm, "bytes kept");

        // An iBeacon: flags and Apple's manufacturer block.
        byte[] beacon = bytes(
                0x02, 0x01, 0x1A,
                0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15,
                0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2, 0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0,
                0x00, 0x01, 0x00, 0x02, 0xC5);
        r = ScanRecord.parseFromBytes(beacon);
        byte[] apple = r.getManufacturerSpecificData(0x004C);
        check(apple != null && apple.length == 23 && apple[0] == 0x02 && apple[1] == 0x15 && (apple[22] & 0xFF) == 0xC5,
                "manufacturer data by little-endian company id");
        check(r.getServiceUuids() == null, "no service UUIDs: null, as AOSP");
        check(r.getDeviceName() == null, "no name");
        check(r.getTxPowerLevel() == Integer.MIN_VALUE, "no tx power");

        // A 128-bit UUID (Nordic UART), little-endian on the air, a shortened
        // name, and zero padding after the data, as a phone's 62-byte record.
        byte[] uart = new byte[62];
        byte[] uartData = bytes(
                0x11, 0x07, 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00,
                0x40, 0x6E,
                0x03, 0x08, 'N', 'U');
        System.arraycopy(uartData, 0, uart, 0, uartData.length);
        r = ScanRecord.parseFromBytes(uart);
        check(r.getServiceUuids() != null && r.getServiceUuids().size() == 1
                && r.getServiceUuids().get(0).getUuid().equals(UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")),
                "128-bit service UUID, little-endian");
        check("NU".equals(r.getDeviceName()), "shortened name");

        // 32-bit UUIDs and solicitation lists.
        r = ScanRecord.parseFromBytes(bytes(0x05, 0x05, 0x78, 0x56, 0x34, 0x12, 0x03, 0x14, 0x0F, 0x18));
        check(r.getServiceUuids() != null
                && r.getServiceUuids().get(0).equals(ParcelUuid.fromString("12345678-0000-1000-8000-00805F9B34FB")),
                "32-bit service UUID");
        check(r.getServiceSolicitationUuids() != null
                && r.getServiceSolicitationUuids().get(0)
                        .equals(ParcelUuid.fromString("0000180F-0000-1000-8000-00805F9B34FB")),
                "16-bit solicitation UUID");

        // Malformed: a length past the end. The bytes are kept, nothing else.
        r = ScanRecord.parseFromBytes(bytes(0x02, 0x01, 0x06, 0x10, 0x09, 'x'));
        check(r != null && r.getBytes() != null && r.getAdvertiseFlags() == -1 && r.getDeviceName() == null,
                "malformed record keeps only its bytes");
        check(ScanRecord.parseFromBytes(null) == null, "null bytes: null");
        check(ScanRecord.parseFromBytes(new byte[0]) != null, "empty record parses");

        // ScanFilter over a parsed result, as BluetoothLeScanner applies it.
        ScanRecord parsed = ScanRecord.parseFromBytes(hrm);
        ScanResult result = new ScanResult(new android.bluetooth.BluetoothDevice("A4:C1:38:9E:00:7F"), parsed, -60, 0L);
        check(new ScanFilter.Builder().setServiceUuid(ParcelUuid.fromString("0000180D-0000-1000-8000-00805F9B34FB"))
                .build().matches(result), "filter by service UUID matches");
        check(!new ScanFilter.Builder().setServiceUuid(ParcelUuid.fromString("00001800-0000-1000-8000-00805F9B34FB"))
                .build().matches(result), "filter by another UUID does not");
        check(new ScanFilter.Builder().setDeviceName("HRM").build().matches(result), "filter by name");
        check(new ScanFilter.Builder().setDeviceAddress("A4:C1:38:9E:00:7F").build().matches(result),
                "filter by address");
        check(new ScanFilter.Builder().setServiceData(ParcelUuid.fromString("0000180F-0000-1000-8000-00805F9B34FB"),
                bytes(0x64)).build().matches(result), "filter by service data prefix");
        ScanResult beaconResult = new ScanResult(new android.bluetooth.BluetoothDevice("A4:C1:38:9E:00:80"),
                ScanRecord.parseFromBytes(beacon), -70, 0L);
        check(new ScanFilter.Builder().setManufacturerData(0x004C, bytes(0x02, 0x15)).build().matches(beaconResult),
                "filter by manufacturer data");

        System.out.println(failures == 0 ? "OK" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
