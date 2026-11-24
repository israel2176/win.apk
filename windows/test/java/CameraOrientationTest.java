// CameraOrientationTest -- the camera's orientation as apps compute with it,
// and what a camera without focus or torch control reports. Runs on a plain
// JVM: the camera is CameraBridge's virtual one (a back camera with neither),
// and there is no window, so the display rotation is ROTATION_0.
//
//   javac -cp "<runtime>/lib/aosp-graphics.jar;<runtime>/lib/android-framework.jar;<runtime>/lib/macapk-loader.jar" -d out \
//         windows/test/java/CameraOrientationTest.java
//   java  -cp "<runtime>/lib/android-framework.jar;<runtime>/lib/macapk-loader.jar;out" CameraOrientationTest
//
// Checks:
//  * Scanovate's cheque scanner (Hapoalim, Leumi, Mercantile) turns the
//    preview by pictureAngle = (CameraInfo.orientation - rotation * 90) mod 360:
//    0 for an upright webcam in a landscape (ROTATION_90) and a portrait
//    window, for the back camera it opens. So do camera1's documented
//    setDisplayOrientation formula (back and front) and CameraX's
//    getRelativeImageRotation (its rotationDegrees).
//  * AOSP's buffer transforms: degToTransform, getRotationTransform and
//    GLConsumer's matrix.
//  * The virtual camera: CameraInfo.orientation and SENSOR_ORIENTATION 0 at
//    ROTATION_0; focus "fixed" only, no flash modes; setParameters refuses
//    "auto" focus and "torch" as AOSP's camera service does; autoFocus answers
//    true at once; camera2 lists AF OFF only, a minimum focus distance of 0,
//    no flash unit, and setTorchMode throws.
//  * No made-up sensors: getDefaultSensor is null for the accelerometer, the
//    gyroscope and every other type, the list is empty, and no
//    android.hardware.sensor.* feature is derived from the catalog.
// Exits 0 when every check passes.

import android.hardware.Camera;
import android.hardware.Sensor;
import android.hardware.SensorManager;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
import com.macapk.loader.CameraBridge;
import java.util.Arrays;
import java.util.List;

