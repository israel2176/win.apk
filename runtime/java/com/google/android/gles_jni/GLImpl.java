// com.google.android.gles_jni.GLImpl -- the GL10/GL11 object EGLContext.getGL() hands out, which
// GLSurfaceView passes to its renderer's onSurfaceCreated, onSurfaceChanged and onDrawFrame.
//
// AOSP's GLImpl (frameworks/base opengl/java/com/google/android/gles_jni/GLImpl.java) is JNI over
// the GLES 1 driver for the context current on the calling thread. This one forwards each method to
// the static binding of the same name and parameters (android.opengl.GLES11, GLES10, GLES20), which
// call ANGLE for the calling thread's context the same way. In an ES 2 context ANGLE answers the
// fixed-function calls with GL_INVALID_OPERATION, as a device's driver does.
//
// What has no binding here (the OES framebuffer, matrix palette and draw-texture extensions,
// glGetPointerv, the texture-coordinate generation queries) does nothing and answers 0: AOSP throws
// UnsupportedOperationException only for a driver that lacks the extension, which a phone's does
// not, so a renderer that would work on one is not killed here. Generated from the interfaces;
// the bodies are one line each.
package com.google.android.gles_jni;

import android.opengl.GLES10;
import android.opengl.GLES11;
import android.opengl.GLES20;

import javax.microedition.khronos.opengles.GL10;
import javax.microedition.khronos.opengles.GL10Ext;
import javax.microedition.khronos.opengles.GL11;
import javax.microedition.khronos.opengles.GL11Ext;
import javax.microedition.khronos.opengles.GL11ExtensionPack;

public final class GLImpl implements GL10, GL10Ext, GL11, GL11Ext, GL11ExtensionPack {
    /** One per process: it keeps no state, and every call goes to the calling thread's context. */
    public static final GLImpl INSTANCE = new GLImpl();

    private GLImpl() {}

    /** A method with no binding here (see the header). */
    private static void unsupported() {}

