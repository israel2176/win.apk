package android.os.flagging;
/** Why reading the aconfig flag storage failed (API 36). Mac.apk ships no
 *  aconfig storage and AconfigPackage answers every flag with the caller's
 *  default, so nothing here throws one; apps name it in catch clauses and read
 *  its error code.
 *  Source: frameworks/base/core/java/android/os/flagging/AconfigStorageReadException.java */
public class AconfigStorageReadException extends RuntimeException {
    /** Generic errors. */
    public static final int ERROR_GENERIC = 0;
    /** The storage system is not found. */
    public static final int ERROR_STORAGE_SYSTEM_NOT_FOUND = 1;
    /** The package is not found in the storage. */
    public static final int ERROR_PACKAGE_NOT_FOUND = 2;
    /** The container is not found in the storage. */
    public static final int ERROR_CONTAINER_NOT_FOUND = 3;
    /** A storage file could not be read. */
    public static final int ERROR_CANNOT_READ_STORAGE_FILE = 4;
    /** The package's fingerprint does not match the one in the storage file. */
    public static final int ERROR_FILE_FINGERPRINT_MISMATCH = 5;

    private final int mErrorCode;

    public AconfigStorageReadException(int errorCode, String msg) {
        super(msg);
        mErrorCode = errorCode;
    }

    public AconfigStorageReadException(int errorCode, String msg, Throwable cause) {
        super(msg, cause);
        mErrorCode = errorCode;
    }

    public AconfigStorageReadException(int errorCode, Throwable cause) {
        super(cause);
        mErrorCode = errorCode;
    }

    public int getErrorCode() { return mErrorCode; }

    /** AOSP prefixes the message with the error's name. */
    @Override public String getMessage() {
        return errorName(mErrorCode) + ": " + super.getMessage();
    }

    private static String errorName(int code) {
        switch (code) {
            case ERROR_GENERIC: return "ERROR_GENERIC";
            case ERROR_STORAGE_SYSTEM_NOT_FOUND: return "ERROR_STORAGE_SYSTEM_NOT_FOUND";
            case ERROR_PACKAGE_NOT_FOUND: return "ERROR_PACKAGE_NOT_FOUND";
            case ERROR_CONTAINER_NOT_FOUND: return "ERROR_CONTAINER_NOT_FOUND";
            case ERROR_CANNOT_READ_STORAGE_FILE: return "ERROR_CANNOT_READ_STORAGE_FILE";
            case ERROR_FILE_FINGERPRINT_MISMATCH: return "ERROR_FILE_FINGERPRINT_MISMATCH";
            default: return "<Unknown error code " + code + ">";
        }
    }
}