public final class CameraOrientationTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "[PASS] " : "[FAIL] ") + what);
        if (!ok) failures++;
    }

    /** Scanovate's pictureAngle. */
    static int scanovate(int orientation, int rotation) {
        return ((orientation - rotation * 90) % 360 + 360) % 360;
    }

    /** android.hardware.Camera.setDisplayOrientation's documented example. */
    static int camera1Display(int orientation, int rotation, boolean front) {
        int degrees = rotation * 90;
        if (front) {
            int r = (orientation + degrees) % 360;
            return (360 - r) % 360;
        }
        return (orientation - degrees + 360) % 360;
    }

    /** CameraX CameraOrientationUtil.getRelativeImageRotation. */
    static int cameraX(int sensorOrientation, int rotation, boolean back) {
        int dest = rotation * 90;
        return back ? (sensorOrientation - dest + 360) % 360 : (sensorOrientation + dest) % 360;
    }

    public static void main(String[] args) throws Exception {
        int back = CameraBridge.FACING_BACK, front = CameraBridge.FACING_FRONT;

        // The two cases a cheque scanner meets: its landscape-fixed scan
        // activity (ROTATION_90) and a portrait one.
        int o1 = CameraBridge.sensorOrientationFor(back, 1);
        check(o1 == 90, "back camera, ROTATION_90: orientation 90 (got " + o1 + ")");
        check(scanovate(90, 1) == 0, "Scanovate: rotation 1 with orientation 90 gives 0");
        check(scanovate(o1, 1) == 0, "Scanovate's angle on our landscape orientation is 0");
        int o0 = CameraBridge.sensorOrientationFor(back, 0);
        check(o0 == 0, "back camera, ROTATION_0: orientation 0 (got " + o0 + ")");
        check(scanovate(0, 0) == 0, "Scanovate: rotation 0 with orientation 0 gives 0");
        check(scanovate(o0, 0) == 0, "Scanovate's angle on our portrait orientation is 0");

        for (int rotation = 0; rotation < 4; rotation++) {
            int ob = CameraBridge.sensorOrientationFor(back, rotation);
            int of = CameraBridge.sensorOrientationFor(front, rotation);
            check(camera1Display(ob, rotation, false) == 0 && camera1Display(of, rotation, true) == 0,
                    "camera1 setDisplayOrientation formula gives 0 at rotation " + rotation
                            + " (back " + ob + ", front " + of + ")");
            check(cameraX(ob, rotation, true) == 0 && cameraX(of, rotation, false) == 0,
                    "CameraX rotationDegrees is 0 at rotation " + rotation);
        }
        check(CameraBridge.sensorOrientationFor(front, 1) == 270, "front camera, ROTATION_90: orientation 270");

        // AOSP's transforms.
        check(CameraBridge.previewTransform(0, false) == 0
                        && CameraBridge.previewTransform(90, false) == CameraBridge.TRANSFORM_ROT_90
                        && CameraBridge.previewTransform(0, true) == CameraBridge.TRANSFORM_FLIP_H
                        && CameraBridge.previewTransform(180, true) == CameraBridge.TRANSFORM_FLIP_V,
                "degToTransform");
        check(CameraBridge.outputTransform(0, false) == 0
                        && CameraBridge.outputTransform(90, false) == CameraBridge.TRANSFORM_ROT_90
                        && CameraBridge.outputTransform(90, true)
                                == (CameraBridge.TRANSFORM_FLIP_H ^ CameraBridge.TRANSFORM_ROT_270)
                        && CameraBridge.outputTransform(0, true) == CameraBridge.TRANSFORM_FLIP_H,
                "getRotationTransform");
        float[] flipV = { 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1 };
        check(Arrays.equals(CameraBridge.consumerMatrix(0), flipV), "GLConsumer matrix of no transform is flip-V");
        // ROT_90 maps texture (s, t) = (0, 0) -- the buffer's bottom-left in GL
        // terms -- through rot90 then flip-V: GLConsumer's mtxRot90 is
        // {0,1,0,0, -1,0,0,0, 0,0,1,0, 1,0,0,1}.
        float[] r90 = CameraBridge.consumerMatrix(CameraBridge.TRANSFORM_ROT_90);
        float[] want90 = { 0, -1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1 };
        check(Arrays.equals(r90, want90), "GLConsumer matrix of ROT_90 " + Arrays.toString(r90));

        // The virtual camera (no webcam natives on a plain JVM).
        check(Camera.getNumberOfCameras() == 1, "one (virtual) camera");
        Camera.CameraInfo info = new Camera.CameraInfo();
        Camera.getCameraInfo(0, info);
        check(info.facing == Camera.CameraInfo.CAMERA_FACING_BACK, "it is a back camera");
        check(info.orientation == 0, "CameraInfo.orientation 0 at ROTATION_0 (got " + info.orientation + ")");
        check(scanovate(info.orientation, 0) == 0, "Scanovate's angle on it is 0");

        Camera cam = Camera.open();
        check(cam != null, "Camera.open() opens the back camera");
        Camera.Parameters p = cam.getParameters();
        List<String> focus = p.getSupportedFocusModes();
        check(focus != null && focus.equals(Arrays.asList("fixed")), "focus modes [fixed]: " + focus);
        check(p.getSupportedFlashModes() == null, "no flash modes");
        boolean threw = false;
        try {
            Camera.Parameters q = cam.getParameters();
            q.setFocusMode(Camera.Parameters.FOCUS_MODE_AUTO);
            cam.setParameters(q);
        } catch (RuntimeException e) {
            threw = "setParameters failed".equals(e.getMessage());
        }
        check(threw, "setParameters refuses focus \"auto\" on a fixed-focus camera");
        check("fixed".equals(cam.getParameters().getFocusMode()), "and keeps \"fixed\"");
        threw = false;
        try {
            Camera.Parameters q = cam.getParameters();
            q.setFlashMode(Camera.Parameters.FLASH_MODE_TORCH);
            cam.setParameters(q);
        } catch (RuntimeException e) {
            threw = "setParameters failed".equals(e.getMessage());
        }
        check(threw, "setParameters refuses flash \"torch\" without a torch");
        Camera.Parameters off = cam.getParameters();
        off.setFlashMode(Camera.Parameters.FLASH_MODE_OFF);
        cam.setParameters(off);
        check(true, "setParameters accepts flash \"off\" without a flash, as AOSP does");
        cam.release();
        // Callbacks come on the Looper of the thread that opened the camera.
        final boolean[] focused = { false };
        Thread looper = new Thread(() -> {
            android.os.Looper.prepare();
            Camera c1 = Camera.open();
            c1.autoFocus((success, c) -> {
                focused[0] = success;
                c.release();
                android.os.Looper.myLooper().quit();
            });
            android.os.Looper.loop();
        });
        looper.setDaemon(true);
        looper.start();
        looper.join(3000);
        check(focused[0], "autoFocus on a fixed-focus camera calls back with true");

        CameraManager cm = new CameraManager();
        CameraCharacteristics ch = cm.getCameraCharacteristics("0");
        check(Integer.valueOf(0).equals(ch.get(CameraCharacteristics.SENSOR_ORIENTATION)),
                "SENSOR_ORIENTATION 0 at ROTATION_0");
        check(Integer.valueOf(CameraMetadata.LENS_FACING_BACK).equals(ch.get(CameraCharacteristics.LENS_FACING)),
                "LENS_FACING_BACK");
        int[] af = ch.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES);
        check(af != null && af.length == 1 && af[0] == CameraMetadata.CONTROL_AF_MODE_OFF, "AF modes [OFF]");
        check(Float.valueOf(0f).equals(ch.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE)),
                "LENS_INFO_MINIMUM_FOCUS_DISTANCE 0 (fixed focus)");
        check(Boolean.FALSE.equals(ch.get(CameraCharacteristics.FLASH_INFO_AVAILABLE)), "no flash unit");
        threw = false;
        try {
            cm.setTorchMode("0", true);
        } catch (IllegalArgumentException e) {
            threw = true;
        }
        check(threw, "setTorchMode throws IllegalArgumentException without a flash unit");
        check(!CameraBridge.anyCameraHasAutofocus() && !CameraBridge.anyCameraHasTorch(),
                "no autofocus or flash feature for the virtual camera");

        SensorManager sm = SensorManager.macapkNew();
        check(sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) == null, "no accelerometer");
        check(sm.getSensorList(Sensor.TYPE_ACCELEROMETER).isEmpty(), "none in the list");
        check(sm.getDefaultSensor(Sensor.TYPE_GRAVITY) == null
                        && sm.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR) == null
                        && sm.getDefaultSensor(Sensor.TYPE_ORIENTATION) == null,
                "nothing fused from one (gravity, rotation vector, orientation)");
        check(!SensorManager.macapkSensorFeatures().contains("android.hardware.sensor.accelerometer"),
                "no android.hardware.sensor.accelerometer feature");
        check(sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE) == null
                        && sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE_UNCALIBRATED) == null,
                "no gyroscope");
        check(sm.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD) == null
                        && sm.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR) == null
                        && sm.getDefaultSensor(Sensor.TYPE_LIGHT) == null
                        && sm.getDefaultSensor(Sensor.TYPE_PROXIMITY) == null
                        && sm.getDefaultSensor(Sensor.TYPE_STEP_COUNTER) == null,
                "no magnetometer, game rotation vector, light, proximity or step counter");
        check(sm.getSensorList(Sensor.TYPE_ALL).isEmpty(), "the sensor list is empty");
        check(SensorManager.macapkSensorFeatures().isEmpty(), "no android.hardware.sensor.* feature");
        check(!sm.registerListener(new android.hardware.SensorEventListener() {
                    @Override public void onSensorChanged(android.hardware.SensorEvent e) {}
                    @Override public void onAccuracyChanged(Sensor s, int a) {}
                }, sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER), SensorManager.SENSOR_DELAY_GAME),
                "registering for the missing accelerometer returns false, no crash");

        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
