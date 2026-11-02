// android.bluetooth.le.ScanRecord
//
// The advertisement payload of a ScanResult, parsed from its bytes (the
// advertising data followed by the scan response, in wire form: length, AD
// type, data) as AOSP's parseFromBytes does: the flags, the service UUIDs
// (16-, 32- and 128-bit, complete or incomplete lists), the solicitation
// UUIDs, the local name (complete or shortened), the TX power level, the
// service data by UUID and the manufacturer data by company id. A record
// that cannot be parsed keeps its bytes and nothing else, as on a device.
// Mirrors frameworks/base/core/java/android/bluetooth/le/ScanRecord.java.
package android.bluetooth.le;

import android.os.ParcelUuid;
import android.util.SparseArray;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.UUID;

public final class ScanRecord {

    private static final int DATA_TYPE_FLAGS = 0x01;
    private static final int DATA_TYPE_SERVICE_UUIDS_16_BIT_PARTIAL = 0x02;
    private static final int DATA_TYPE_SERVICE_UUIDS_16_BIT_COMPLETE = 0x03;
    private static final int DATA_TYPE_SERVICE_UUIDS_32_BIT_PARTIAL = 0x04;
    private static final int DATA_TYPE_SERVICE_UUIDS_32_BIT_COMPLETE = 0x05;
    private static final int DATA_TYPE_SERVICE_UUIDS_128_BIT_PARTIAL = 0x06;
    private static final int DATA_TYPE_SERVICE_UUIDS_128_BIT_COMPLETE = 0x07;
    private static final int DATA_TYPE_LOCAL_NAME_SHORT = 0x08;
    private static final int DATA_TYPE_LOCAL_NAME_COMPLETE = 0x09;
    private static final int DATA_TYPE_TX_POWER_LEVEL = 0x0A;
    private static final int DATA_TYPE_SERVICE_SOLICITATION_UUIDS_16_BIT = 0x14;
    private static final int DATA_TYPE_SERVICE_SOLICITATION_UUIDS_128_BIT = 0x15;
    private static final int DATA_TYPE_SERVICE_DATA_16_BIT = 0x16;
    private static final int DATA_TYPE_SERVICE_SOLICITATION_UUIDS_32_BIT = 0x1F;
    private static final int DATA_TYPE_SERVICE_DATA_32_BIT = 0x20;
    private static final int DATA_TYPE_SERVICE_DATA_128_BIT = 0x21;
    private static final int DATA_TYPE_MANUFACTURER_SPECIFIC_DATA = 0xFF;

    private static final int UUID_BYTES_16_BIT = 2;
    private static final int UUID_BYTES_32_BIT = 4;
    private static final int UUID_BYTES_128_BIT = 16;
    private static final long BASE_UUID_MSB = 0x0000000000001000L;
    private static final long BASE_UUID_LSB = 0x800000805F9B34FBL;

    private final int mAdvertiseFlags;
    private final List<ParcelUuid> mServiceUuids;
    private final List<ParcelUuid> mServiceSolicitationUuids;
    private final SparseArray<byte[]> mManufacturerSpecificData;
    private final Map<ParcelUuid, byte[]> mServiceData;
    private final int mTxPowerLevel;
    private final String mDeviceName;
    private final byte[] mBytes;

    ScanRecord() {
        this(null, null, null, null, -1, Integer.MIN_VALUE, null, null);
    }

    private ScanRecord(List<ParcelUuid> serviceUuids, List<ParcelUuid> serviceSolicitationUuids,
            SparseArray<byte[]> manufacturerData, Map<ParcelUuid, byte[]> serviceData, int advertiseFlags,
            int txPowerLevel, String localName, byte[] bytes) {
        mServiceSolicitationUuids = serviceSolicitationUuids;
        mServiceUuids = serviceUuids;
        mManufacturerSpecificData = manufacturerData != null ? manufacturerData : new SparseArray<byte[]>();
        mServiceData = serviceData;
        mDeviceName = localName;
        mAdvertiseFlags = advertiseFlags;
        mTxPowerLevel = txPowerLevel;
        mBytes = bytes;
    }

    /** AOSP: -1 when the record carries no flags. */
    public int getAdvertiseFlags() { return mAdvertiseFlags; }

    /** AOSP: null when the record lists no service UUIDs. */
    public List<ParcelUuid> getServiceUuids() { return mServiceUuids; }

    /** AOSP: the service-solicitation UUIDs advertised in the record. */
    public List<ParcelUuid> getServiceSolicitationUuids() { return mServiceSolicitationUuids; }