    @Override public void glActiveTexture(int a0) { GLES10.glActiveTexture(a0); }
    @Override public void glAlphaFunc(int a0, float a1) { GLES10.glAlphaFunc(a0, a1); }
    @Override public void glAlphaFuncx(int a0, int a1) { GLES10.glAlphaFuncx(a0, a1); }
    @Override public void glBindBuffer(int a0, int a1) { GLES11.glBindBuffer(a0, a1); }
    @Override public void glBindFramebufferOES(int a0, int a1) { unsupported(); }
    @Override public void glBindRenderbufferOES(int a0, int a1) { unsupported(); }
    @Override public void glBindTexture(int a0, int a1) { GLES10.glBindTexture(a0, a1); }
    @Override public void glBlendEquationSeparate(int a0, int a1) { GLES20.glBlendEquationSeparate(a0, a1); }
    @Override public void glBlendEquation(int a0) { GLES20.glBlendEquation(a0); }
    @Override public void glBlendFuncSeparate(int a0, int a1, int a2, int a3) {
        GLES20.glBlendFuncSeparate(a0, a1, a2, a3);
    }
    @Override public void glBlendFunc(int a0, int a1) { GLES10.glBlendFunc(a0, a1); }
    @Override public void glBufferData(int a0, int a1, java.nio.Buffer a2, int a3) {
        GLES11.glBufferData(a0, a1, a2, a3);
    }
    @Override public void glBufferSubData(int a0, int a1, int a2, java.nio.Buffer a3) {
        GLES11.glBufferSubData(a0, a1, a2, a3);
    }
    @Override public int glCheckFramebufferStatusOES(int a0) { unsupported(); return 0; }
    @Override public void glClearColor(float a0, float a1, float a2, float a3) { GLES10.glClearColor(a0, a1, a2, a3); }
    @Override public void glClearColorx(int a0, int a1, int a2, int a3) { GLES10.glClearColorx(a0, a1, a2, a3); }
    @Override public void glClearDepthf(float a0) { GLES10.glClearDepthf(a0); }
    @Override public void glClearDepthx(int a0) { GLES10.glClearDepthx(a0); }
    @Override public void glClearStencil(int a0) { GLES10.glClearStencil(a0); }
    @Override public void glClear(int a0) { GLES10.glClear(a0); }
    @Override public void glClientActiveTexture(int a0) { GLES10.glClientActiveTexture(a0); }
    @Override public void glClipPlanef(int a0, float[] a1, int a2) { GLES11.glClipPlanef(a0, a1, a2); }
    @Override public void glClipPlanef(int a0, java.nio.FloatBuffer a1) { GLES11.glClipPlanef(a0, a1); }
    @Override public void glClipPlanex(int a0, int[] a1, int a2) { GLES11.glClipPlanex(a0, a1, a2); }
    @Override public void glClipPlanex(int a0, java.nio.IntBuffer a1) { GLES11.glClipPlanex(a0, a1); }
    @Override public void glColor4f(float a0, float a1, float a2, float a3) { GLES10.glColor4f(a0, a1, a2, a3); }
    @Override public void glColor4ub(byte a0, byte a1, byte a2, byte a3) { GLES11.glColor4ub(a0, a1, a2, a3); }
    @Override public void glColor4x(int a0, int a1, int a2, int a3) { GLES10.glColor4x(a0, a1, a2, a3); }
    @Override public void glColorMask(boolean a0, boolean a1, boolean a2, boolean a3) {
        GLES10.glColorMask(a0, a1, a2, a3);
    }
    @Override public void glColorPointer(int a0, int a1, int a2, java.nio.Buffer a3) {
        GLES10.glColorPointer(a0, a1, a2, a3);
    }
    @Override public void glColorPointer(int a0, int a1, int a2, int a3) { GLES11.glColorPointer(a0, a1, a2, a3); }
    @Override public void glCompressedTexImage2D(int a0, int a1, int a2, int a3, int a4, int a5, int a6,
            java.nio.Buffer a7) {
        GLES10.glCompressedTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7);
    }
    @Override public void glCompressedTexSubImage2D(int a0, int a1, int a2, int a3, int a4, int a5, int a6,
            int a7, java.nio.Buffer a8) {
        GLES10.glCompressedTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8);
    }
    @Override public void glCopyTexImage2D(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7) {
        GLES10.glCopyTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7);
    }
    @Override public void glCopyTexSubImage2D(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7) {
        GLES10.glCopyTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7);
    }
    @Override public void glCullFace(int a0) { GLES10.glCullFace(a0); }
    @Override public void glCurrentPaletteMatrixOES(int a0) { unsupported(); }
    @Override public void glDeleteBuffers(int a0, int[] a1, int a2) { GLES11.glDeleteBuffers(a0, a1, a2); }
    @Override public void glDeleteBuffers(int a0, java.nio.IntBuffer a1) { GLES11.glDeleteBuffers(a0, a1); }
    @Override public void glDeleteFramebuffersOES(int a0, int[] a1, int a2) { unsupported(); }
    @Override public void glDeleteFramebuffersOES(int a0, java.nio.IntBuffer a1) { unsupported(); }
    @Override public void glDeleteRenderbuffersOES(int a0, int[] a1, int a2) { unsupported(); }
    @Override public void glDeleteRenderbuffersOES(int a0, java.nio.IntBuffer a1) { unsupported(); }
    @Override public void glDeleteTextures(int a0, int[] a1, int a2) { GLES10.glDeleteTextures(a0, a1, a2); }
    @Override public void glDeleteTextures(int a0, java.nio.IntBuffer a1) { GLES10.glDeleteTextures(a0, a1); }
    @Override public void glDepthFunc(int a0) { GLES10.glDepthFunc(a0); }
    @Override public void glDepthMask(boolean a0) { GLES10.glDepthMask(a0); }
    @Override public void glDepthRangef(float a0, float a1) { GLES10.glDepthRangef(a0, a1); }
    @Override public void glDepthRangex(int a0, int a1) { GLES10.glDepthRangex(a0, a1); }
    @Override public void glDisableClientState(int a0) { GLES10.glDisableClientState(a0); }
    @Override public void glDisable(int a0) { GLES10.glDisable(a0); }
    @Override public void glDrawArrays(int a0, int a1, int a2) { GLES10.glDrawArrays(a0, a1, a2); }
    @Override public void glDrawElements(int a0, int a1, int a2, java.nio.Buffer a3) {
        GLES10.glDrawElements(a0, a1, a2, a3);
    }
    @Override public void glDrawElements(int a0, int a1, int a2, int a3) { GLES11.glDrawElements(a0, a1, a2, a3); }
    @Override public void glDrawTexfOES(float a0, float a1, float a2, float a3, float a4) { unsupported(); }
    @Override public void glDrawTexfvOES(float[] a0, int a1) { unsupported(); }
    @Override public void glDrawTexfvOES(java.nio.FloatBuffer a0) { unsupported(); }
    @Override public void glDrawTexiOES(int a0, int a1, int a2, int a3, int a4) { unsupported(); }
    @Override public void glDrawTexivOES(int[] a0, int a1) { unsupported(); }
    @Override public void glDrawTexivOES(java.nio.IntBuffer a0) { unsupported(); }
    @Override public void glDrawTexsOES(short a0, short a1, short a2, short a3, short a4) { unsupported(); }
    @Override public void glDrawTexsvOES(short[] a0, int a1) { unsupported(); }
    @Override public void glDrawTexsvOES(java.nio.ShortBuffer a0) { unsupported(); }
    @Override public void glDrawTexxOES(int a0, int a1, int a2, int a3, int a4) { unsupported(); }
    @Override public void glDrawTexxvOES(int[] a0, int a1) { unsupported(); }
    @Override public void glDrawTexxvOES(java.nio.IntBuffer a0) { unsupported(); }
    @Override public void glEnableClientState(int a0) { GLES10.glEnableClientState(a0); }
    @Override public void glEnable(int a0) { GLES10.glEnable(a0); }
    @Override public void glFinish() { GLES10.glFinish(); }
    @Override public void glFlush() { GLES10.glFlush(); }
    @Override public void glFogf(int a0, float a1) { GLES10.glFogf(a0, a1); }
    @Override public void glFogfv(int a0, float[] a1, int a2) { GLES10.glFogfv(a0, a1, a2); }
    @Override public void glFogfv(int a0, java.nio.FloatBuffer a1) { GLES10.glFogfv(a0, a1); }
    @Override public void glFogx(int a0, int a1) { GLES10.glFogx(a0, a1); }
    @Override public void glFogxv(int a0, int[] a1, int a2) { GLES10.glFogxv(a0, a1, a2); }
    @Override public void glFogxv(int a0, java.nio.IntBuffer a1) { GLES10.glFogxv(a0, a1); }
    @Override public void glFramebufferRenderbufferOES(int a0, int a1, int a2, int a3) { unsupported(); }
    @Override public void glFramebufferTexture2DOES(int a0, int a1, int a2, int a3, int a4) { unsupported(); }
    @Override public void glFrontFace(int a0) { GLES10.glFrontFace(a0); }
    @Override public void glFrustumf(float a0, float a1, float a2, float a3, float a4, float a5) {
        GLES10.glFrustumf(a0, a1, a2, a3, a4, a5);
    }
    @Override public void glFrustumx(int a0, int a1, int a2, int a3, int a4, int a5) {
        GLES10.glFrustumx(a0, a1, a2, a3, a4, a5);
    }
    @Override public void glGenBuffers(int a0, int[] a1, int a2) { GLES11.glGenBuffers(a0, a1, a2); }
    @Override public void glGenBuffers(int a0, java.nio.IntBuffer a1) { GLES11.glGenBuffers(a0, a1); }
    @Override public void glGenFramebuffersOES(int a0, int[] a1, int a2) { unsupported(); }
    @Override public void glGenFramebuffersOES(int a0, java.nio.IntBuffer a1) { unsupported(); }
    @Override public void glGenRenderbuffersOES(int a0, int[] a1, int a2) { unsupported(); }
    @Override public void glGenRenderbuffersOES(int a0, java.nio.IntBuffer a1) { unsupported(); }
    @Override public void glGenTextures(int a0, int[] a1, int a2) { GLES10.glGenTextures(a0, a1, a2); }
    @Override public void glGenTextures(int a0, java.nio.IntBuffer a1) { GLES10.glGenTextures(a0, a1); }
    @Override public void glGenerateMipmapOES(int a0) { unsupported(); }
    @Override public void glGetBooleanv(int a0, boolean[] a1, int a2) { GLES11.glGetBooleanv(a0, a1, a2); }
    @Override public void glGetBooleanv(int a0, java.nio.IntBuffer a1) { GLES11.glGetBooleanv(a0, a1); }
    @Override public void glGetBufferParameteriv(int a0, int a1, int[] a2, int a3) {
        GLES11.glGetBufferParameteriv(a0, a1, a2, a3);
    }
    @Override public void glGetBufferParameteriv(int a0, int a1, java.nio.IntBuffer a2) {
        GLES11.glGetBufferParameteriv(a0, a1, a2);
    }
    @Override public void glGetClipPlanef(int a0, float[] a1, int a2) { GLES11.glGetClipPlanef(a0, a1, a2); }
    @Override public void glGetClipPlanef(int a0, java.nio.FloatBuffer a1) { GLES11.glGetClipPlanef(a0, a1); }
    @Override public void glGetClipPlanex(int a0, int[] a1, int a2) { GLES11.glGetClipPlanex(a0, a1, a2); }
    @Override public void glGetClipPlanex(int a0, java.nio.IntBuffer a1) { GLES11.glGetClipPlanex(a0, a1); }
    @Override public int glGetError() { return GLES10.glGetError(); }
    @Override public void glGetFixedv(int a0, int[] a1, int a2) { GLES11.glGetFixedv(a0, a1, a2); }
    @Override public void glGetFixedv(int a0, java.nio.IntBuffer a1) { GLES11.glGetFixedv(a0, a1); }
    @Override public void glGetFloatv(int a0, float[] a1, int a2) { GLES11.glGetFloatv(a0, a1, a2); }
    @Override public void glGetFloatv(int a0, java.nio.FloatBuffer a1) { GLES11.glGetFloatv(a0, a1); }
    @Override public void glGetFramebufferAttachmentParameterivOES(int a0, int a1, int a2, int[] a3, int a4) {
        unsupported();
    }
    @Override public void glGetFramebufferAttachmentParameterivOES(int a0, int a1, int a2, java.nio.IntBuffer a3) {
        unsupported();
    }
    @Override public void glGetIntegerv(int a0, int[] a1, int a2) { GLES10.glGetIntegerv(a0, a1, a2); }
    @Override public void glGetIntegerv(int a0, java.nio.IntBuffer a1) { GLES10.glGetIntegerv(a0, a1); }
    @Override public void glGetLightfv(int a0, int a1, float[] a2, int a3) { GLES11.glGetLightfv(a0, a1, a2, a3); }
    @Override public void glGetLightfv(int a0, int a1, java.nio.FloatBuffer a2) { GLES11.glGetLightfv(a0, a1, a2); }
    @Override public void glGetLightxv(int a0, int a1, int[] a2, int a3) { GLES11.glGetLightxv(a0, a1, a2, a3); }
    @Override public void glGetLightxv(int a0, int a1, java.nio.IntBuffer a2) { GLES11.glGetLightxv(a0, a1, a2); }
    @Override public void glGetMaterialfv(int a0, int a1, float[] a2, int a3) {
        GLES11.glGetMaterialfv(a0, a1, a2, a3);
    }
    @Override public void glGetMaterialfv(int a0, int a1, java.nio.FloatBuffer a2) {
        GLES11.glGetMaterialfv(a0, a1, a2);
    }
    @Override public void glGetMaterialxv(int a0, int a1, int[] a2, int a3) { GLES11.glGetMaterialxv(a0, a1, a2, a3); }
    @Override public void glGetMaterialxv(int a0, int a1, java.nio.IntBuffer a2) { GLES11.glGetMaterialxv(a0, a1, a2); }
    @Override public void glGetPointerv(int a0, java.nio.Buffer[] a1) { unsupported(); }
    @Override public void glGetRenderbufferParameterivOES(int a0, int a1, int[] a2, int a3) { unsupported(); }
    @Override public void glGetRenderbufferParameterivOES(int a0, int a1, java.nio.IntBuffer a2) { unsupported(); }
    @Override public java.lang.String glGetString(int a0) { return GLES10.glGetString(a0); }
    @Override public void glGetTexEnviv(int a0, int a1, int[] a2, int a3) { GLES11.glGetTexEnviv(a0, a1, a2, a3); }
    @Override public void glGetTexEnviv(int a0, int a1, java.nio.IntBuffer a2) { GLES11.glGetTexEnviv(a0, a1, a2); }
    @Override public void glGetTexEnvxv(int a0, int a1, int[] a2, int a3) { GLES11.glGetTexEnvxv(a0, a1, a2, a3); }
    @Override public void glGetTexEnvxv(int a0, int a1, java.nio.IntBuffer a2) { GLES11.glGetTexEnvxv(a0, a1, a2); }
    @Override public void glGetTexGenfv(int a0, int a1, float[] a2, int a3) { unsupported(); }
    @Override public void glGetTexGenfv(int a0, int a1, java.nio.FloatBuffer a2) { unsupported(); }
    @Override public void glGetTexGeniv(int a0, int a1, int[] a2, int a3) { unsupported(); }
    @Override public void glGetTexGeniv(int a0, int a1, java.nio.IntBuffer a2) { unsupported(); }
    @Override public void glGetTexGenxv(int a0, int a1, int[] a2, int a3) { unsupported(); }
    @Override public void glGetTexGenxv(int a0, int a1, java.nio.IntBuffer a2) { unsupported(); }
    @Override public void glGetTexParameterfv(int a0, int a1, float[] a2, int a3) {
        GLES11.glGetTexParameterfv(a0, a1, a2, a3);
    }
    @Override public void glGetTexParameterfv(int a0, int a1, java.nio.FloatBuffer a2) {
        GLES11.glGetTexParameterfv(a0, a1, a2);
    }
    @Override public void glGetTexParameteriv(int a0, int a1, int[] a2, int a3) {
        GLES11.glGetTexParameteriv(a0, a1, a2, a3);
    }
    @Override public void glGetTexParameteriv(int a0, int a1, java.nio.IntBuffer a2) {
        GLES11.glGetTexParameteriv(a0, a1, a2);
    }
    @Override public void glGetTexParameterxv(int a0, int a1, int[] a2, int a3) {
        GLES11.glGetTexParameterxv(a0, a1, a2, a3);
    }
    @Override public void glGetTexParameterxv(int a0, int a1, java.nio.IntBuffer a2) {
        GLES11.glGetTexParameterxv(a0, a1, a2);
    }
    @Override public void glHint(int a0, int a1) { GLES10.glHint(a0, a1); }
    @Override public boolean glIsBuffer(int a0) { return GLES11.glIsBuffer(a0); }
    @Override public boolean glIsEnabled(int a0) { return GLES11.glIsEnabled(a0); }
    @Override public boolean glIsFramebufferOES(int a0) { unsupported(); return false; }
    @Override public boolean glIsRenderbufferOES(int a0) { unsupported(); return false; }
    @Override public boolean glIsTexture(int a0) { return GLES11.glIsTexture(a0); }
    @Override public void glLightModelf(int a0, float a1) { GLES10.glLightModelf(a0, a1); }
    @Override public void glLightModelfv(int a0, float[] a1, int a2) { GLES10.glLightModelfv(a0, a1, a2); }
    @Override public void glLightModelfv(int a0, java.nio.FloatBuffer a1) { GLES10.glLightModelfv(a0, a1); }
    @Override public void glLightModelx(int a0, int a1) { GLES10.glLightModelx(a0, a1); }
    @Override public void glLightModelxv(int a0, int[] a1, int a2) { GLES10.glLightModelxv(a0, a1, a2); }
    @Override public void glLightModelxv(int a0, java.nio.IntBuffer a1) { GLES10.glLightModelxv(a0, a1); }
    @Override public void glLightf(int a0, int a1, float a2) { GLES10.glLightf(a0, a1, a2); }
    @Override public void glLightfv(int a0, int a1, float[] a2, int a3) { GLES10.glLightfv(a0, a1, a2, a3); }
    @Override public void glLightfv(int a0, int a1, java.nio.FloatBuffer a2) { GLES10.glLightfv(a0, a1, a2); }
    @Override public void glLightx(int a0, int a1, int a2) { GLES10.glLightx(a0, a1, a2); }
    @Override public void glLightxv(int a0, int a1, int[] a2, int a3) { GLES10.glLightxv(a0, a1, a2, a3); }
    @Override public void glLightxv(int a0, int a1, java.nio.IntBuffer a2) { GLES10.glLightxv(a0, a1, a2); }
    @Override public void glLineWidth(float a0) { GLES10.glLineWidth(a0); }
    @Override public void glLineWidthx(int a0) { GLES10.glLineWidthx(a0); }
    @Override public void glLoadIdentity() { GLES10.glLoadIdentity(); }
    @Override public void glLoadMatrixf(float[] a0, int a1) { GLES10.glLoadMatrixf(a0, a1); }
    @Override public void glLoadMatrixf(java.nio.FloatBuffer a0) { GLES10.glLoadMatrixf(a0); }
    @Override public void glLoadMatrixx(int[] a0, int a1) { GLES10.glLoadMatrixx(a0, a1); }
    @Override public void glLoadMatrixx(java.nio.IntBuffer a0) { GLES10.glLoadMatrixx(a0); }
    @Override public void glLoadPaletteFromModelViewMatrixOES() { unsupported(); }
    @Override public void glLogicOp(int a0) { GLES10.glLogicOp(a0); }
    @Override public void glMaterialf(int a0, int a1, float a2) { GLES10.glMaterialf(a0, a1, a2); }
    @Override public void glMaterialfv(int a0, int a1, float[] a2, int a3) { GLES10.glMaterialfv(a0, a1, a2, a3); }
    @Override public void glMaterialfv(int a0, int a1, java.nio.FloatBuffer a2) { GLES10.glMaterialfv(a0, a1, a2); }
    @Override public void glMaterialx(int a0, int a1, int a2) { GLES10.glMaterialx(a0, a1, a2); }
    @Override public void glMaterialxv(int a0, int a1, int[] a2, int a3) { GLES10.glMaterialxv(a0, a1, a2, a3); }
    @Override public void glMaterialxv(int a0, int a1, java.nio.IntBuffer a2) { GLES10.glMaterialxv(a0, a1, a2); }
    @Override public void glMatrixIndexPointerOES(int a0, int a1, int a2, java.nio.Buffer a3) { unsupported(); }
    @Override public void glMatrixIndexPointerOES(int a0, int a1, int a2, int a3) { unsupported(); }
    @Override public void glMatrixMode(int a0) { GLES10.glMatrixMode(a0); }
    @Override public void glMultMatrixf(float[] a0, int a1) { GLES10.glMultMatrixf(a0, a1); }
    @Override public void glMultMatrixf(java.nio.FloatBuffer a0) { GLES10.glMultMatrixf(a0); }
    @Override public void glMultMatrixx(int[] a0, int a1) { GLES10.glMultMatrixx(a0, a1); }
    @Override public void glMultMatrixx(java.nio.IntBuffer a0) { GLES10.glMultMatrixx(a0); }
    @Override public void glMultiTexCoord4f(int a0, float a1, float a2, float a3, float a4) {
        GLES10.glMultiTexCoord4f(a0, a1, a2, a3, a4);
    }
    @Override public void glMultiTexCoord4x(int a0, int a1, int a2, int a3, int a4) {
        GLES10.glMultiTexCoord4x(a0, a1, a2, a3, a4);
    }
    @Override public void glNormal3f(float a0, float a1, float a2) { GLES10.glNormal3f(a0, a1, a2); }
    @Override public void glNormal3x(int a0, int a1, int a2) { GLES10.glNormal3x(a0, a1, a2); }
    @Override public void glNormalPointer(int a0, int a1, java.nio.Buffer a2) { GLES10.glNormalPointer(a0, a1, a2); }
    @Override public void glNormalPointer(int a0, int a1, int a2) { GLES11.glNormalPointer(a0, a1, a2); }
    @Override public void glOrthof(float a0, float a1, float a2, float a3, float a4, float a5) {
        GLES10.glOrthof(a0, a1, a2, a3, a4, a5);
    }
    @Override public void glOrthox(int a0, int a1, int a2, int a3, int a4, int a5) {
        GLES10.glOrthox(a0, a1, a2, a3, a4, a5);
    }
    @Override public void glPixelStorei(int a0, int a1) { GLES10.glPixelStorei(a0, a1); }
    @Override public void glPointParameterf(int a0, float a1) { GLES11.glPointParameterf(a0, a1); }
    @Override public void glPointParameterfv(int a0, float[] a1, int a2) { GLES11.glPointParameterfv(a0, a1, a2); }
    @Override public void glPointParameterfv(int a0, java.nio.FloatBuffer a1) { GLES11.glPointParameterfv(a0, a1); }
    @Override public void glPointParameterx(int a0, int a1) { GLES11.glPointParameterx(a0, a1); }
    @Override public void glPointParameterxv(int a0, int[] a1, int a2) { GLES11.glPointParameterxv(a0, a1, a2); }
    @Override public void glPointParameterxv(int a0, java.nio.IntBuffer a1) { GLES11.glPointParameterxv(a0, a1); }
    @Override public void glPointSizePointerOES(int a0, int a1, java.nio.Buffer a2) {
        GLES11.glPointSizePointerOES(a0, a1, a2);
    }
    @Override public void glPointSize(float a0) { GLES10.glPointSize(a0); }
    @Override public void glPointSizex(int a0) { GLES10.glPointSizex(a0); }
    @Override public void glPolygonOffset(float a0, float a1) { GLES10.glPolygonOffset(a0, a1); }
    @Override public void glPolygonOffsetx(int a0, int a1) { GLES10.glPolygonOffsetx(a0, a1); }
    @Override public void glPopMatrix() { GLES10.glPopMatrix(); }
    @Override public void glPushMatrix() { GLES10.glPushMatrix(); }
    @Override public int glQueryMatrixxOES(int[] a0, int a1, int[] a2, int a3) { unsupported(); return 0; }
    @Override public int glQueryMatrixxOES(java.nio.IntBuffer a0, java.nio.IntBuffer a1) { unsupported(); return 0; }
    @Override public void glReadPixels(int a0, int a1, int a2, int a3, int a4, int a5, java.nio.Buffer a6) {
        GLES10.glReadPixels(a0, a1, a2, a3, a4, a5, a6);
    }
    @Override public void glRenderbufferStorageOES(int a0, int a1, int a2, int a3) { unsupported(); }
    @Override public void glRotatef(float a0, float a1, float a2, float a3) { GLES10.glRotatef(a0, a1, a2, a3); }
    @Override public void glRotatex(int a0, int a1, int a2, int a3) { GLES10.glRotatex(a0, a1, a2, a3); }
    @Override public void glSampleCoverage(float a0, boolean a1) { GLES10.glSampleCoverage(a0, a1); }
    @Override public void glSampleCoveragex(int a0, boolean a1) { GLES10.glSampleCoveragex(a0, a1); }
    @Override public void glScalef(float a0, float a1, float a2) { GLES10.glScalef(a0, a1, a2); }
    @Override public void glScalex(int a0, int a1, int a2) { GLES10.glScalex(a0, a1, a2); }
    @Override public void glScissor(int a0, int a1, int a2, int a3) { GLES10.glScissor(a0, a1, a2, a3); }
    @Override public void glShadeModel(int a0) { GLES10.glShadeModel(a0); }
    @Override public void glStencilFunc(int a0, int a1, int a2) { GLES10.glStencilFunc(a0, a1, a2); }
    @Override public void glStencilMask(int a0) { GLES10.glStencilMask(a0); }
    @Override public void glStencilOp(int a0, int a1, int a2) { GLES10.glStencilOp(a0, a1, a2); }
    @Override public void glTexCoordPointer(int a0, int a1, int a2, java.nio.Buffer a3) {
        GLES10.glTexCoordPointer(a0, a1, a2, a3);
    }
    @Override public void glTexCoordPointer(int a0, int a1, int a2, int a3) {
        GLES11.glTexCoordPointer(a0, a1, a2, a3);
    }
    @Override public void glTexEnvf(int a0, int a1, float a2) { GLES10.glTexEnvf(a0, a1, a2); }
    @Override public void glTexEnvfv(int a0, int a1, float[] a2, int a3) { GLES10.glTexEnvfv(a0, a1, a2, a3); }
    @Override public void glTexEnvfv(int a0, int a1, java.nio.FloatBuffer a2) { GLES10.glTexEnvfv(a0, a1, a2); }
    @Override public void glTexEnvi(int a0, int a1, int a2) { GLES11.glTexEnvi(a0, a1, a2); }
    @Override public void glTexEnviv(int a0, int a1, int[] a2, int a3) { GLES11.glTexEnviv(a0, a1, a2, a3); }
    @Override public void glTexEnviv(int a0, int a1, java.nio.IntBuffer a2) { GLES11.glTexEnviv(a0, a1, a2); }
    @Override public void glTexEnvx(int a0, int a1, int a2) { GLES10.glTexEnvx(a0, a1, a2); }
    @Override public void glTexEnvxv(int a0, int a1, int[] a2, int a3) { GLES10.glTexEnvxv(a0, a1, a2, a3); }
    @Override public void glTexEnvxv(int a0, int a1, java.nio.IntBuffer a2) { GLES10.glTexEnvxv(a0, a1, a2); }
    @Override public void glTexGenf(int a0, int a1, float a2) { unsupported(); }
    @Override public void glTexGenfv(int a0, int a1, float[] a2, int a3) { unsupported(); }
    @Override public void glTexGenfv(int a0, int a1, java.nio.FloatBuffer a2) { unsupported(); }
    @Override public void glTexGeni(int a0, int a1, int a2) { unsupported(); }
    @Override public void glTexGeniv(int a0, int a1, int[] a2, int a3) { unsupported(); }
    @Override public void glTexGeniv(int a0, int a1, java.nio.IntBuffer a2) { unsupported(); }
    @Override public void glTexGenx(int a0, int a1, int a2) { unsupported(); }
    @Override public void glTexGenxv(int a0, int a1, int[] a2, int a3) { unsupported(); }
    @Override public void glTexGenxv(int a0, int a1, java.nio.IntBuffer a2) { unsupported(); }
    @Override public void glTexImage2D(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7,
            java.nio.Buffer a8) {
        GLES10.glTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8);
    }
    @Override public void glTexParameterf(int a0, int a1, float a2) { GLES10.glTexParameterf(a0, a1, a2); }
    @Override public void glTexParameterfv(int a0, int a1, float[] a2, int a3) {
        GLES11.glTexParameterfv(a0, a1, a2, a3);
    }
    @Override public void glTexParameterfv(int a0, int a1, java.nio.FloatBuffer a2) {
        GLES11.glTexParameterfv(a0, a1, a2);
    }
    @Override public void glTexParameteri(int a0, int a1, int a2) { GLES11.glTexParameteri(a0, a1, a2); }
    @Override public void glTexParameteriv(int a0, int a1, int[] a2, int a3) {
        GLES11.glTexParameteriv(a0, a1, a2, a3);
    }
    @Override public void glTexParameteriv(int a0, int a1, java.nio.IntBuffer a2) {
        GLES11.glTexParameteriv(a0, a1, a2);
    }
    @Override public void glTexParameterx(int a0, int a1, int a2) { GLES10.glTexParameterx(a0, a1, a2); }
    @Override public void glTexParameterxv(int a0, int a1, int[] a2, int a3) {
        GLES11.glTexParameterxv(a0, a1, a2, a3);
    }
    @Override public void glTexParameterxv(int a0, int a1, java.nio.IntBuffer a2) {
        GLES11.glTexParameterxv(a0, a1, a2);
    }
    @Override public void glTexSubImage2D(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7,
            java.nio.Buffer a8) {
        GLES10.glTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8);
    }
    @Override public void glTranslatef(float a0, float a1, float a2) { GLES10.glTranslatef(a0, a1, a2); }
    @Override public void glTranslatex(int a0, int a1, int a2) { GLES10.glTranslatex(a0, a1, a2); }
    @Override public void glVertexPointer(int a0, int a1, int a2, java.nio.Buffer a3) {
        GLES10.glVertexPointer(a0, a1, a2, a3);
    }
    @Override public void glVertexPointer(int a0, int a1, int a2, int a3) { GLES11.glVertexPointer(a0, a1, a2, a3); }
    @Override public void glViewport(int a0, int a1, int a2, int a3) { GLES10.glViewport(a0, a1, a2, a3); }
    @Override public void glWeightPointerOES(int a0, int a1, int a2, java.nio.Buffer a3) { unsupported(); }
    @Override public void glWeightPointerOES(int a0, int a1, int a2, int a3) { unsupported(); }
}