    /** AOSP: every manufacturer-specific block in the record, keyed by
     *  company id; empty rather than null, so a caller iterating it does not
     *  NPE. */
    public SparseArray<byte[]> getManufacturerSpecificData() { return mManufacturerSpecificData; }

    public byte[] getManufacturerSpecificData(int manufacturerId) {
        return mManufacturerSpecificData.get(manufacturerId);
    }

    public Map<ParcelUuid, byte[]> getServiceData() { return mServiceData; }

    public byte[] getServiceData(ParcelUuid serviceDataUuid) {
        if (serviceDataUuid == null || mServiceData == null) return null;
        return mServiceData.get(serviceDataUuid);
    }

    public int getTxPowerLevel() { return mTxPowerLevel; }
    public String getDeviceName() { return mDeviceName; }
    public byte[] getBytes() { return mBytes; }

    /** AOSP's parser (hidden there, public static). Null for null bytes; a
     *  record with the bytes alone when they cannot be parsed. */
    public static ScanRecord parseFromBytes(byte[] scanRecord) {
        if (scanRecord == null) return null;
        int currentPos = 0;
        int advertiseFlag = -1;
        List<ParcelUuid> serviceUuids = new ArrayList<>();
        List<ParcelUuid> serviceSolicitationUuids = new ArrayList<>();
        String localName = null;
        int txPowerLevel = Integer.MIN_VALUE;
        SparseArray<byte[]> manufacturerData = new SparseArray<>();
        Map<ParcelUuid, byte[]> serviceData = new android.util.ArrayMap<>();
        try {
            while (currentPos < scanRecord.length) {
                int length = scanRecord[currentPos++] & 0xFF;
                if (length == 0) break;
                int dataLength = length - 1;
                int fieldType = scanRecord[currentPos++] & 0xFF;
                switch (fieldType) {
                case DATA_TYPE_FLAGS:
                    advertiseFlag = scanRecord[currentPos] & 0xFF;
                    break;
                case DATA_TYPE_SERVICE_UUIDS_16_BIT_PARTIAL:
                case DATA_TYPE_SERVICE_UUIDS_16_BIT_COMPLETE:
                    parseServiceUuid(scanRecord, currentPos, dataLength, UUID_BYTES_16_BIT, serviceUuids);
                    break;
                case DATA_TYPE_SERVICE_UUIDS_32_BIT_PARTIAL:
                case DATA_TYPE_SERVICE_UUIDS_32_BIT_COMPLETE:
                    parseServiceUuid(scanRecord, currentPos, dataLength, UUID_BYTES_32_BIT, serviceUuids);
                    break;
                case DATA_TYPE_SERVICE_UUIDS_128_BIT_PARTIAL:
                case DATA_TYPE_SERVICE_UUIDS_128_BIT_COMPLETE:
                    parseServiceUuid(scanRecord, currentPos, dataLength, UUID_BYTES_128_BIT, serviceUuids);
                    break;
                case DATA_TYPE_SERVICE_SOLICITATION_UUIDS_16_BIT:
                    parseServiceUuid(scanRecord, currentPos, dataLength, UUID_BYTES_16_BIT, serviceSolicitationUuids);
                    break;
                case DATA_TYPE_SERVICE_SOLICITATION_UUIDS_32_BIT:
                    parseServiceUuid(scanRecord, currentPos, dataLength, UUID_BYTES_32_BIT, serviceSolicitationUuids);
                    break;
                case DATA_TYPE_SERVICE_SOLICITATION_UUIDS_128_BIT:
                    parseServiceUuid(scanRecord, currentPos, dataLength, UUID_BYTES_128_BIT, serviceSolicitationUuids);
                    break;
                case DATA_TYPE_LOCAL_NAME_SHORT:
                case DATA_TYPE_LOCAL_NAME_COMPLETE:
                    localName = new String(extractBytes(scanRecord, currentPos, dataLength),
                            java.nio.charset.StandardCharsets.UTF_8);
                    break;
                case DATA_TYPE_TX_POWER_LEVEL:
                    txPowerLevel = scanRecord[currentPos];
                    break;
                case DATA_TYPE_SERVICE_DATA_16_BIT:
                case DATA_TYPE_SERVICE_DATA_32_BIT:
                case DATA_TYPE_SERVICE_DATA_128_BIT: {
                    int serviceUuidLength = UUID_BYTES_16_BIT;
                    if (fieldType == DATA_TYPE_SERVICE_DATA_32_BIT) serviceUuidLength = UUID_BYTES_32_BIT;
                    else if (fieldType == DATA_TYPE_SERVICE_DATA_128_BIT) serviceUuidLength = UUID_BYTES_128_BIT;
                    byte[] serviceDataUuidBytes = extractBytes(scanRecord, currentPos, serviceUuidLength);
                    ParcelUuid serviceDataUuid = parseUuidFrom(serviceDataUuidBytes);
                    int serviceDataArrayLength = dataLength - serviceUuidLength;
                    byte[] serviceDataArray = extractBytes(scanRecord, currentPos + serviceUuidLength,
                            serviceDataArrayLength);
                    serviceData.put(serviceDataUuid, serviceDataArray);
                    break;
                }
                case DATA_TYPE_MANUFACTURER_SPECIFIC_DATA: {
                    // The company id is little-endian, the data follows it.
                    int manufacturerId = ((scanRecord[currentPos + 1] & 0xFF) << 8) + (scanRecord[currentPos] & 0xFF);
                    byte[] manufacturerDataBytes = extractBytes(scanRecord, currentPos + 2, dataLength - 2);
                    manufacturerData.put(manufacturerId, manufacturerDataBytes);
                    break;
                }
                default:
                    break;
                }
                currentPos += dataLength;
            }
            if (serviceUuids.isEmpty()) serviceUuids = null;
            if (serviceSolicitationUuids.isEmpty()) serviceSolicitationUuids = null;
            return new ScanRecord(serviceUuids, serviceSolicitationUuids, manufacturerData, serviceData,
                    advertiseFlag, txPowerLevel, localName, scanRecord);
        } catch (Exception e) {
            // Malformed: the bytes are kept and nothing else, as AOSP does.
            return new ScanRecord(null, null, null, null, -1, Integer.MIN_VALUE, null, scanRecord);
        }
    }

    private static int parseServiceUuid(byte[] scanRecord, int currentPos, int dataLength, int uuidLength,
            List<ParcelUuid> serviceUuids) {
        while (dataLength > 0) {
            byte[] uuidBytes = extractBytes(scanRecord, currentPos, uuidLength);
            serviceUuids.add(parseUuidFrom(uuidBytes));
            dataLength -= uuidLength;
            currentPos += uuidLength;
        }
        return currentPos;
    }

    private static byte[] extractBytes(byte[] scanRecord, int start, int length) {
        byte[] bytes = new byte[length];
        System.arraycopy(scanRecord, start, bytes, 0, length);
        return bytes;
    }

    /** BluetoothUuid.parseUuidFrom: a little-endian 16-, 32- or 128-bit
     *  UUID, the short ones in the Bluetooth base UUID. */
    static ParcelUuid parseUuidFrom(byte[] uuidBytes) {
        if (uuidBytes == null) throw new IllegalArgumentException("uuidBytes cannot be null");
        int length = uuidBytes.length;
        if (length != UUID_BYTES_16_BIT && length != UUID_BYTES_32_BIT && length != UUID_BYTES_128_BIT) {
            throw new IllegalArgumentException("uuidBytes length invalid - " + length);
        }
        if (length == UUID_BYTES_128_BIT) {
            long msb = 0, lsb = 0;
            for (int i = 0; i < 8; i++) {
                lsb |= (long) (uuidBytes[i] & 0xFF) << (8 * i);
                msb |= (long) (uuidBytes[8 + i] & 0xFF) << (8 * i);
            }
            return new ParcelUuid(new UUID(msb, lsb));
        }
        long shortUuid;
        if (length == UUID_BYTES_16_BIT) {
            shortUuid = uuidBytes[0] & 0xFF;
            shortUuid += (uuidBytes[1] & 0xFF) << 8;
        } else {
            shortUuid = uuidBytes[0] & 0xFF;
            shortUuid += (uuidBytes[1] & 0xFF) << 8;
            shortUuid += (uuidBytes[2] & 0xFF) << 16;
            shortUuid += (long) (uuidBytes[3] & 0xFF) << 24;
        }
        long msb = BASE_UUID_MSB + (shortUuid << 32);
        return new ParcelUuid(new UUID(msb, BASE_UUID_LSB));
    }

    @Override public String toString() {
        return "ScanRecord [mAdvertiseFlags=" + mAdvertiseFlags + ", mServiceUuids=" + mServiceUuids
                + ", mServiceSolicitationUuids=" + mServiceSolicitationUuids
                + ", mManufacturerSpecificData=" + mManufacturerSpecificData
                + ", mServiceData=" + (mServiceData == null ? "null" : mServiceData.keySet())
                + ", mTxPowerLevel=" + mTxPowerLevel + ", mDeviceName=" + mDeviceName + "]";
    }
}
