/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2014 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

// Modified by Lasse Oorni and Yao Wei Tjong for Urho3D

#include "../../SDL_internal.h"
#include "SDL_stdinc.h"
#include "SDL_assert.h"
#include "SDL_log.h"

#ifdef __ANDROID__

#include "SDL_system.h"
#include "SDL_android.h"

#include <EGL/egl.h>

#include "../../events/SDL_events_c.h"
#include "../../video/android/SDL_androidkeyboard.h"
#include "../../video/android/SDL_androidtouch.h"
#include "../../video/android/SDL_androidvideo.h"
#include "../../video/android/SDL_androidwindow.h"
#include "../../joystick/android/SDL_sysjoystick_c.h"

#include <android/log.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>
#define LOG_TAG "SDL_android"
/* #define LOGI(...)  __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__) */
/* #define LOGE(...)  __android_log_print(ANDROID_LOG_ERROR,LOG_TAG,__VA_ARGS__) */
#define LOGI(...) do {} while (false)
#define LOGE(...) do {} while (false)

/* Uncomment this to log messages entering and exiting methods in this file */
/* #define DEBUG_JNI */

static void Android_JNI_ThreadDestroyed(void*);

/*******************************************************************************
 This file links the Java side of Android with libsdl
*******************************************************************************/
#include <jni.h>
#include <android/log.h>
#include <stdbool.h>


/*******************************************************************************
                               Globals
*******************************************************************************/
static pthread_key_t mThreadKey;
static JavaVM* mJavaVM;

/* Main activity */
static jclass mActivityClass;

/* method signatures */
static jmethodID midGetNativeSurface;
static jmethodID midFlipBuffers;
static jmethodID midAudioInit;
static jmethodID midAudioWriteShortBuffer;
static jmethodID midAudioWriteByteBuffer;
static jmethodID midAudioQuit;
static jmethodID midPollInputDevices;
static jmethodID midHandleSDLEvent;
static jmethodID midCreateLuaRuntime;

/* Accelerometer data storage */
static float fLastAccelerometer[3];
static bool bHasNewData;

// Urho3D: application files dir
static char* mFilesDir = 0;

/*******************************************************************************
                 Functions called by JNI
*******************************************************************************/

/* Library init */
jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    JNIEnv *env;
    mJavaVM = vm;
    LOGI("JNI_OnLoad called");
    if ((*mJavaVM)->GetEnv(mJavaVM, (void**) &env, JNI_VERSION_1_4) != JNI_OK) {
        LOGE("Failed to get the environment using GetEnv()");
        return -1;
    }
    /*
     * Create mThreadKey so we can keep track of the JNIEnv assigned to each thread
     * Refer to http://developer.android.com/guide/practices/design/jni.html for the rationale behind this
     */
    if (pthread_key_create(&mThreadKey, Android_JNI_ThreadDestroyed) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "SDL", "Error initializing pthread key");
    }
    Android_JNI_SetupThread();

    return JNI_VERSION_1_4;
}

// Urho3D: added function
const char* SDL_Android_GetFilesDir()
{
    return mFilesDir;
}

/* Called before SDL_main() to initialize JNI bindings */
// Urho3D: added passing user files directory from SDLActivity on startup
void SDL_Android_Init(JNIEnv* mEnv, jclass cls, jstring filesDir)
{
    __android_log_print(ANDROID_LOG_INFO, "SDL", "SDL_Android_Init()");

    Android_JNI_SetupThread();

    // Copy the files dir
    const char *str;
    str = (*mEnv)->GetStringUTFChars(mEnv, filesDir, 0);
    if (str)
    {
        if (mFilesDir)
            free(mFilesDir);

        size_t length = strlen(str) + 1;
        mFilesDir = (char*)malloc(length);
        memcpy(mFilesDir, str, length);
        (*mEnv)->ReleaseStringUTFChars(mEnv, filesDir, str);
    }

    mActivityClass = (jclass)((*mEnv)->NewGlobalRef(mEnv, cls));

    midGetNativeSurface = (*mEnv)->GetStaticMethodID(mEnv, mActivityClass,
                                "getNativeSurface","()Landroid/view/Surface;");
    midFlipBuffers = (*mEnv)->GetStaticMethodID(mEnv, mActivityClass,
                                "flipBuffers","()V");
    midAudioInit = (*mEnv)->GetStaticMethodID(mEnv, mActivityClass,
                                "audioInit", "(IZZI)I");
    midAudioWriteShortBuffer = (*mEnv)->GetStaticMethodID(mEnv, mActivityClass,
                                "audioWriteShortBuffer", "([S)V");
    midAudioWriteByteBuffer = (*mEnv)->GetStaticMethodID(mEnv, mActivityClass,
                                "audioWriteByteBuffer", "([B)V");
    midAudioQuit = (*mEnv)->GetStaticMethodID(mEnv, mActivityClass,
                                "audioQuit", "()V");
    midPollInputDevices = (*mEnv)->GetStaticMethodID(mEnv, mActivityClass,
                                "pollInputDevices", "()V");

    midHandleSDLEvent = (*mEnv)->GetStaticMethodID(mEnv, mActivityClass,
                                "handlSDLEvent", "()V");

    midCreateLuaRuntime = (*mEnv)->GetStaticMethodID(mEnv, mActivityClass,
                                "createLuaRuntime", "()V");

    bHasNewData = false;

    if(!midGetNativeSurface || !midFlipBuffers || !midAudioInit ||
       !midAudioWriteShortBuffer || !midAudioWriteByteBuffer || !midAudioQuit || !midPollInputDevices) {
        __android_log_print(ANDROID_LOG_WARN, "SDL", "SDL: Couldn't locate Java callbacks, check that they're named and typed correctly");
    }
    __android_log_print(ANDROID_LOG_INFO, "SDL", "SDL_Android_Init() finished!");
}

// Handle event on the SDL thread
void SDL_AndroidHandleEvent() {
    JNIEnv *env = Android_JNI_GetEnv();
    (*env)->CallStaticVoidMethod(env, mActivityClass, midHandleSDLEvent);
}

void Android_CreateLuaRuntime() {
    JNIEnv *env = Android_JNI_GetEnv();
    (*env)->CallStaticVoidMethod(env, mActivityClass, midCreateLuaRuntime);   
}

/* Resize */
void Java_org_libsdl_app_SDLActivity_onNativeResize(
                                    JNIEnv* env, jclass jcls,
                                    jint width, jint height, jint format)
{
    Android_SetScreenResolution(width, height, format);
}

// Paddown
int Java_org_libsdl_app_SDLActivity_onNativePadDown(
                                    JNIEnv* env, jclass jcls,
                                    jint device_id, jint keycode)
{
    return Android_OnPadDown(device_id, keycode);
}

// Padup
int Java_org_libsdl_app_SDLActivity_onNativePadUp(
                                   JNIEnv* env, jclass jcls,
                                   jint device_id, jint keycode)
{
    return Android_OnPadUp(device_id, keycode);
}

/* Joy */
void Java_org_libsdl_app_SDLActivity_onNativeJoy(
                                    JNIEnv* env, jclass jcls,
                                    jint device_id, jint axis, jfloat value)
{
    Android_OnJoy(device_id, axis, value);
}

/* POV Hat */
void Java_org_libsdl_app_SDLActivity_onNativeHat(
                                    JNIEnv* env, jclass jcls,
                                    jint device_id, jint hat_id, jint x, jint y)
{
    Android_OnHat(device_id, hat_id, x, y);
}


int Java_org_libsdl_app_SDLActivity_nativeAddJoystick(
    JNIEnv* env, jclass jcls,
    jint device_id, jstring device_name, jint is_accelerometer, 
    jint nbuttons, jint naxes, jint nhats, jint nballs)
{
    int retval;
    const char *name = (*env)->GetStringUTFChars(env, device_name, NULL);

    retval = Android_AddJoystick(device_id, name, (SDL_bool) is_accelerometer, nbuttons, naxes, nhats, nballs);

    (*env)->ReleaseStringUTFChars(env, device_name, name);
    
    return retval;
}

int Java_org_libsdl_app_SDLActivity_nativeRemoveJoystick(
    JNIEnv* env, jclass jcls, jint device_id)
{
    return Android_RemoveJoystick(device_id);
}


/* Surface Created */
void Java_org_libsdl_app_SDLActivity_onNativeSurfaceChanged(JNIEnv* env, jclass jcls)
{
    SDL_WindowData *data;
    SDL_VideoDevice *_this;

    if (Android_Window == NULL || Android_Window->driverdata == NULL ) {
        return;
    }
    
    _this =  SDL_GetVideoDevice();
    data =  (SDL_WindowData *) Android_Window->driverdata;
    
    /* If the surface has been previously destroyed by onNativeSurfaceDestroyed, recreate it here */
    if (data->egl_surface == EGL_NO_SURFACE) {
        if(data->native_window) {
            ANativeWindow_release(data->native_window);
        }
        data->native_window = Android_JNI_GetNativeWindow();
        data->egl_surface = SDL_EGL_CreateSurface(_this, (NativeWindowType) data->native_window);
    }
    
    /* GL Context handling is done in the event loop because this function is run from the Java thread */
    
}

/* Surface Destroyed */
void Java_org_libsdl_app_SDLActivity_onNativeSurfaceDestroyed(JNIEnv* env, jclass jcls)
{
    /* We have to clear the current context and destroy the egl surface here
     * Otherwise there's BAD_NATIVE_WINDOW errors coming from eglCreateWindowSurface on resume
     * Ref: http://stackoverflow.com/questions/8762589/eglcreatewindowsurface-on-ics-and-switching-from-2d-to-3d
     */
    SDL_WindowData *data;
    SDL_VideoDevice *_this;
    
    if (Android_Window == NULL || Android_Window->driverdata == NULL ) {
        return;
    }
    
    _this =  SDL_GetVideoDevice();
    data = (SDL_WindowData *) Android_Window->driverdata;
    
    if (data->egl_surface != EGL_NO_SURFACE) {
        SDL_EGL_MakeCurrent(_this, NULL, NULL);
        SDL_EGL_DestroySurface(_this, data->egl_surface);
        data->egl_surface = EGL_NO_SURFACE;
    }
    
    /* GL Context handling is done in the event loop because this function is run from the Java thread */

}

void Java_org_libsdl_app_SDLActivity_nativeFlipBuffers(JNIEnv* env, jclass jcls)
{
    SDL_GL_SwapWindow(Android_Window);
}

/* Keydown */
void Java_org_libsdl_app_SDLActivity_onNativeKeyDown(
                                    JNIEnv* env, jclass jcls, jint keycode)
{
    Android_OnKeyDown(keycode);
}

/* Keyup */
void Java_org_libsdl_app_SDLActivity_onNativeKeyUp(
                                    JNIEnv* env, jclass jcls, jint keycode)
{
    Android_OnKeyUp(keycode);
}

/* Keyboard Focus Lost */
void Java_org_libsdl_app_SDLActivity_onNativeKeyboardFocusLost(
                                    JNIEnv* env, jclass jcls)
{
    /* Calling SDL_StopTextInput will take care of hiding the keyboard and cleaning up the DummyText widget */
    SDL_StopTextInput();
}


/* Touch */
void Java_org_libsdl_app_SDLActivity_onNativeTouch(
                                    JNIEnv* env, jclass jcls,
                                    jint touch_device_id_in, jint pointer_finger_id_in,
                                    jint action, jfloat x, jfloat y, jfloat p)
{
    Android_OnTouch(touch_device_id_in, pointer_finger_id_in, action, x, y, p);
}

/* Accelerometer */
void Java_org_libsdl_app_SDLActivity_onNativeAccel(
                                    JNIEnv* env, jclass jcls,
                                    jfloat x, jfloat y, jfloat z)
{
    fLastAccelerometer[0] = x;
    fLastAccelerometer[1] = y;
    fLastAccelerometer[2] = z;
    bHasNewData = true;
}

/* Low memory */
void Java_org_libsdl_app_SDLActivity_nativeLowMemory(
                                    JNIEnv* env, jclass cls)
{
    SDL_SendAppEvent(SDL_APP_LOWMEMORY);
}

/* Quit */
void Java_org_libsdl_app_SDLActivity_nativeQuit(
                                    JNIEnv* env, jclass cls)
{
    // Urho3D: added log print
    __android_log_print(ANDROID_LOG_VERBOSE, "SDL", "nativeQuit()");

    /* Discard previous events. The user should have handled state storage
     * in SDL_APP_WILLENTERBACKGROUND. After nativeQuit() is called, no
     * events other than SDL_QUIT and SDL_APP_TERMINATING should fire */
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    /* Inject a SDL_QUIT event */
    SDL_SendQuit();
    SDL_SendAppEvent(SDL_APP_TERMINATING);
    /* Resume the event loop so that the app can catch SDL_QUIT which
     * should now be the top event in the event queue. */
    if (!SDL_SemValue(Android_ResumeSem)) SDL_SemPost(Android_ResumeSem);
}

/* Pause */
void Java_org_libsdl_app_SDLActivity_nativePause(
                                    JNIEnv* env, jclass cls)
{
    __android_log_print(ANDROID_LOG_VERBOSE, "SDL", "nativePause()");
    if (Android_Window) {
        SDL_SendWindowEvent(Android_Window, SDL_WINDOWEVENT_FOCUS_LOST, 0, 0);
        SDL_SendWindowEvent(Android_Window, SDL_WINDOWEVENT_MINIMIZED, 0, 0);
        SDL_SendAppEvent(SDL_APP_WILLENTERBACKGROUND);
        SDL_SendAppEvent(SDL_APP_DIDENTERBACKGROUND);
    
        /* *After* sending the relevant events, signal the pause semaphore 
         * so the event loop knows to pause and (optionally) block itself */
        if (!SDL_SemValue(Android_PauseSem)) SDL_SemPost(Android_PauseSem);
    }
}

/* Resume */
void Java_org_libsdl_app_SDLActivity_nativeResume(
                                    JNIEnv* env, jclass cls)
{
    __android_log_print(ANDROID_LOG_VERBOSE, "SDL", "nativeResume()");

    if (Android_Window) {
        SDL_SendAppEvent(SDL_APP_WILLENTERFOREGROUND);
        SDL_SendAppEvent(SDL_APP_DIDENTERFOREGROUND);
        SDL_SendWindowEvent(Android_Window, SDL_WINDOWEVENT_FOCUS_GAINED, 0, 0);
        SDL_SendWindowEvent(Android_Window, SDL_WINDOWEVENT_RESTORED, 0, 0);
        /* Signal the resume semaphore so the event loop knows to resume and restore the GL Context
         * We can't restore the GL Context here because it needs to be done on the SDL main thread
         * and this function will be called from the Java thread instead.
         */
        if (!SDL_SemValue(Android_ResumeSem)) SDL_SemPost(Android_ResumeSem);
    }
}

void Java_org_libsdl_app_SDLInputConnection_nativeCommitText(
                                    JNIEnv* env, jclass cls,
                                    jstring text, jint newCursorPosition)
{
    const char *utftext = (*env)->GetStringUTFChars(env, text, NULL);

    SDL_SendKeyboardText(utftext);

    (*env)->ReleaseStringUTFChars(env, text, utftext);
}

void Java_org_libsdl_app_SDLInputConnection_nativeSetComposingText(
                                    JNIEnv* env, jclass cls,
                                    jstring text, jint newCursorPosition)
{
    const char *utftext = (*env)->GetStringUTFChars(env, text, NULL);

    SDL_SendEditingText(utftext, 0, 0);

    (*env)->ReleaseStringUTFChars(env, text, utftext);
}



/*******************************************************************************
             Functions called by SDL into Java
*******************************************************************************/

static int s_active = 0;
struct LocalReferenceHolder
{
    JNIEnv *m_env;
    const char *m_func;
};

static struct LocalReferenceHolder LocalReferenceHolder_Setup(const char *func)
{
    struct LocalReferenceHolder refholder;
    refholder.m_env = NULL;
    refholder.m_func = func;
#ifdef DEBUG_JNI
    SDL_Log("Entering function %s", func);
#endif
    return refholder;
}

static SDL_bool LocalReferenceHolder_Init(struct LocalReferenceHolder *refholder, JNIEnv *env)
{
    const int capacity = 16;
    if ((*env)->PushLocalFrame(env, capacity) < 0) {
        SDL_SetError("Failed to allocate enough JVM local references");
        return SDL_FALSE;
    }
    ++s_active;
    refholder->m_env = env;
    return SDL_TRUE;
}

static void LocalReferenceHolder_Cleanup(struct LocalReferenceHolder *refholder)
{
#ifdef DEBUG_JNI
    SDL_Log("Leaving function %s", refholder->m_func);
#endif
    if (refholder->m_env) {
        JNIEnv* env = refholder->m_env;
        (*env)->PopLocalFrame(env, NULL);
        --s_active;
    }
}

static SDL_bool LocalReferenceHolder_IsActive()
{
    return s_active > 0;    
}

ANativeWindow* Android_JNI_GetNativeWindow(void)
{
    ANativeWindow* anw;
    jobject s;
    JNIEnv *env = Android_JNI_GetEnv();

    s = (*env)->CallStaticObjectMethod(env, mActivityClass, midGetNativeSurface);
    anw = ANativeWindow_fromSurface(env, s);
    (*env)->DeleteLocalRef(env, s);
  
    return anw;
}

void Android_JNI_SwapWindow()
{
    JNIEnv *mEnv = Android_JNI_GetEnv();
    (*mEnv)->CallStaticVoidMethod(mEnv, mActivityClass, midFlipBuffers);
}

void Android_JNI_SetActivityTitle(const char *title)
{
    jmethodID mid;
    JNIEnv *mEnv = Android_JNI_GetEnv();
    mid = (*mEnv)->GetStaticMethodID(mEnv, mActivityClass,"setActivityTitle","(Ljava/lang/String;)Z");
    if (mid) {
        jstring jtitle = (jstring)((*mEnv)->NewStringUTF(mEnv, title));
        (*mEnv)->CallStaticBooleanMethod(mEnv, mActivityClass, mid, jtitle);
        (*mEnv)->DeleteLocalRef(mEnv, jtitle);
    }
}

SDL_bool Android_JNI_GetAccelerometerValues(float values[3])
{
    int i;
    SDL_bool retval = SDL_FALSE;

    if (bHasNewData) {
        for (i = 0; i < 3; ++i) {
            values[i] = fLastAccelerometer[i];
        }
        bHasNewData = false;
        retval = SDL_TRUE;
    }

    return retval;
}

static void Android_JNI_ThreadDestroyed(void* value)
{
    /* The thread is being destroyed, detach it from the Java VM and set the mThreadKey value to NULL as required */
    JNIEnv *env = (JNIEnv*) value;
    if (env != NULL) {
        (*mJavaVM)->DetachCurrentThread(mJavaVM);
        pthread_setspecific(mThreadKey, NULL);
    }
}

JNIEnv* Android_JNI_GetEnv(void)
{
    /* From http://developer.android.com/guide/practices/jni.html
     * All threads are Linux threads, scheduled by the kernel.
     * They're usually started from managed code (using Thread.start), but they can also be created elsewhere and then
     * attached to the JavaVM. For example, a thread started with pthread_create can be attached with the
     * JNI AttachCurrentThread or AttachCurrentThreadAsDaemon functions. Until a thread is attached, it has no JNIEnv,
     * and cannot make JNI calls.
     * Attaching a natively-created thread causes a java.lang.Thread object to be constructed and added to the "main"
     * ThreadGroup, making it visible to the debugger. Calling AttachCurrentThread on an already-attached thread
     * is a no-op.
     * Note: You can call this function any number of times for the same thread, there's no harm in it
     */

    JNIEnv *env;
    int status = (*mJavaVM)->AttachCurrentThread(mJavaVM, &env, NULL);
    if(status < 0) {
        LOGE("failed to attach current thread");
        return 0;
    }

    /* From http://developer.android.com/guide/practices/jni.html
     * Threads attached through JNI must call DetachCurrentThread before they exit. If coding this directly is awkward,
     * in Android 2.0 (Eclair) and higher you can use pthread_key_create to define a destructor function that will be
     * called before the thread exits, and call DetachCurrentThread from there. (Use that key with pthread_setspecific
     * to store the JNIEnv in thread-local-storage; that way it'll be passed into your destructor as the argument.)
     * Note: The destructor is not called unless the stored value is != NULL
     * Note: You can call this function any number of times for the same thread, there's no harm in it
     *       (except for some lost CPU cycles)
     */
    pthread_setspecific(mThreadKey, (void*) env);

    return env;
}

int Android_JNI_SetupThread(void)
{
    Android_JNI_GetEnv();
    return 1;
}

/*
 * Audio support
 */
static jboolean audioBuffer16Bit = JNI_FALSE;
static jboolean audioBufferStereo = JNI_FALSE;
static jobject audioBuffer = NULL;
static void* audioBufferPinned = NULL;

int Android_JNI_OpenAudioDevice(int sampleRate, int is16Bit, int channelCount, int desiredBufferFrames)
{
    int audioBufferFrames;

    JNIEnv *env = Android_JNI_GetEnv();

    if (!env) {
        LOGE("callback_handler: failed to attach current thread");
    }
    Android_JNI_SetupThread();

    __android_log_print(ANDROID_LOG_VERBOSE, "SDL", "SDL audio: opening device");
    audioBuffer16Bit = is16Bit;
    audioBufferStereo = channelCount > 1;

    if ((*env)->CallStaticIntMethod(env, mActivityClass, midAudioInit, sampleRate, audioBuffer16Bit, audioBufferStereo, desiredBufferFrames) != 0) {
        /* Error during audio initialization */
        __android_log_print(ANDROID_LOG_WARN, "SDL", "SDL audio: error on AudioTrack initialization!");
        return 0;
    }

    /* Allocating the audio buffer from the Java side and passing it as the return value for audioInit no longer works on
     * Android >= 4.2 due to a "stale global reference" error. So now we allocate this buffer directly from this side. */

    if (is16Bit) {
        jshortArray audioBufferLocal = (*env)->NewShortArray(env, desiredBufferFrames * (audioBufferStereo ? 2 : 1));
        if (audioBufferLocal) {
            audioBuffer = (*env)->NewGlobalRef(env, audioBufferLocal);
            (*env)->DeleteLocalRef(env, audioBufferLocal);
        }
    }
    else {
        jbyteArray audioBufferLocal = (*env)->NewByteArray(env, desiredBufferFrames * (audioBufferStereo ? 2 : 1));
        if (audioBufferLocal) {
            audioBuffer = (*env)->NewGlobalRef(env, audioBufferLocal);
            (*env)->DeleteLocalRef(env, audioBufferLocal);
        }
    }

    if (audioBuffer == NULL) {
        __android_log_print(ANDROID_LOG_WARN, "SDL", "SDL audio: could not allocate an audio buffer!");
        return 0;
    }

    jboolean isCopy = JNI_FALSE;
    if (audioBuffer16Bit) {
        audioBufferPinned = (*env)->GetShortArrayElements(env, (jshortArray)audioBuffer, &isCopy);
        audioBufferFrames = (*env)->GetArrayLength(env, (jshortArray)audioBuffer);
    } else {
        audioBufferPinned = (*env)->GetByteArrayElements(env, (jbyteArray)audioBuffer, &isCopy);
        audioBufferFrames = (*env)->GetArrayLength(env, (jbyteArray)audioBuffer);
    }
    if (audioBufferStereo) {
        audioBufferFrames /= 2;
    }

    return audioBufferFrames;
}

void * Android_JNI_GetAudioBuffer()
{
    return audioBufferPinned;
}

void Android_JNI_WriteAudioBuffer()
{
    JNIEnv *mAudioEnv = Android_JNI_GetEnv();

    if (audioBuffer16Bit) {
        (*mAudioEnv)->ReleaseShortArrayElements(mAudioEnv, (jshortArray)audioBuffer, (jshort *)audioBufferPinned, JNI_COMMIT);
        (*mAudioEnv)->CallStaticVoidMethod(mAudioEnv, mActivityClass, midAudioWriteShortBuffer, (jshortArray)audioBuffer);
    } else {
        (*mAudioEnv)->ReleaseByteArrayElements(mAudioEnv, (jbyteArray)audioBuffer, (jbyte *)audioBufferPinned, JNI_COMMIT);
        (*mAudioEnv)->CallStaticVoidMethod(mAudioEnv, mActivityClass, midAudioWriteByteBuffer, (jbyteArray)audioBuffer);
    }

    /* JNI_COMMIT means the changes are committed to the VM but the buffer remains pinned */
}

void Android_JNI_CloseAudioDevice()
{
    JNIEnv *env = Android_JNI_GetEnv();

    (*env)->CallStaticVoidMethod(env, mActivityClass, midAudioQuit);

    if (audioBuffer) {
        (*env)->DeleteGlobalRef(env, audioBuffer);
        audioBuffer = NULL;
        audioBufferPinned = NULL;
    }
}

/* Test for an exception and call SDL_SetError with its detail if one occurs */
/* If the parameter silent is truthy then SDL_SetError() will not be called. */
static bool Android_JNI_ExceptionOccurred(bool silent)
{
    SDL_assert(LocalReferenceHolder_IsActive());
    JNIEnv *mEnv = Android_JNI_GetEnv();

    jthrowable exception = (*mEnv)->ExceptionOccurred(mEnv);
    if (exception != NULL) {
        jmethodID mid;

        /* Until this happens most JNI operations have undefined behaviour */
        (*mEnv)->ExceptionClear(mEnv);

        if (!silent) {
            jclass exceptionClass = (*mEnv)->GetObjectClass(mEnv, exception);
            jclass classClass = (*mEnv)->FindClass(mEnv, "java/lang/Class");

            mid = (*mEnv)->GetMethodID(mEnv, classClass, "getName", "()Ljava/lang/String;");
            jstring exceptionName = (jstring)(*mEnv)->CallObjectMethod(mEnv, exceptionClass, mid);
            const char* exceptionNameUTF8 = (*mEnv)->GetStringUTFChars(mEnv, exceptionName, 0);

            mid = (*mEnv)->GetMethodID(mEnv, exceptionClass, "getMessage", "()Ljava/lang/String;");
            jstring exceptionMessage = (jstring)(*mEnv)->CallObjectMethod(mEnv, exception, mid);

            if (exceptionMessage != NULL) {
                const char* exceptionMessageUTF8 = (*mEnv)->GetStringUTFChars(mEnv, exceptionMessage, 0);
                SDL_SetError("%s: %s", exceptionNameUTF8, exceptionMessageUTF8);
                (*mEnv)->ReleaseStringUTFChars(mEnv, exceptionMessage, exceptionMessageUTF8);
            } else {
                SDL_SetError("%s", exceptionNameUTF8);
            }

            (*mEnv)->ReleaseStringUTFChars(mEnv, exceptionName, exceptionNameUTF8);
        }

        return true;
    }

    return false;
}

static int Internal_Android_JNI_FileOpen(SDL_RWops* ctx)
{
    struct LocalReferenceHolder refs = LocalReferenceHolder_Setup(__FUNCTION__);

    int result = 0;

    jmethodID mid;
    jobject context;
    jobject assetManager;
    jobject inputStream;
    jclass channels;
    jobject readableByteChannel;
    jstring fileNameJString;
    jobject fd;
    jclass fdCls;
    jfieldID descriptor;

    JNIEnv *mEnv = Android_JNI_GetEnv();
    if (!LocalReferenceHolder_Init(&refs, mEnv)) {
        goto failure;
    }

    fileNameJString = (jstring)ctx->hidden.androidio.fileNameRef;
    ctx->hidden.androidio.position = 0;

    /* context = SDLActivity.getContext(); */
    mid = (*mEnv)->GetStaticMethodID(mEnv, mActivityClass,
            "getContext","()Landroid/content/Context;");
    context = (*mEnv)->CallStaticObjectMethod(mEnv, mActivityClass, mid);


    /* assetManager = context.getAssets(); */
    mid = (*mEnv)->GetMethodID(mEnv, (*mEnv)->GetObjectClass(mEnv, context),
            "getAssets", "()Landroid/content/res/AssetManager;");
    assetManager = (*mEnv)->CallObjectMethod(mEnv, context, mid);

    /* First let's try opening the file to obtain an AssetFileDescriptor.
    * This method reads the files directly from the APKs using standard *nix calls
    */
    mid = (*mEnv)->GetMethodID(mEnv, (*mEnv)->GetObjectClass(mEnv, assetManager), "openFd", "(Ljava/lang/String;)Landroid/content/res/AssetFileDescriptor;");
    inputStream = (*mEnv)->CallObjectMethod(mEnv, assetManager, mid, fileNameJString);
    if (Android_JNI_ExceptionOccurred(true)) {
        goto fallback;
    }

    mid = (*mEnv)->GetMethodID(mEnv, (*mEnv)->GetObjectClass(mEnv, inputStream), "getStartOffset", "()J");
    ctx->hidden.androidio.offset = (*mEnv)->CallLongMethod(mEnv, inputStream, mid);
    if (Android_JNI_ExceptionOccurred(true)) {
        goto fallback;
    }

    mid = (*mEnv)->GetMethodID(mEnv, (*mEnv)->GetObjectClass(mEnv, inputStream), "getDeclaredLength", "()J");
    ctx->hidden.androidio.size = (*mEnv)->CallLongMethod(mEnv, inputStream, mid);
    if (Android_JNI_ExceptionOccurred(true)) {
        goto fallback;
    }

    mid = (*mEnv)->GetMethodID(mEnv, (*mEnv)->GetObjectClass(mEnv, inputStream), "getFileDescriptor", "()Ljava/io/FileDescriptor;");
    fd = (*mEnv)->CallObjectMethod(mEnv, inputStream, mid);
    fdCls = (*mEnv)->GetObjectClass(mEnv, fd);
    descriptor = (*mEnv)->GetFieldID(mEnv, fdCls, "descriptor", "I");
    ctx->hidden.androidio.fd = (*mEnv)->GetIntField(mEnv, fd, descriptor);
    ctx->hidden.androidio.assetFileDescriptorRef = (*mEnv)->NewGlobalRef(mEnv, inputStream);

    /* Seek to the correct offset in the file. */
    lseek(ctx->hidden.androidio.fd, (off_t)ctx->hidden.androidio.offset, SEEK_SET);

    if (false) {
fallback:
        /* Disabled log message because of spam on the Nexus 7 */
        /* __android_log_print(ANDROID_LOG_DEBUG, "SDL", "Falling back to legacy InputStream method for opening file"); */

        /* Try the old method using InputStream */
        ctx->hidden.androidio.assetFileDescriptorRef = NULL;

        /* inputStream = assetManager.open(<filename>); */
        mid = (*mEnv)->GetMethodID(mEnv, (*mEnv)->GetObjectClass(mEnv, assetManager),
                "open", "(Ljava/lang/String;I)Ljava/io/InputStream;");
        inputStream = (*mEnv)->CallObjectMethod(mEnv, assetManager, mid, fileNameJString, 1 /* ACCESS_RANDOM */);
        if (Android_JNI_ExceptionOccurred(false)) {
            goto failure;
        }

        ctx->hidden.androidio.inputStreamRef = (*mEnv)->NewGlobalRef(mEnv, inputStream);

        /* Despite all the visible documentation on [Asset]InputStream claiming
         * that the .available() method is not guaranteed to return the entire file
         * size, comments in <sdk>/samples/<ver>/ApiDemos/src/com/example/ ...
         * android/apis/content/ReadAsset.java imply that Android's
         * AssetInputStream.available() /will/ always return the total file size
        */
        
        /* size = inputStream.available(); */
        mid = (*mEnv)->GetMethodID(mEnv, (*mEnv)->GetObjectClass(mEnv, inputStream),
                "available", "()I");
        ctx->hidden.androidio.size = (long)(*mEnv)->CallIntMethod(mEnv, inputStream, mid);
        if (Android_JNI_ExceptionOccurred(false)) {
            goto failure;
        }

        /* readableByteChannel = Channels.newChannel(inputStream); */
        channels = (*mEnv)->FindClass(mEnv, "java/nio/channels/Channels");
        mid = (*mEnv)->GetStaticMethodID(mEnv, channels,
                "newChannel",
                "(Ljava/io/InputStream;)Ljava/nio/channels/ReadableByteChannel;");
        readableByteChannel = (*mEnv)->CallStaticObjectMethod(
                mEnv, channels, mid, inputStream);
        if (Android_JNI_ExceptionOccurred(false)) {
            goto failure;
        }

        ctx->hidden.androidio.readableByteChannelRef =
            (*mEnv)->NewGlobalRef(mEnv, readableByteChannel);

        /* Store .read id for reading purposes */
        mid = (*mEnv)->GetMethodID(mEnv, (*mEnv)->GetObjectClass(mEnv, readableByteChannel),
                "read", "(Ljava/nio/ByteBuffer;)I");
        ctx->hidden.androidio.readMethod = mid;
    }

    if (false) {
failure:
        result = -1;

        (*mEnv)->DeleteGlobalRef(mEnv, (jobject)ctx->hidden.androidio.fileNameRef);

        if(ctx->hidden.androidio.inputStreamRef != NULL) {
            (*mEnv)->DeleteGlobalRef(mEnv, (jobject)ctx->hidden.androidio.inputStreamRef);
        }

        if(ctx->hidden.androidio.readableByteChannelRef != NULL) {
            (*mEnv)->DeleteGlobalRef(mEnv, (jobject)ctx->hidden.androidio.readableByteChannelRef);
        }

        if(ctx->hidden.androidio.assetFileDescriptorRef != NULL) {
            (*mEnv)->DeleteGlobalRef(mEnv, (jobject)ctx->hidden.androidio.assetFileDescriptorRef);
        }

    }
    
    LocalReferenceHolder_Cleanup(&refs);
    return result;
}

int Android_JNI_FileOpen(SDL_RWops* ctx,
        const char* fileName, const char* mode)
{
    struct LocalReferenceHolder refs = LocalReferenceHolder_Setup(__FUNCTION__);
    JNIEnv *mEnv = Android_JNI_GetEnv();
    int retval;

    if (!LocalReferenceHolder_Init(&refs, mEnv)) {
        LocalReferenceHolder_Cleanup(&refs);        
        return -1;
    }

    if (!ctx) {
        LocalReferenceHolder_Cleanup(&refs);
        return -1;
    }

    jstring fileNameJString = (*mEnv)->NewStringUTF(mEnv, fileName);
    ctx->hidden.androidio.fileNameRef = (*mEnv)->NewGlobalRef(mEnv, fileNameJString);
    ctx->hidden.androidio.inputStreamRef = NULL;
    ctx->hidden.androidio.readableByteChannelRef = NULL;
    ctx->hidden.androidio.readMethod = NULL;
    ctx->hidden.androidio.assetFileDescriptorRef = NULL;

    retval = Internal_Android_JNI_FileOpen(ctx);
    LocalReferenceHolder_Cleanup(&refs);
    return retval;
}

size_t Android_JNI_FileRead(SDL_RWops* ctx, void* buffer,
        size_t size, size_t maxnum)
{
    struct LocalReferenceHolder refs = LocalReferenceHolder_Setup(__FUNCTION__);

    if (ctx->hidden.androidio.assetFileDescriptorRef) {
        size_t bytesMax = size * maxnum;
        if (ctx->hidden.androidio.size != -1 /* UNKNOWN_LENGTH */ && ctx->hidden.androidio.position + bytesMax > ctx->hidden.androidio.size) {
            bytesMax = ctx->hidden.androidio.size - ctx->hidden.androidio.position;
        }
        size_t result = read(ctx->hidden.androidio.fd, buffer, bytesMax );
        if (result > 0) {
            ctx->hidden.androidio.position += result;
            LocalReferenceHolder_Cleanup(&refs);
            return result / size;
        }
        LocalReferenceHolder_Cleanup(&refs);
        return 0;
    } else {
        jlong bytesRemaining = (jlong) (size * maxnum);
        jlong bytesMax = (jlong) (ctx->hidden.androidio.size -  ctx->hidden.androidio.position);
        int bytesRead = 0;

        /* Don't read more bytes than those that remain in the file, otherwise we get an exception */
        if (bytesRemaining >  bytesMax) bytesRemaining = bytesMax;

        JNIEnv *mEnv = Android_JNI_GetEnv();
        if (!LocalReferenceHolder_Init(&refs, mEnv)) {
            LocalReferenceHolder_Cleanup(&refs);            
            return 0;
        }

        jobject readableByteChannel = (jobject)ctx->hidden.androidio.readableByteChannelRef;
        jmethodID readMethod = (jmethodID)ctx->hidden.androidio.readMethod;
        jobject byteBuffer = (*mEnv)->NewDirectByteBuffer(mEnv, buffer, bytesRemaining);

        while (bytesRemaining > 0) {
            /* result = readableByteChannel.read(...); */
            int result = (*mEnv)->CallIntMethod(mEnv, readableByteChannel, readMethod, byteBuffer);

            if (Android_JNI_ExceptionOccurred(false)) {
                LocalReferenceHolder_Cleanup(&refs);            
                return 0;
            }

            if (result < 0) {
                break;
            }

            bytesRemaining -= result;
            bytesRead += result;
            ctx->hidden.androidio.position += result;
        }
        LocalReferenceHolder_Cleanup(&refs);                    
        return bytesRead / size;
    }
}

size_t Android_JNI_FileWrite(SDL_RWops* ctx, const void* buffer,
        size_t size, size_t num)
{
    SDL_SetError("Cannot write to Android package filesystem");
    return 0;
}

static int Internal_Android_JNI_FileClose(SDL_RWops* ctx, bool release)
{
    struct LocalReferenceHolder refs = LocalReferenceHolder_Setup(__FUNCTION__);

    int result = 0;
    JNIEnv *mEnv = Android_JNI_GetEnv();

    if (!LocalReferenceHolder_Init(&refs, mEnv)) {
        LocalReferenceHolder_Cleanup(&refs);
        return SDL_SetError("Failed to allocate enough JVM local references");
    }

    if (ctx) {
        if (release) {
            (*mEnv)->DeleteGlobalRef(mEnv, (jobject)ctx->hidden.androidio.fileNameRef);
        }

        if (ctx->hidden.androidio.assetFileDescriptorRef) {
            jobject inputStream = (jobject)ctx->hidden.androidio.assetFileDescriptorRef;
            jmethodID mid = (*mEnv)->GetMethodID(mEnv, (*mEnv)->GetObjectClass(mEnv, inputStream),
                    "close", "()V");
            (*mEnv)->CallVoidMethod(mEnv, inputStream, mid);
            (*mEnv)->DeleteGlobalRef(mEnv, (jobject)ctx->hidden.androidio.assetFileDescriptorRef);
            if (Android_JNI_ExceptionOccurred(false)) {
                result = -1;
            }
        }
        else {
            jobject inputStream = (jobject)ctx->hidden.androidio.inputStreamRef;

            /* inputStream.close(); */
            jmethodID mid = (*mEnv)->GetMethodID(mEnv, (*mEnv)->GetObjectClass(mEnv, inputStream),
                    "close", "()V");
            (*mEnv)->CallVoidMethod(mEnv, inputStream, mid);
            (*mEnv)->DeleteGlobalRef(mEnv, (jobject)ctx->hidden.androidio.inputStreamRef);
            (*mEnv)->DeleteGlobalRef(mEnv, (jobject)ctx->hidden.androidio.readableByteChannelRef);
            if (Android_JNI_ExceptionOccurred(false)) {
                result = -1;
            }
        }

        if (release) {
            SDL_FreeRW(ctx);
        }
    }

    LocalReferenceHolder_Cleanup(&refs);
    return result;
}


Sint64 Android_JNI_FileSize(SDL_RWops* ctx)
{
    return ctx->hidden.androidio.size;
}

Sint64 Android_JNI_FileSeek(SDL_RWops* ctx, Sint64 offset, int whence)
{
    if (ctx->hidden.androidio.assetFileDescriptorRef) {
        switch (whence) {
            case RW_SEEK_SET:
                if (ctx->hidden.androidio.size != -1 /* UNKNOWN_LENGTH */ && offset > ctx->hidden.androidio.size) offset = ctx->hidden.androidio.size;
                offset += ctx->hidden.androidio.offset;
                break;
            case RW_SEEK_CUR:
                offset += ctx->hidden.androidio.position;
                if (ctx->hidden.androidio.size != -1 /* UNKNOWN_LENGTH */ && offset > ctx->hidden.androidio.size) offset = ctx->hidden.androidio.size;
                offset += ctx->hidden.androidio.offset;
                break;
            case RW_SEEK_END:
                offset = ctx->hidden.androidio.offset + ctx->hidden.androidio.size + offset;
                break;
            default:
                return SDL_SetError("Unknown value for 'whence'");
        }
        whence = SEEK_SET;

        off_t ret = lseek(ctx->hidden.androidio.fd, (off_t)offset, SEEK_SET);
        if (ret == -1) return -1;
        ctx->hidden.androidio.position = ret - ctx->hidden.androidio.offset;
    } else {
        Sint64 newPosition;

        switch (whence) {
            case RW_SEEK_SET:
                newPosition = offset;
                break;
            case RW_SEEK_CUR:
                newPosition = ctx->hidden.androidio.position + offset;
                break;
            case RW_SEEK_END:
                newPosition = ctx->hidden.androidio.size + offset;
                break;
            default:
                return SDL_SetError("Unknown value for 'whence'");
        }

        /* Validate the new position */
        if (newPosition < 0) {
            return SDL_Error(SDL_EFSEEK);
        }
        if (newPosition > ctx->hidden.androidio.size) {
            newPosition = ctx->hidden.androidio.size;
        }

        Sint64 movement = newPosition - ctx->hidden.androidio.position;
        if (movement > 0) {
            unsigned char buffer[4096];

            /* The easy case where we're seeking forwards */
            while (movement > 0) {
                Sint64 amount = sizeof (buffer);
                if (amount > movement) {
                    amount = movement;
                }
                size_t result = Android_JNI_FileRead(ctx, buffer, 1, amount);
                if (result <= 0) {
                    /* Failed to read/skip the required amount, so fail */
                    return -1;
                }

                movement -= result;
            }

        } else if (movement < 0) {
            /* We can't seek backwards so we have to reopen the file and seek */
            /* forwards which obviously isn't very efficient */
            Internal_Android_JNI_FileClose(ctx, false);
            Internal_Android_JNI_FileOpen(ctx);
            Android_JNI_FileSeek(ctx, newPosition, RW_SEEK_SET);
        }
    }

    return ctx->hidden.androidio.position;

}

int Android_JNI_FileClose(SDL_RWops* ctx)
{
    return Internal_Android_JNI_FileClose(ctx, true);
}

/* returns a new global reference which needs to be released later */
static jobject Android_JNI_GetSystemServiceObject(const char* name)
{
    struct LocalReferenceHolder refs = LocalReferenceHolder_Setup(__FUNCTION__);
    JNIEnv* env = Android_JNI_GetEnv();
    jobject retval = NULL;

    if (!LocalReferenceHolder_Init(&refs, env)) {
        LocalReferenceHolder_Cleanup(&refs);
        return NULL;
    }

    jstring service = (*env)->NewStringUTF(env, name);

    jmethodID mid;

    mid = (*env)->GetStaticMethodID(env, mActivityClass, "getContext", "()Landroid/content/Context;");
    jobject context = (*env)->CallStaticObjectMethod(env, mActivityClass, mid);

    mid = (*env)->GetMethodID(env, mActivityClass, "getSystemServiceFromUiThread", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject manager = (*env)->CallObjectMethod(env, context, mid, service);

    (*env)->DeleteLocalRef(env, service);

    retval = manager ? (*env)->NewGlobalRef(env, manager) : NULL;
    LocalReferenceHolder_Cleanup(&refs);
    return retval;
}

#define SETUP_CLIPBOARD(error) \
    struct LocalReferenceHolder refs = LocalReferenceHolder_Setup(__FUNCTION__); \
    JNIEnv* env = Android_JNI_GetEnv(); \
    if (!LocalReferenceHolder_Init(&refs, env)) { \
        LocalReferenceHolder_Cleanup(&refs); \
        return error; \
    } \
    jobject clipboard = Android_JNI_GetSystemServiceObject("clipboard"); \
    if (!clipboard) { \
        LocalReferenceHolder_Cleanup(&refs); \
        return error; \
    }

#define CLEANUP_CLIPBOARD() \
    LocalReferenceHolder_Cleanup(&refs);

int Android_JNI_SetClipboardText(const char* text)
{
    SETUP_CLIPBOARD(-1)

    jmethodID mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, clipboard), "setText", "(Ljava/lang/CharSequence;)V");
    jstring string = (*env)->NewStringUTF(env, text);
    (*env)->CallVoidMethod(env, clipboard, mid, string);
    (*env)->DeleteGlobalRef(env, clipboard);
    (*env)->DeleteLocalRef(env, string);

    CLEANUP_CLIPBOARD();

    return 0;
}

char* Android_JNI_GetClipboardText()
{
    SETUP_CLIPBOARD(SDL_strdup(""))

    jmethodID mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, clipboard), "getText", "()Ljava/lang/CharSequence;");
    jobject sequence = (*env)->CallObjectMethod(env, clipboard, mid);
    (*env)->DeleteGlobalRef(env, clipboard);
    if (sequence) {
        mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, sequence), "toString", "()Ljava/lang/String;");
        jstring string = (jstring)((*env)->CallObjectMethod(env, sequence, mid));
        const char* utf = (*env)->GetStringUTFChars(env, string, 0);
        if (utf) {
            char* text = SDL_strdup(utf);
            (*env)->ReleaseStringUTFChars(env, string, utf);

            CLEANUP_CLIPBOARD();

            return text;
        }
    }

    CLEANUP_CLIPBOARD();    

    return SDL_strdup("");
}

SDL_bool Android_JNI_HasClipboardText()
{
    SETUP_CLIPBOARD(SDL_FALSE)

    jmethodID mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, clipboard), "hasText", "()Z");
    jboolean has = (*env)->CallBooleanMethod(env, clipboard, mid);
    (*env)->DeleteGlobalRef(env, clipboard);

    CLEANUP_CLIPBOARD();
    
    return has ? SDL_TRUE : SDL_FALSE;
}


/* returns 0 on success or -1 on error (others undefined then)
 * returns truthy or falsy value in plugged, charged and battery
 * returns the value in seconds and percent or -1 if not available
 */
int Android_JNI_GetPowerInfo(int* plugged, int* charged, int* battery, int* seconds, int* percent)
{
    struct LocalReferenceHolder refs = LocalReferenceHolder_Setup(__FUNCTION__);
    JNIEnv* env = Android_JNI_GetEnv();
    if (!LocalReferenceHolder_Init(&refs, env)) {
        LocalReferenceHolder_Cleanup(&refs);
        return -1;
    }

    jmethodID mid;

    mid = (*env)->GetStaticMethodID(env, mActivityClass, "getContext", "()Landroid/content/Context;");
    jobject context = (*env)->CallStaticObjectMethod(env, mActivityClass, mid);

    jstring action = (*env)->NewStringUTF(env, "android.intent.action.BATTERY_CHANGED");

    jclass cls = (*env)->FindClass(env, "android/content/IntentFilter");

    mid = (*env)->GetMethodID(env, cls, "<init>", "(Ljava/lang/String;)V");
    jobject filter = (*env)->NewObject(env, cls, mid, action);

    (*env)->DeleteLocalRef(env, action);

    mid = (*env)->GetMethodID(env, mActivityClass, "registerReceiver", "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;");
    jobject intent = (*env)->CallObjectMethod(env, context, mid, NULL, filter);

    (*env)->DeleteLocalRef(env, filter);

    cls = (*env)->GetObjectClass(env, intent);

    jstring iname;
    jmethodID imid = (*env)->GetMethodID(env, cls, "getIntExtra", "(Ljava/lang/String;I)I");

#define GET_INT_EXTRA(var, key) \
    iname = (*env)->NewStringUTF(env, key); \
    int var = (*env)->CallIntMethod(env, intent, imid, iname, -1); \
    (*env)->DeleteLocalRef(env, iname);

    jstring bname;
    jmethodID bmid = (*env)->GetMethodID(env, cls, "getBooleanExtra", "(Ljava/lang/String;Z)Z");

#define GET_BOOL_EXTRA(var, key) \
    bname = (*env)->NewStringUTF(env, key); \
    int var = (*env)->CallBooleanMethod(env, intent, bmid, bname, JNI_FALSE); \
    (*env)->DeleteLocalRef(env, bname);

    if (plugged) {
        GET_INT_EXTRA(plug, "plugged") /* == BatteryManager.EXTRA_PLUGGED (API 5) */
        if (plug == -1) {
            LocalReferenceHolder_Cleanup(&refs);
            return -1;
        }
        /* 1 == BatteryManager.BATTERY_PLUGGED_AC */
        /* 2 == BatteryManager.BATTERY_PLUGGED_USB */
        *plugged = (0 < plug) ? 1 : 0;
    }

    if (charged) {
        GET_INT_EXTRA(status, "status") /* == BatteryManager.EXTRA_STATUS (API 5) */
        if (status == -1) {
            LocalReferenceHolder_Cleanup(&refs);
            return -1;
        }
        /* 5 == BatteryManager.BATTERY_STATUS_FULL */
        *charged = (status == 5) ? 1 : 0;
    }

    if (battery) {
        GET_BOOL_EXTRA(present, "present") /* == BatteryManager.EXTRA_PRESENT (API 5) */
        *battery = present ? 1 : 0;
    }

    if (seconds) {
        *seconds = -1; /* not possible */
    }

    if (percent) {
        GET_INT_EXTRA(level, "level") /* == BatteryManager.EXTRA_LEVEL (API 5) */
        GET_INT_EXTRA(scale, "scale") /* == BatteryManager.EXTRA_SCALE (API 5) */
        if ((level == -1) || (scale == -1)) {
            LocalReferenceHolder_Cleanup(&refs);
            return -1;
        }
        *percent = level * 100 / scale;
    }

    (*env)->DeleteLocalRef(env, intent);

    LocalReferenceHolder_Cleanup(&refs);
    return 0;
}

/* returns number of found touch devices as return value and ids in parameter ids */
int Android_JNI_GetTouchDeviceIds(int **ids) {
    JNIEnv *env = Android_JNI_GetEnv();
    jint sources = 4098; /* == InputDevice.SOURCE_TOUCHSCREEN */
    jmethodID mid = (*env)->GetStaticMethodID(env, mActivityClass, "inputGetInputDeviceIds", "(I)[I");
    jintArray array = (jintArray) (*env)->CallStaticObjectMethod(env, mActivityClass, mid, sources);
    int number = 0;
    *ids = NULL;
    if (array) {
        number = (int) (*env)->GetArrayLength(env, array);
        if (0 < number) {
            jint* elements = (*env)->GetIntArrayElements(env, array, NULL);
            if (elements) {
                int i;
                *ids = SDL_malloc(number * sizeof (**ids));
                for (i = 0; i < number; ++i) { /* not assuming sizeof (jint) == sizeof (int) */
                    (*ids)[i] = elements[i];
                }
                (*env)->ReleaseIntArrayElements(env, array, elements, JNI_ABORT);
            }
        }
        (*env)->DeleteLocalRef(env, array);
    }
    return number;
}

void Android_JNI_PollInputDevices()
{
    JNIEnv *env = Android_JNI_GetEnv();
    (*env)->CallStaticVoidMethod(env, mActivityClass, midPollInputDevices);    
}

/* sends message to be handled on the UI event dispatch thread */
int Android_JNI_SendMessage(int command, int param)
{
    JNIEnv *env = Android_JNI_GetEnv();
    if (!env) {
        return -1;
    }
    jmethodID mid = (*env)->GetStaticMethodID(env, mActivityClass, "sendMessage", "(II)Z");
    if (!mid) {
        return -1;
    }
    jboolean success = (*env)->CallStaticBooleanMethod(env, mActivityClass, mid, command, param);
    return success ? 0 : -1;
}

void Android_JNI_ShowTextInput(SDL_Rect *inputRect)
{
    JNIEnv *env = Android_JNI_GetEnv();
    if (!env) {
        return;
    }

    jmethodID mid = (*env)->GetStaticMethodID(env, mActivityClass, "showTextInput", "(IIII)Z");
    if (!mid) {
        return;
    }
    (*env)->CallStaticBooleanMethod(env, mActivityClass, mid,
                               inputRect->x,
                               inputRect->y,
                               inputRect->w,
                               inputRect->h );
}

void Android_JNI_HideTextInput()
{
    /* has to match Activity constant */
    const int COMMAND_TEXTEDIT_HIDE = 3;
    Android_JNI_SendMessage(COMMAND_TEXTEDIT_HIDE, 0);
}

/*
//////////////////////////////////////////////////////////////////////////////
//
// Functions exposed to SDL applications in SDL_system.h
//////////////////////////////////////////////////////////////////////////////
*/

// Urho3D - function to return a list of files under a given path in "assets" directory (caller is responsible to free the C string array)
char** SDL_Android_GetFileList(const char* path, int* count)
{
    struct LocalReferenceHolder refs = LocalReferenceHolder_Setup(__FUNCTION__);
    JNIEnv* mEnv = Android_JNI_GetEnv();
    if (!LocalReferenceHolder_Init(&refs, mEnv))
    {
        LocalReferenceHolder_Cleanup(&refs);
        return NULL;
    }

    jstring pathJString = (*mEnv)->NewStringUTF(mEnv, path);

    /* context = SDLActivity.getContext(); */
    jmethodID mid = (*mEnv)->GetStaticMethodID(mEnv, mActivityClass,
            "getContext","()Landroid/content/Context;");
    jobject context = (*mEnv)->CallStaticObjectMethod(mEnv, mActivityClass, mid);

    /* assetManager = context.getAssets(); */
    mid = (*mEnv)->GetMethodID(mEnv, (*mEnv)->GetObjectClass(mEnv, context),
            "getAssets", "()Landroid/content/res/AssetManager;");
    jobject assetManager = (*mEnv)->CallObjectMethod(mEnv, context, mid);

    /* stringArray = assetManager.list(path) */
    mid = (*mEnv)->GetMethodID(mEnv, (*mEnv)->GetObjectClass(mEnv, assetManager), "list", "(Ljava/lang/String;)[Ljava/lang/String;");
    jobjectArray stringArray = (*mEnv)->CallObjectMethod(mEnv, assetManager, mid, pathJString);
    if (Android_JNI_ExceptionOccurred(true))
    {
        LocalReferenceHolder_Cleanup(&refs);
        return NULL;
    }

    jsize arrayLength = (*mEnv)->GetArrayLength(mEnv, stringArray);
    char** cStringArray = (char**)SDL_malloc(arrayLength * sizeof(char*));
    jint i;
    for (i = 0; i < arrayLength; ++i)
    {
        jstring string = (jstring)(*mEnv)->GetObjectArrayElement(mEnv, stringArray, i);
        const char* cString = (*mEnv)->GetStringUTFChars(mEnv, string, 0);
        cStringArray[i] = cString ? SDL_strdup(cString) : NULL;
        (*mEnv)->ReleaseStringUTFChars(mEnv, string, cString);
    }

    *count = arrayLength;

    LocalReferenceHolder_Cleanup(&refs);
    return cStringArray;
}

// Urho3D - helper function to free the file list returned by SDL_Android_GetFileList()
void SDL_Android_FreeFileList(char*** array, int* count)
{
    int i = *count;
    if ((i > 0) && (*array != NULL))
    {
        while (i--)
            SDL_free((*array)[i]);
    }
    SDL_free(*array);
    *array = NULL;
    *count = 0;
}

void *SDL_AndroidGetJNIEnv()
{
    return Android_JNI_GetEnv();
}

void *SDL_AndroidGetActivity()
{
    /* See SDL_system.h for caveats on using this function. */

    jmethodID mid;

    JNIEnv *env = Android_JNI_GetEnv();
    if (!env) {
        return NULL;
    }

    /* return SDLActivity.getContext(); */
    mid = (*env)->GetStaticMethodID(env, mActivityClass,
            "getContext","()Landroid/content/Context;");
    return (*env)->CallStaticObjectMethod(env, mActivityClass, mid);
}

const char * SDL_AndroidGetInternalStoragePath()
{
    static char *s_AndroidInternalFilesPath = NULL;

    if (!s_AndroidInternalFilesPath) {
        struct LocalReferenceHolder refs = LocalReferenceHolder_Setup(__FUNCTION__);
        jmethodID mid;
        jobject context;
        jobject fileObject;
        jstring pathString;
        const char *path;

        JNIEnv *env = Android_JNI_GetEnv();
        if (!LocalReferenceHolder_Init(&refs, env)) {
            LocalReferenceHolder_Cleanup(&refs);
            return NULL;
        }

        /* context = SDLActivity.getContext(); */
        mid = (*env)->GetStaticMethodID(env, mActivityClass,
                "getContext","()Landroid/content/Context;");
        context = (*env)->CallStaticObjectMethod(env, mActivityClass, mid);

        /* fileObj = context.getFilesDir(); */
        mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, context),
                "getFilesDir", "()Ljava/io/File;");
        fileObject = (*env)->CallObjectMethod(env, context, mid);
        if (!fileObject) {
            SDL_SetError("Couldn't get internal directory");
            LocalReferenceHolder_Cleanup(&refs);
            return NULL;
        }

        /* path = fileObject.getAbsolutePath(); */
        mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, fileObject),
                "getAbsolutePath", "()Ljava/lang/String;");
        pathString = (jstring)(*env)->CallObjectMethod(env, fileObject, mid);

        path = (*env)->GetStringUTFChars(env, pathString, NULL);
        s_AndroidInternalFilesPath = SDL_strdup(path);
        (*env)->ReleaseStringUTFChars(env, pathString, path);

        LocalReferenceHolder_Cleanup(&refs);
    }
    return s_AndroidInternalFilesPath;
}

int SDL_AndroidGetExternalStorageState()
{
    struct LocalReferenceHolder refs = LocalReferenceHolder_Setup(__FUNCTION__);
    jmethodID mid;
    jclass cls;
    jstring stateString;
    const char *state;
    int stateFlags;

    JNIEnv *env = Android_JNI_GetEnv();
    if (!LocalReferenceHolder_Init(&refs, env)) {
        LocalReferenceHolder_Cleanup(&refs);
        return 0;
    }

    cls = (*env)->FindClass(env, "android/os/Environment");
    mid = (*env)->GetStaticMethodID(env, cls,
            "getExternalStorageState", "()Ljava/lang/String;");
    stateString = (jstring)(*env)->CallStaticObjectMethod(env, cls, mid);

    state = (*env)->GetStringUTFChars(env, stateString, NULL);

    /* Print an info message so people debugging know the storage state */
    __android_log_print(ANDROID_LOG_INFO, "SDL", "external storage state: %s", state);

    if (SDL_strcmp(state, "mounted") == 0) {
        stateFlags = SDL_ANDROID_EXTERNAL_STORAGE_READ |
                     SDL_ANDROID_EXTERNAL_STORAGE_WRITE;
    } else if (SDL_strcmp(state, "mounted_ro") == 0) {
        stateFlags = SDL_ANDROID_EXTERNAL_STORAGE_READ;
    } else {
        stateFlags = 0;
    }
    (*env)->ReleaseStringUTFChars(env, stateString, state);

    LocalReferenceHolder_Cleanup(&refs);
    return stateFlags;
}

const char * SDL_AndroidGetExternalStoragePath()
{
    static char *s_AndroidExternalFilesPath = NULL;

    if (!s_AndroidExternalFilesPath) {
        struct LocalReferenceHolder refs = LocalReferenceHolder_Setup(__FUNCTION__);
        jmethodID mid;
        jobject context;
        jobject fileObject;
        jstring pathString;
        const char *path;

        JNIEnv *env = Android_JNI_GetEnv();
        if (!LocalReferenceHolder_Init(&refs, env)) {
            LocalReferenceHolder_Cleanup(&refs);
            return NULL;
        }

        /* context = SDLActivity.getContext(); */
        mid = (*env)->GetStaticMethodID(env, mActivityClass,
                "getContext","()Landroid/content/Context;");
        context = (*env)->CallStaticObjectMethod(env, mActivityClass, mid);

        /* fileObj = context.getExternalFilesDir(); */
        mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, context),
                "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
        fileObject = (*env)->CallObjectMethod(env, context, mid, NULL);
        if (!fileObject) {
            SDL_SetError("Couldn't get external directory");
            LocalReferenceHolder_Cleanup(&refs);
            return NULL;
        }

        /* path = fileObject.getAbsolutePath(); */
        mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, fileObject),
                "getAbsolutePath", "()Ljava/lang/String;");
        pathString = (jstring)(*env)->CallObjectMethod(env, fileObject, mid);

        path = (*env)->GetStringUTFChars(env, pathString, NULL);
        s_AndroidExternalFilesPath = SDL_strdup(path);
        (*env)->ReleaseStringUTFChars(env, pathString, path);

        LocalReferenceHolder_Cleanup(&refs);
    }
    return s_AndroidExternalFilesPath;
}

#endif /* __ANDROID__ */

/* vi: set ts=4 sw=4 expandtab: */




/******************************************************************************
* $Id$
* Copyright (C) 2003-2007 Kepler Project.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/

/***************************************************************************
*
* $ED
*    This module is the implementation of luajava's dynamic library.
*    In this module lua's functions are exported to be used in java by jni,
*    and also the functions that will be used and exported to lua so that
*    Java Objects' functions can be called.
*
*****************************************************************************/


#include <stdio.h>
#include <stdlib.h>
#include "../../../../LuaJIT/src/lua.h"
#include "../../../../LuaJIT/src/lualib.h"
#include "../../../../LuaJIT/src/lauxlib.h"


/* Constant that is used to index the JNI Environment */
#define LUAJAVAJNIENVTAG      "__JNIEnv"
/* Defines whether the metatable is of a java Object */
#define LUAJAVAOBJECTIND      "__IsJavaObject"
/* Defines the lua State Index Property Name */
#define LUAJAVASTATEINDEX     "LuaJavaStateIndex"
/* Index metamethod name */
#define LUAINDEXMETAMETHODTAG "__index"
/* New index metamethod name */
#define LUANEWINDEXMETAMETHODTAG "__newindex"
/* Garbage collector metamethod name */
#define LUAGCMETAMETHODTAG    "__gc"
/* Call metamethod name */
#define LUACALLMETAMETHODTAG  "__call"
/* Constant that defines where in the metatable should I place the function name */
#define LUAJAVAOBJFUNCCALLED  "__FunctionCalled"



static jclass    throwable_class      = NULL;
static jmethodID get_message_method   = NULL;
static jclass    java_function_class  = NULL;
static jmethodID java_function_method = NULL;
static jclass    luajava_api_class    = NULL;
static jclass    java_lang_class      = NULL;


/***************************************************************************
*
* $FC Function objectIndex
* 
* $ED Description
*    Function to be called by the metamethod __index of the java object
* 
* $EP Function Parameters
*    $P L - lua State
*    $P Stack - Parameters will be received by the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int objectIndex( lua_State * L );


/***************************************************************************
*
* $FC Function objectIndexReturn
* 
* $ED Description
*    Function returned by the metamethod __index of a java Object. It is 
*    the actual function that is going to call the java method.
* 
* $EP Function Parameters
*    $P L - lua State
*    $P Stack - Parameters will be received by the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int objectIndexReturn( lua_State * L );


/***************************************************************************
*
* $FC Function objectNewIndex
* 
* $ED Description
*    Function to be called by the metamethod __newindex of the java object
* 
* $EP Function Parameters
*    $P L - lua State
*    $P Stack - Parameters will be received by the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int objectNewIndex( lua_State * L );


/***************************************************************************
*
* $FC Function classIndex
* 
* $ED Description
*    Function to be called by the metamethod __index of the java class
* 
* $EP Function Parameters
*    $P L - lua State
*    $P Stack - Parameters will be received by the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int classIndex( lua_State * L );


/***************************************************************************
*
* $FC Function arrayIndex
* 
* $ED Description
*    Function to be called by the metamethod __index of a java array
* 
* $EP Function Parameters
*    $P L - lua State
*    $P Stack - Parameters will be received by the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int arrayIndex( lua_State * L );
   

/***************************************************************************
*
* $FC Function arrayNewIndex
* 
* $ED Description
*    Function to be called by the metamethod __newindex of a java array
* 
* $EP Function Parameters
*    $P L - lua State
*    $P Stack - Parameters will be received by the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int arrayNewIndex( lua_State * L );


/***************************************************************************
*
* $FC Function GC
* 
* $ED Description
*    Function to be called by the metamethod __gc of the java object
* 
* $EP Function Parameters
*    $P L - lua State
*    $P Stack - Parameters will be received by the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int gc( lua_State * L );


/***************************************************************************
*
* $FC Function javaBindClass
* 
* $ED Description
*    Implementation of lua function luajava.BindClass
* 
* $EP Function Parameters
*    $P L - lua State
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int javaBindClass( lua_State * L );

/***************************************************************************
*
* $FC Function createProxy
* 
* $ED Description
*    Implementation of lua function luajava.createProxy.
*    Transform a lua table into a java class that implements a list 
*  of interfaces
* 
* $EP Function Parameters
*    $P L - lua State
*    $P Stack - Parameters will be received by the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int createProxy( lua_State * L );

/***************************************************************************
*
* $FC Function javaNew
* 
* $ED Description
*    Implementation of lua function luajava.new
* 
* $EP Function Parameters
*    $P L - lua State
*    $P Stack - Parameters will be received by the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int javaNew( lua_State * L );


/***************************************************************************
*
* $FC Function javaNewInstance
* 
* $ED Description
*    Implementation of lua function luajava.newInstance
* 
* $EP Function Parameters
*    $P L - lua State
*    $P Stack - Parameters will be received by the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int javaNewInstance( lua_State * L );


/***************************************************************************
*
* $FC Function javaLoadLib
* 
* $ED Description
*    Implementation of lua function luajava.loadLib
* 
* $EP Function Parameters
*    $P L - lua State
*    $P Stack - Parameters will be received by the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int javaLoadLib( lua_State * L );


/***************************************************************************
*
* $FC pushJavaObject
* 
* $ED Description
*    Function to create a lua proxy to a java object
* 
* $EP Function Parameters
*    $P L - lua State
*    $P javaObject - Java Object to be pushed on the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int pushJavaObject( lua_State * L , jobject javaObject );


/***************************************************************************
*
* $FC pushJavaArray
* 
* $ED Description
*    Function to create a lua proxy to a java array
* 
* $EP Function Parameters
*    $P L - lua State
*    $P javaObject - Java array to be pushed on the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int pushJavaArray( lua_State * L , jobject javaObject );


/***************************************************************************
*
* $FC pushJavaClass
* 
* $ED Description
*    Function to create a lua proxy to a java class
* 
* $EP Function Parameters
*    $P L - lua State
*    $P javaObject - Java Class to be pushed on the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function
* 
*$. **********************************************************************/

   static int pushJavaClass( lua_State * L , jobject javaObject );


/***************************************************************************
*
* $FC isJavaObject
* 
* $ED Description
*    Returns 1 is given index represents a java object
* 
* $EP Function Parameters
*    $P L - lua State
*    $P idx - index on the stack
* 
* $FV Returned Value
*    int - Boolean.
* 
*$. **********************************************************************/

   static int isJavaObject( lua_State * L , int idx );


/***************************************************************************
*
* $FC getStateFromCPtr
* 
* $ED Description
*    Returns the lua_State from the CPtr Java Object
* 
* $EP Function Parameters
*    $P L - lua State
*    $P cptr - CPtr object
* 
* $FV Returned Value
*    int - Number of values to be returned by the function.
* 
*$. **********************************************************************/

   static lua_State * getStateFromCPtr( JNIEnv * env , jobject cptr );


/***************************************************************************
*
* $FC luaJavaFunctionCall
* 
* $ED Description
*    function called by metamethod __call of instances of JavaFunctionWrapper
* 
* $EP Function Parameters
*    $P L - lua State
*    $P Stack - Parameters will be received by the stack
* 
* $FV Returned Value
*    int - Number of values to be returned by the function.
* 
*$. **********************************************************************/

   static int luaJavaFunctionCall( lua_State * L );


/***************************************************************************
*
* $FC pushJNIEnv
* 
* $ED Description
*    function that pushes the jni environment into the lua state
* 
* $EP Function Parameters
*    $P env - java environment
*    $P L - lua State
* 
* $FV Returned Value
*    void
* 
*$. **********************************************************************/

   static void pushJNIEnv( JNIEnv * env , lua_State * L );


   /***************************************************************************
*
* $FC getEnvFromState
* 
* $ED Description
*    auxiliary function to get the JNIEnv from the lua state
* 
* $EP Function Parameters
*    $P L - lua State
* 
* $FV Returned Value
*    JNIEnv * - JNI environment
* 
*$. **********************************************************************/

   static JNIEnv * getEnvFromState( lua_State * L );
   

/********************* Implementations ***************************/

/***************************************************************************
*
*  Function: objectIndex
*  ****/

int objectIndex( lua_State * L )
{
   lua_Number stateIndex;
   const char * key;
   jmethodID method;
   jint checkField;
   jobject * obj;
   jstring str;
   jthrowable exp;
   JNIEnv * javaEnv;

   /* Gets the luaState index */
   lua_pushstring( L , LUAJAVASTATEINDEX );
   lua_rawget( L , LUA_REGISTRYINDEX );

   if ( !lua_isnumber( L , -1 ) )
   {
      lua_pushstring( L , "Impossible to identify luaState id." );
      lua_error( L );
   }

   stateIndex = lua_tonumber( L , -1 );
   lua_pop( L , 1 );

   if ( !lua_isstring( L , -1 ) )
   {
      lua_pushstring( L , "Invalid object index. Must be string." );
      lua_error( L );
   }

   key = lua_tostring( L , -1 );

   if ( !isJavaObject( L , 1 ) )
   {
      lua_pushstring( L , "Not a valid Java Object." );
      lua_error( L );
   }

   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   obj = ( jobject * ) lua_touserdata( L , 1 );

   method = ( *javaEnv )->GetStaticMethodID( javaEnv , luajava_api_class , "checkField" ,
                                             "(ILjava/lang/Object;Ljava/lang/String;)I" );

   str = ( *javaEnv )->NewStringUTF( javaEnv , key );

   checkField = ( *javaEnv )->CallStaticIntMethod( javaEnv , luajava_api_class , method ,
                                                   (jint)stateIndex , *obj , str );

   exp = ( *javaEnv )->ExceptionOccurred( javaEnv );

   /* Handles exception */
   if ( exp != NULL )
   {
      jobject jstr;
      const char * cStr;
      
      ( *javaEnv )->ExceptionClear( javaEnv );
      jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , get_message_method );

      ( *javaEnv )->DeleteLocalRef( javaEnv , str );

      if ( jstr == NULL )
      {
         jmethodID methodId;

         methodId = ( *javaEnv )->GetMethodID( javaEnv , throwable_class , "toString" , "()Ljava/lang/String;" );
         jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , methodId );
      }

      cStr = ( *javaEnv )->GetStringUTFChars( javaEnv , jstr , NULL );

      lua_pushstring( L , cStr );

      ( *javaEnv )->ReleaseStringUTFChars( javaEnv , jstr, cStr );

      lua_error( L );
   }

   ( *javaEnv )->DeleteLocalRef( javaEnv , str );

   if ( checkField != 0 )
   {
      return checkField;
   }

   lua_getmetatable( L , 1 );

   if ( !lua_istable( L , -1 ) )
   {
      lua_pushstring( L , "Invalid MetaTable." );
      lua_error( L );
   }

   lua_pushstring( L , LUAJAVAOBJFUNCCALLED );
   lua_pushstring( L , key );
   lua_rawset( L , -3 );

   lua_pop( L , 1 );

   lua_pushcfunction( L , &objectIndexReturn );

   return 1;
}


/***************************************************************************
*
*  Function: objectIndexReturn
*  ****/

int objectIndexReturn( lua_State * L )
{
   lua_Number stateIndex;
   jobject * pObject;
   jmethodID method;
   jthrowable exp;
   const char * methodName;
   jint ret;
   jstring str;
   JNIEnv * javaEnv;

   /* Gets the luaState index */
   lua_pushstring( L , LUAJAVASTATEINDEX );
   lua_rawget( L , LUA_REGISTRYINDEX );

   if ( !lua_isnumber( L , -1 ) )
   {
      lua_pushstring( L , "Impossible to identify luaState id." );
      lua_error( L );
   }

   stateIndex = lua_tonumber( L , -1 );
   lua_pop( L , 1 );

   /* Checks if is a valid java object */
   if ( !isJavaObject( L , 1 ) )
   {
      lua_pushstring( L , "Not a valid OO function call." );
      lua_error( L );
   }

   lua_getmetatable( L , 1 );
   if ( lua_type( L , -1 ) == LUA_TNIL )
   {
      lua_pushstring( L , "Not a valid java Object." );
      lua_error( L );
   }

   /* Gets the method Name */
   lua_pushstring( L , LUAJAVAOBJFUNCCALLED );
   lua_rawget( L , -2 );
   if ( lua_type( L , -1 ) == LUA_TNIL )
   {
      lua_pushstring( L , "Not a OO function call." );
      lua_error( L );
   }
   methodName = lua_tostring( L , -1 );

   lua_pop( L , 2 );

   /* Gets the object reference */
   pObject = ( jobject* ) lua_touserdata( L , 1 );

   /* Gets the JNI Environment */
   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   /* Gets method */
   method = ( *javaEnv )->GetStaticMethodID( javaEnv , luajava_api_class , "objectIndex" ,
                                             "(ILjava/lang/Object;Ljava/lang/String;)I" );

   str = ( *javaEnv )->NewStringUTF( javaEnv , methodName );

   ret = ( *javaEnv )->CallStaticIntMethod( javaEnv , luajava_api_class , method , (jint)stateIndex , 
                                            *pObject , str );

   exp = ( *javaEnv )->ExceptionOccurred( javaEnv );

   /* Handles exception */
   if ( exp != NULL )
   {
      jobject jstr;
      const char * cStr;
      
      ( *javaEnv )->ExceptionClear( javaEnv );
      jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , get_message_method );

      ( *javaEnv )->DeleteLocalRef( javaEnv , str );

      if ( jstr == NULL )
      {
         jmethodID methodId;

         methodId = ( *javaEnv )->GetMethodID( javaEnv , throwable_class , "toString" , "()Ljava/lang/String;" );
         jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , methodId );
      }

      cStr = ( *javaEnv )->GetStringUTFChars( javaEnv , jstr , NULL );

      lua_pushstring( L , cStr );

      ( *javaEnv )->ReleaseStringUTFChars( javaEnv , jstr, cStr );

      lua_error( L );
   }

   ( *javaEnv )->DeleteLocalRef( javaEnv , str );

   /* pushes new object into lua stack */
   return ret;
}


/***************************************************************************
*
*  Function: objectNewIndex
*  ****/

int objectNewIndex( lua_State * L  )
{
   lua_Number stateIndex;
   jobject * obj;
   jmethodID method;
   const char * fieldName;
   jstring str;
   jint ret;
   jthrowable exp;
   JNIEnv * javaEnv;

   /* Gets the luaState index */
   lua_pushstring( L , LUAJAVASTATEINDEX );
   lua_rawget( L , LUA_REGISTRYINDEX );

   if ( !lua_isnumber( L , -1 ) )
   {
      lua_pushstring( L , "Impossible to identify luaState id." );
      lua_error( L );
   }

   stateIndex = lua_tonumber( L , -1 );
   lua_pop( L , 1 );

   if ( !isJavaObject( L , 1 ) )
   {
      lua_pushstring( L , "Not a valid java class." );
      lua_error( L );
   }

   /* Gets the field Name */

   if ( !lua_isstring( L , 2 ) )
   {
      lua_pushstring( L , "Not a valid field call." );
      lua_error( L );
   }

   fieldName = lua_tostring( L , 2 );

   /* Gets the object reference */
   obj = ( jobject* ) lua_touserdata( L , 1 );

   /* Gets the JNI Environment */
   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   method = ( *javaEnv )->GetStaticMethodID( javaEnv , luajava_api_class , "objectNewIndex" ,
                                             "(ILjava/lang/Object;Ljava/lang/String;)I" );

   str = ( *javaEnv )->NewStringUTF( javaEnv , fieldName );

   ret = ( *javaEnv )->CallStaticIntMethod( javaEnv , luajava_api_class , method, (jint)stateIndex , 
                                            *obj , str );

   exp = ( *javaEnv )->ExceptionOccurred( javaEnv );

   /* Handles exception */
   if ( exp != NULL )
   {
      jobject jstr;
      const char * cStr;
      
      ( *javaEnv )->ExceptionClear( javaEnv );
      jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , get_message_method );

      ( *javaEnv )->DeleteLocalRef( javaEnv , str );

      if ( jstr == NULL )
      {
         jmethodID methodId;

         methodId = ( *javaEnv )->GetMethodID( javaEnv , throwable_class , "toString" , "()Ljava/lang/String;" );
         jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , methodId );
      }

      cStr = ( *javaEnv )->GetStringUTFChars( javaEnv , jstr , NULL );

      lua_pushstring( L , cStr );

      ( *javaEnv )->ReleaseStringUTFChars( javaEnv , jstr, cStr );

      lua_error( L );
   }

   ( *javaEnv )->DeleteLocalRef( javaEnv , str );


   return ret;
}


/***************************************************************************
*
*  Function: classIndex
*  ****/

int classIndex( lua_State * L )
{
   lua_Number stateIndex;
   jobject * obj;
   jmethodID method;
   const char * fieldName;
   jstring str;
   jint ret;
   jthrowable exp;
   JNIEnv * javaEnv;

   /* Gets the luaState index */
   lua_pushstring( L , LUAJAVASTATEINDEX );
   lua_rawget( L , LUA_REGISTRYINDEX );

   if ( !lua_isnumber( L , -1 ) )
   {
      lua_pushstring( L , "Impossible to identify luaState id." );
      lua_error( L );
   }

   stateIndex = lua_tonumber( L , -1 );
   lua_pop( L , 1 );

   if ( !isJavaObject( L , 1 ) )
   {
      lua_pushstring( L , "Not a valid java class." );
      lua_error( L );
   }

   /* Gets the field Name */

   if ( !lua_isstring( L , 2 ) )
   {
      lua_pushstring( L , "Not a valid field call." );
      lua_error( L );
   }

   fieldName = lua_tostring( L , 2 );

   /* Gets the object reference */
   obj = ( jobject* ) lua_touserdata( L , 1 );

   /* Gets the JNI Environment */
   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   method = ( *javaEnv )->GetStaticMethodID( javaEnv , luajava_api_class , "classIndex" ,
                                             "(ILjava/lang/Class;Ljava/lang/String;)I" );

   str = ( *javaEnv )->NewStringUTF( javaEnv , fieldName );

   /* Return 1 for field, 2 for method or 0 for error */
   ret = ( *javaEnv )->CallStaticIntMethod( javaEnv , luajava_api_class , method, (jint)stateIndex , 
                                            *obj , str );

   exp = ( *javaEnv )->ExceptionOccurred( javaEnv );

   /* Handles exception */
   if ( exp != NULL )
   {
      jobject jstr;
      const char * cStr;
      
      ( *javaEnv )->ExceptionClear( javaEnv );
      jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , get_message_method );

      ( *javaEnv )->DeleteLocalRef( javaEnv , str );

      if ( jstr == NULL )
      {
         jmethodID methodId;

         methodId = ( *javaEnv )->GetMethodID( javaEnv , throwable_class , "toString" , "()Ljava/lang/String;" );
         jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , methodId );
      }

      cStr = ( *javaEnv )->GetStringUTFChars( javaEnv , jstr , NULL );

      lua_pushstring( L , cStr );

      ( *javaEnv )->ReleaseStringUTFChars( javaEnv , jstr, cStr );

      lua_error( L );
   }

   ( *javaEnv )->DeleteLocalRef( javaEnv , str );

   if ( ret == 0 )
   {
      lua_pushstring( L , "Name is not a static field or function." );
      lua_error( L );
   }

   if ( ret == 2 )
   {
      lua_getmetatable( L , 1 );
      lua_pushstring( L , LUAJAVAOBJFUNCCALLED );
      lua_pushstring( L , fieldName );
      lua_rawset( L , -3 );

      lua_pop( L , 1 );

      lua_pushcfunction( L , &objectIndexReturn );

      return 1;
   }

   return ret;
}


/***************************************************************************
*
*  Function: arrayIndex
*  ****/

int arrayIndex( lua_State * L )
{
   lua_Number stateIndex;
   lua_Integer key;
   jmethodID method;
   jint ret;
   jobject * obj;
   jthrowable exp;
   JNIEnv * javaEnv;

    /* Can index as number or string */
   if ( !lua_isnumber( L , -1 ) && !lua_isstring( L , -1 ) )
   {
      lua_pushstring( L , "Invalid object index. Must be integer or string." );
      lua_error( L );
   }

    /* Important! If the index is not a number, behave as normal Java object */
    if ( !lua_isnumber( L , -1 ) )
    {
        return objectIndex( L );
    }

    /* Index is number */

   /* Gets the luaState index */
   lua_pushstring( L , LUAJAVASTATEINDEX );
   lua_rawget( L , LUA_REGISTRYINDEX );

   if ( !lua_isnumber( L , -1 ) )
   {
      lua_pushstring( L , "Impossible to identify luaState id." );
      lua_error( L );
   }

   stateIndex = lua_tonumber( L , -1 );
   lua_pop( L , 1 );

    // Array index
   key = lua_tointeger( L , -1 );

   if ( !isJavaObject( L , 1 ) )
   {
      lua_pushstring( L , "Not a valid Java Object." );
      lua_error( L );
   }

   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   obj = ( jobject * ) lua_touserdata( L , 1 );

   method = ( *javaEnv )->GetStaticMethodID( javaEnv , luajava_api_class , "arrayIndex" ,
                                             "(ILjava/lang/Object;I)I" );

   ret = ( *javaEnv )->CallStaticIntMethod( javaEnv , luajava_api_class , method ,
                                                   (jint)stateIndex , *obj , (jlong)key );

   exp = ( *javaEnv )->ExceptionOccurred( javaEnv );

   /* Handles exception */
   if ( exp != NULL )
   {
      jobject jstr;
      const char * cStr;
      
      ( *javaEnv )->ExceptionClear( javaEnv );
      jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , get_message_method );

      if ( jstr == NULL )
      {
         jmethodID methodId;

         methodId = ( *javaEnv )->GetMethodID( javaEnv , throwable_class , "toString" , "()Ljava/lang/String;" );
         jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , methodId );
      }

      cStr = ( *javaEnv )->GetStringUTFChars( javaEnv , jstr , NULL );

      lua_pushstring( L , cStr );

      ( *javaEnv )->ReleaseStringUTFChars( javaEnv , jstr, cStr );

      lua_error( L );
   }

   return ret;
}


/***************************************************************************
*
*  Function: arrayNewIndex
*  ****/

int arrayNewIndex( lua_State * L )
{
   lua_Number stateIndex;
   jobject * obj;
   jmethodID method;
   lua_Integer key;
   jint ret;
   jthrowable exp;
   JNIEnv * javaEnv;

   /* Gets the luaState index */
   lua_pushstring( L , LUAJAVASTATEINDEX );
   lua_rawget( L , LUA_REGISTRYINDEX );

   if ( !lua_isnumber( L , -1 ) )
   {
      lua_pushstring( L , "Impossible to identify luaState id." );
      lua_error( L );
   }

   stateIndex = lua_tonumber( L , -1 );
   lua_pop( L , 1 );

   if ( !isJavaObject( L , 1 ) )
   {
      lua_pushstring( L , "Not a valid java class." );
      lua_error( L );
   }

   /* Gets the field Name */

   if ( !lua_isnumber( L , 2 ) )
   {
      lua_pushstring( L , "Not a valid array index." );
      lua_error( L );
   }

   key = lua_tointeger( L , 2 );

   /* Gets the object reference */
   obj = ( jobject* ) lua_touserdata( L , 1 );

   /* Gets the JNI Environment */
   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   method = ( *javaEnv )->GetStaticMethodID( javaEnv , luajava_api_class , "arrayNewIndex" ,
                                             "(ILjava/lang/Object;I)I" );

   ret = ( *javaEnv )->CallStaticIntMethod( javaEnv , luajava_api_class , method, (jint)stateIndex , 
                                            *obj , (jint)key );

   exp = ( *javaEnv )->ExceptionOccurred( javaEnv );

   /* Handles exception */
   if ( exp != NULL )
   {
      jobject jstr;
      const char * cStr;
      
      ( *javaEnv )->ExceptionClear( javaEnv );
      jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , get_message_method );

      if ( jstr == NULL )
      {
         jmethodID methodId;

         methodId = ( *javaEnv )->GetMethodID( javaEnv , throwable_class , "toString" , "()Ljava/lang/String;" );
         jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , methodId );
      }

      cStr = ( *javaEnv )->GetStringUTFChars( javaEnv , jstr , NULL );

      lua_pushstring( L , cStr );

      ( *javaEnv )->ReleaseStringUTFChars( javaEnv , jstr, cStr );

      lua_error( L );
   }


   return ret;
}


/***************************************************************************
*
*  Function: gc
*  ****/

int gc( lua_State * L )
{
   jobject * pObj;
   JNIEnv * javaEnv;

   if ( !isJavaObject( L , 1 ) )
   {
      return 0;
   }

   pObj = ( jobject * ) lua_touserdata( L , 1 );

   /* Gets the JNI Environment */
   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   ( *javaEnv )->DeleteGlobalRef( javaEnv , *pObj );

   return 0;
}


/***************************************************************************
*
*  Function: javaBindClass
*  ****/

int javaBindClass( lua_State * L )
{
   int top;
   jmethodID method;
   const char * className;
   jstring javaClassName;
   jobject classInstance;
   jthrowable exp;
   JNIEnv * javaEnv;

   top = lua_gettop( L );

   if ( top != 1 )
   {
      luaL_error( L , "Error. Function javaBindClass received %d arguments, expected 1." , top );
   }

   /* Gets the JNI Environment */
   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   /* get the string parameter */
   if ( !lua_isstring( L , 1 ) )
   {
      lua_pushstring( L , "Invalid parameter type. String expected." );
      lua_error( L );
   }
   className = lua_tostring( L , 1 );

   method = ( *javaEnv )->GetStaticMethodID( javaEnv , java_lang_class , "forName" , 
                                             "(Ljava/lang/String;)Ljava/lang/Class;" );

   javaClassName = ( *javaEnv )->NewStringUTF( javaEnv , className );

   classInstance = ( *javaEnv )->CallStaticObjectMethod( javaEnv , java_lang_class ,
                                                         method , javaClassName );

   exp = ( *javaEnv )->ExceptionOccurred( javaEnv );

   /* Handles exception */
   if ( exp != NULL )
   {
      jobject jstr;
      const char * cStr;
      
      ( *javaEnv )->ExceptionClear( javaEnv );
      jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , get_message_method );

      ( *javaEnv )->DeleteLocalRef( javaEnv , javaClassName );

      if ( jstr == NULL )
      {
         jmethodID methodId;

         methodId = ( *javaEnv )->GetMethodID( javaEnv , throwable_class , "toString" , "()Ljava/lang/String;" );
         jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , methodId );
      }

      cStr = ( *javaEnv )->GetStringUTFChars( javaEnv , jstr , NULL );

      lua_pushstring( L , cStr );

      ( *javaEnv )->ReleaseStringUTFChars( javaEnv , jstr, cStr );

      lua_error( L );
   }

   ( *javaEnv )->DeleteLocalRef( javaEnv , javaClassName );

   /* pushes new object into lua stack */

   return pushJavaClass( L , classInstance );
}


/***************************************************************************
*
*  Function: createProxy
*  ****/
int createProxy( lua_State * L )
{
  jint ret;
  lua_Number stateIndex;
  const char * impl;
  jmethodID method;
  jthrowable exp;
  jstring str;
  JNIEnv * javaEnv;

  if ( lua_gettop( L ) != 2 )
  {
    lua_pushstring( L , "Error. Function createProxy expects 2 arguments." );
    lua_error( L );
  }

  /* Gets the luaState index */
   lua_pushstring( L , LUAJAVASTATEINDEX );
   lua_rawget( L , LUA_REGISTRYINDEX );

   if ( !lua_isnumber( L , -1 ) )
   {
      lua_pushstring( L , "Impossible to identify luaState id." );
      lua_error( L );
   }

   stateIndex = lua_tonumber( L , -1 );
   lua_pop( L , 1 );

   if ( !lua_isstring( L , 1 ) || !lua_istable( L , 2 ) )
   {
      lua_pushstring( L , "Invalid Argument types. Expected (string, table)." );
      lua_error( L );
   }

   /* Gets the JNI Environment */
   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   method = ( *javaEnv )->GetStaticMethodID( javaEnv , luajava_api_class , "createProxyObject" ,
                                             "(ILjava/lang/String;)I" );

   impl = lua_tostring( L , 1 );

   str = ( *javaEnv )->NewStringUTF( javaEnv , impl );

   ret = ( *javaEnv )->CallStaticIntMethod( javaEnv , luajava_api_class , method, (jint)stateIndex , str );
   
   exp = ( *javaEnv )->ExceptionOccurred( javaEnv );

   /* Handles exception */
   if ( exp != NULL )
   {
      jobject jstr;
      const char * cStr;
      
      ( *javaEnv )->ExceptionClear( javaEnv );
      jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , get_message_method );

      ( *javaEnv )->DeleteLocalRef( javaEnv , str );

      if ( jstr == NULL )
      {
         jmethodID methodId;

         methodId = ( *javaEnv )->GetMethodID( javaEnv , throwable_class , "toString" , "()Ljava/lang/String;" );
         jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , methodId );
      }

      cStr = ( *javaEnv )->GetStringUTFChars( javaEnv , jstr , NULL );

      lua_pushstring( L , cStr );

      ( *javaEnv )->ReleaseStringUTFChars( javaEnv , jstr, cStr );

      lua_error( L );
   }

   ( *javaEnv )->DeleteLocalRef( javaEnv , str );

   return ret;
}

/***************************************************************************
*
*  Function: javaNew
*  ****/

int javaNew( lua_State * L )
{
   int top;
   jint ret;
   jclass clazz;
   jmethodID method;
   jobject classInstance ;
   jthrowable exp;
   jobject * userData;
   lua_Number stateIndex;
   JNIEnv * javaEnv;

   top = lua_gettop( L );

   if ( top == 0 )
   {
      lua_pushstring( L , "Error. Invalid number of parameters." );
      lua_error( L );
   }

   /* Gets the luaState index */
   lua_pushstring( L , LUAJAVASTATEINDEX );
   lua_rawget( L , LUA_REGISTRYINDEX );

   if ( !lua_isnumber( L , -1 ) )
   {
      lua_pushstring( L , "Impossible to identify luaState id." );
      lua_error( L );
   }

   stateIndex = lua_tonumber( L , -1 );
   lua_pop( L , 1 );

   /* Gets the java Class reference */
   if ( !isJavaObject( L , 1 ) )
   {
      lua_pushstring( L , "Argument not a valid Java Class." );
      lua_error( L );
   }

   /* Gets the JNI Environment */
   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   clazz = ( *javaEnv )->FindClass( javaEnv , "java/lang/Class" );

   userData = ( jobject * ) lua_touserdata( L , 1 );

   classInstance = ( jobject ) *userData;

   if ( ( *javaEnv )->IsInstanceOf( javaEnv , classInstance , clazz ) == JNI_FALSE )
   {
      lua_pushstring( L , "Argument not a valid Java Class." );
      lua_error( L );
   }

   method = ( *javaEnv )->GetStaticMethodID( javaEnv , luajava_api_class , "javaNew" , 
                                             "(ILjava/lang/Class;)I" );

   if ( clazz == NULL || method == NULL )
   {
      lua_pushstring( L , "Invalid method org.keplerproject.luajava.LuaJavaAPI.javaNew." );
      lua_error( L );
   }

   ret = ( *javaEnv )->CallStaticIntMethod( javaEnv , clazz , method , (jint)stateIndex , classInstance );

   exp = ( *javaEnv )->ExceptionOccurred( javaEnv );

   /* Handles exception */
   if ( exp != NULL )
   {
      jobject jstr;
      const char * str;
      
      ( *javaEnv )->ExceptionClear( javaEnv );
      jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , get_message_method );

      if ( jstr == NULL )
      {
         jmethodID methodId;

         methodId = ( *javaEnv )->GetMethodID( javaEnv , throwable_class , "toString" , "()Ljava/lang/String;" );
         jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , methodId );
      }

      str = ( *javaEnv )->GetStringUTFChars( javaEnv , jstr , NULL );

      lua_pushstring( L , str );

      ( *javaEnv )->ReleaseStringUTFChars( javaEnv , jstr, str );

      lua_error( L );
   }
  return ret;
}


/***************************************************************************
*
*  Function: javaNewInstance
*  ****/

int javaNewInstance( lua_State * L )
{
   jint ret;
   jmethodID method;
   const char * className;
   jstring javaClassName;
   jthrowable exp;
   lua_Number stateIndex;
   JNIEnv * javaEnv;

   /* Gets the luaState index */
   lua_pushstring( L , LUAJAVASTATEINDEX );
   lua_rawget( L , LUA_REGISTRYINDEX );

   if ( !lua_isnumber( L , -1 ) )
   {
      lua_pushstring( L , "Impossible to identify luaState id." );
      lua_error( L );
   }

   stateIndex = lua_tonumber( L , -1 );
   lua_pop( L , 1 );

   /* get the string parameter */
   if ( !lua_isstring( L , 1 ) )
   {
      lua_pushstring( L , "Invalid parameter type. String expected as first parameter." );
      lua_error( L );
   }

   className = lua_tostring( L , 1 );

   /* Gets the JNI Environment */
   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   method = ( *javaEnv )->GetStaticMethodID( javaEnv , luajava_api_class , "javaNewInstance" ,
                                             "(ILjava/lang/String;)I" );

   javaClassName = ( *javaEnv )->NewStringUTF( javaEnv , className );
   
   ret = ( *javaEnv )->CallStaticIntMethod( javaEnv , luajava_api_class , method, (jint)stateIndex , 
                                            javaClassName );

   exp = ( *javaEnv )->ExceptionOccurred( javaEnv );

   /* Handles exception */
   if ( exp != NULL )
   {
      jobject jstr;
      const char * str;
      
      ( *javaEnv )->ExceptionClear( javaEnv );
      jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , get_message_method );

      ( *javaEnv )->DeleteLocalRef( javaEnv , javaClassName );

      if ( jstr == NULL )
      {
         jmethodID methodId;

         methodId = ( *javaEnv )->GetMethodID( javaEnv , throwable_class , "toString" , "()Ljava/lang/String;" );
         jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , methodId );
      }

      str = ( *javaEnv )->GetStringUTFChars( javaEnv , jstr , NULL );

      lua_pushstring( L , str );

      ( *javaEnv )->ReleaseStringUTFChars( javaEnv , jstr, str );

      lua_error( L );
   }

   ( *javaEnv )->DeleteLocalRef( javaEnv , javaClassName );

   return ret;
}


/***************************************************************************
*
*  Function: javaLoadLib
*  ****/

int javaLoadLib( lua_State * L )
{
   jint ret;
   int top;
   const char * className, * methodName;
   lua_Number stateIndex;
   jmethodID method;
   jthrowable exp;
   jstring javaClassName , javaMethodName;
   JNIEnv * javaEnv;

   top = lua_gettop( L );

   if ( top != 2 )
   {
      lua_pushstring( L , "Error. Invalid number of parameters." );
      lua_error( L );
   }

   /* Gets the luaState index */
   lua_pushstring( L , LUAJAVASTATEINDEX );
   lua_rawget( L , LUA_REGISTRYINDEX );

   if ( !lua_isnumber( L , -1 ) )
   {
      lua_pushstring( L , "Impossible to identify luaState id." );
      lua_error( L );
   }

   stateIndex = lua_tonumber( L , -1 );
   lua_pop( L , 1 );


   if ( !lua_isstring( L , 1 ) || !lua_isstring( L , 2 ) )
   {
      lua_pushstring( L , "Invalid parameter. Strings expected." );
      lua_error( L );
   }

   className  = lua_tostring( L , 1 );
   methodName = lua_tostring( L , 2 );

   /* Gets the JNI Environment */
   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   method = ( *javaEnv )->GetStaticMethodID( javaEnv , luajava_api_class , "javaLoadLib" ,
                                             "(ILjava/lang/String;Ljava/lang/String;)I" );

   javaClassName  = ( *javaEnv )->NewStringUTF( javaEnv , className );
   javaMethodName = ( *javaEnv )->NewStringUTF( javaEnv , methodName );
   
   ret = ( *javaEnv )->CallStaticIntMethod( javaEnv , luajava_api_class , method, (jint)stateIndex , 
                                            javaClassName , javaMethodName );

   exp = ( *javaEnv )->ExceptionOccurred( javaEnv );

   /* Handles exception */
   if ( exp != NULL )
   {
      jobject jstr;
      const char * str;
      
      ( *javaEnv )->ExceptionClear( javaEnv );
      jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , get_message_method );

      ( *javaEnv )->DeleteLocalRef( javaEnv , javaClassName );
      ( *javaEnv )->DeleteLocalRef( javaEnv , javaMethodName );

      if ( jstr == NULL )
      {
         jmethodID methodId;

         methodId = ( *javaEnv )->GetMethodID( javaEnv , throwable_class , "toString" , "()Ljava/lang/String;" );
         jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , methodId );
      }

      str = ( *javaEnv )->GetStringUTFChars( javaEnv , jstr , NULL );

      lua_pushstring( L , str );

      ( *javaEnv )->ReleaseStringUTFChars( javaEnv , jstr, str );

      lua_error( L );
   }

   ( *javaEnv )->DeleteLocalRef( javaEnv , javaClassName );
   ( *javaEnv )->DeleteLocalRef( javaEnv , javaMethodName );

   return ret;
}


/***************************************************************************
*
*  Function: pushJavaClass
*  ****/

int pushJavaClass( lua_State * L , jobject javaObject )
{
   jobject * userData , globalRef;

   /* Gets the JNI Environment */
   JNIEnv * javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   globalRef = ( *javaEnv )->NewGlobalRef( javaEnv , javaObject );

   userData = ( jobject * ) lua_newuserdata( L , sizeof( jobject ) );
   *userData = globalRef;

   /* Creates metatable */
   lua_newtable( L );

   /* pushes the __index metamethod */
   lua_pushstring( L , LUAINDEXMETAMETHODTAG );
   lua_pushcfunction( L , &classIndex );
   lua_rawset( L , -3 );

   /* pushes the __newindex metamethod */
   lua_pushstring( L , LUANEWINDEXMETAMETHODTAG );
   lua_pushcfunction( L , &objectNewIndex );
   lua_rawset( L , -3 );

   /* pushes the __gc metamethod */
   lua_pushstring( L , LUAGCMETAMETHODTAG );
   lua_pushcfunction( L , &gc );
   lua_rawset( L , -3 );

   /* Is Java Object boolean */
   lua_pushstring( L , LUAJAVAOBJECTIND );
   lua_pushboolean( L , 1 );
   lua_rawset( L , -3 );

   if ( lua_setmetatable( L , -2 ) == 0 )
   {
        ( *javaEnv )->DeleteGlobalRef( javaEnv , globalRef );
      lua_pushstring( L , "Cannot create proxy to java class." );
      lua_error( L );
   }

   return 1;
}


/***************************************************************************
*
*  Function: pushJavaObject
*  ****/

int pushJavaObject( lua_State * L , jobject javaObject )
{
   jobject * userData , globalRef;

   /* Gets the JNI Environment */
   JNIEnv * javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   globalRef = ( *javaEnv )->NewGlobalRef( javaEnv , javaObject );

   userData = ( jobject * ) lua_newuserdata( L , sizeof( jobject ) );
   *userData = globalRef;

   /* Creates metatable */
   lua_newtable( L );

   /* pushes the __index metamethod */
   lua_pushstring( L , LUAINDEXMETAMETHODTAG );
   lua_pushcfunction( L , &objectIndex );
   lua_rawset( L , -3 );

   /* pushes the __newindex metamethod */
   lua_pushstring( L , LUANEWINDEXMETAMETHODTAG );
   lua_pushcfunction( L , &objectNewIndex );
   lua_rawset( L , -3 );

   /* pushes the __gc metamethod */
   lua_pushstring( L , LUAGCMETAMETHODTAG );
   lua_pushcfunction( L , &gc );
   lua_rawset( L , -3 );

   /* Is Java Object boolean */
   lua_pushstring( L , LUAJAVAOBJECTIND );
   lua_pushboolean( L , 1 );
   lua_rawset( L , -3 );

   if ( lua_setmetatable( L , -2 ) == 0 )
   {
        ( *javaEnv )->DeleteGlobalRef( javaEnv , globalRef );
      lua_pushstring( L , "Cannot create proxy to java object." );
      lua_error( L );
   }

   return 1;
}


/***************************************************************************
*
*  Function: pushJavaObject
*  ****/

int pushJavaArray( lua_State * L , jobject javaObject )
{
   jobject * userData , globalRef;

   /* Gets the JNI Environment */
   JNIEnv * javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   globalRef = ( *javaEnv )->NewGlobalRef( javaEnv , javaObject );

   userData = ( jobject * ) lua_newuserdata( L , sizeof( jobject ) );
   *userData = globalRef;

   /* Creates metatable */
   lua_newtable( L );

   /* pushes the __index metamethod */
   lua_pushstring( L , LUAINDEXMETAMETHODTAG );
   lua_pushcfunction( L , &arrayIndex );
   lua_rawset( L , -3 );

   /* pushes the __newindex metamethod */
   lua_pushstring( L , LUANEWINDEXMETAMETHODTAG );
   lua_pushcfunction( L , &arrayNewIndex );
   lua_rawset( L , -3 );

   /* pushes the __gc metamethod */
   lua_pushstring( L , LUAGCMETAMETHODTAG );
   lua_pushcfunction( L , &gc );
   lua_rawset( L , -3 );

   /* Is Java Object boolean */
   lua_pushstring( L , LUAJAVAOBJECTIND );
   lua_pushboolean( L , 1 );
   lua_rawset( L , -3 );

   if ( lua_setmetatable( L , -2 ) == 0 )
   {
        ( *javaEnv )->DeleteGlobalRef( javaEnv , globalRef );
      lua_pushstring( L , "Cannot create proxy to java object." );
      lua_error( L );
   }

   return 1;
}


/***************************************************************************
*
*  Function: isJavaObject
*  ****/

int isJavaObject( lua_State * L , int idx )
{
   if ( !lua_isuserdata( L , idx ) )
      return 0;

   if ( lua_getmetatable( L , idx ) == 0 )
      return 0;

   lua_pushstring( L , LUAJAVAOBJECTIND );
   lua_rawget( L , -2 );

   if (lua_isnil( L, -1 ))
   {
      lua_pop( L , 2 );
      return 0;
   }
   lua_pop( L , 2 );
   return 1;
}


extern lua_State* Android_GetLuaState();
/***************************************************************************
*
*  Function: getStateFromCPtr
*  ****/

lua_State * getStateFromCPtr( JNIEnv * env , jobject cptr )
{
   // lua_State * L;

   // jclass classPtr       = ( *env )->GetObjectClass( env , cptr );
   // jfieldID CPtr_peer_ID = ( *env )->GetFieldID( env , classPtr , "peer" , "J" );
   // jbyte * peer          = ( jbyte * ) ( *env )->GetLongField( env , cptr , CPtr_peer_ID );

   // L = ( lua_State * ) peer;

    lua_State * L = Android_GetLuaState();
    
   pushJNIEnv( env ,  L );

   return L;
}


/***************************************************************************
*
*  Function: luaJavaFunctionCall
*  ****/

int luaJavaFunctionCall( lua_State * L )
{
   jobject * obj;
   jthrowable exp;
   int ret;
   JNIEnv * javaEnv;
   
   if ( !isJavaObject( L , 1 ) )
   {
      lua_pushstring( L , "Not a java Function." );
      lua_error( L );
   }

   obj = lua_touserdata( L , 1 );

   /* Gets the JNI Environment */
   javaEnv = getEnvFromState( L );
   if ( javaEnv == NULL )
   {
      lua_pushstring( L , "Invalid JNI Environment." );
      lua_error( L );
   }

   /* the Object must be an instance of the JavaFunction class */
   if ( ( *javaEnv )->IsInstanceOf( javaEnv , *obj , java_function_class ) ==
        JNI_FALSE )
   {
      fprintf( stderr , "Called Java object is not a JavaFunction\n");
      return 0;
   }

   ret = ( *javaEnv )->CallIntMethod( javaEnv , *obj , java_function_method );

   exp = ( *javaEnv )->ExceptionOccurred( javaEnv );

   /* Handles exception */
   if ( exp != NULL )
   {
      jobject jstr;
      const char * str;
      
      ( *javaEnv )->ExceptionClear( javaEnv );
      jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , get_message_method );

      if ( jstr == NULL )
      {
         jmethodID methodId;

         methodId = ( *javaEnv )->GetMethodID( javaEnv , throwable_class , "toString" , "()Ljava/lang/String;" );
         jstr = ( *javaEnv )->CallObjectMethod( javaEnv , exp , methodId );
      }

      str = ( *javaEnv )->GetStringUTFChars( javaEnv , jstr , NULL );

      lua_pushstring( L , str );

      ( *javaEnv )->ReleaseStringUTFChars( javaEnv , jstr, str );

      lua_error( L );
   }
   return ret;
}


/***************************************************************************
*
*  Function: luaJavaFunctionCall
*  ****/

JNIEnv * getEnvFromState( lua_State * L )
{
   JNIEnv ** udEnv;

   lua_pushstring( L , LUAJAVAJNIENVTAG );
   lua_rawget( L , LUA_REGISTRYINDEX );

   if ( !lua_isuserdata( L , -1 ) )
   {
      lua_pop( L , 1 );
      return NULL;
   }

   udEnv = ( JNIEnv ** ) lua_touserdata( L , -1 );

   lua_pop( L , 1 );

   return * udEnv;
}


/***************************************************************************
*
*  Function: pushJNIEnv
*  ****/

void pushJNIEnv( JNIEnv * env , lua_State * L )
{
   JNIEnv ** udEnv;

   lua_pushstring( L , LUAJAVAJNIENVTAG );
   lua_rawget( L , LUA_REGISTRYINDEX );

   if ( !lua_isnil( L , -1 ) )
   {
      udEnv = ( JNIEnv ** ) lua_touserdata( L , -1 );
      *udEnv = env;
      lua_pop( L , 1 );
   }
   else
   {
      lua_pop( L , 1 );
      udEnv = ( JNIEnv ** ) lua_newuserdata( L , sizeof( JNIEnv * ) );
      *udEnv = env;

      lua_pushstring( L , LUAJAVAJNIENVTAG );
      lua_insert( L , -2 );
      lua_rawset( L , LUA_REGISTRYINDEX );
   }
}

/*
** Assumes the table is on top of the stack.
*/
static void set_info (lua_State *L) {
    lua_pushliteral (L, "_COPYRIGHT");
    lua_pushliteral (L, "Copyright (C) 2003-2007 Kepler Project");
    lua_settable (L, -3);
    lua_pushliteral (L, "_DESCRIPTION");
    lua_pushliteral (L, "LuaJava is a script tool for Java");
    lua_settable (L, -3);
    lua_pushliteral (L, "_NAME");
    lua_pushliteral (L, "LuaJava");
    lua_settable (L, -3);
    lua_pushliteral (L, "_VERSION");
    lua_pushliteral (L, "1.1");
    lua_settable (L, -3);
}

/**************************** JNI FUNCTIONS ****************************/

/************************************************************************
*   JNI Called function
*      LuaJava API Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState_luajava_1open
  ( JNIEnv * env , jobject jobj , jobject cptr , jint stateId )
{
  lua_State* L;

  jclass tempClass;

  L = getStateFromCPtr( env , cptr );

  lua_pushstring( L , LUAJAVASTATEINDEX );
  lua_pushnumber( L , (lua_Number)stateId );
  lua_settable( L , LUA_REGISTRYINDEX );


  lua_newtable( L );

  lua_setglobal( L , "luajava" );

  lua_getglobal( L , "luajava" );
  
  set_info( L);
  
  lua_pushstring( L , "bindClass" );
  lua_pushcfunction( L , &javaBindClass );
  lua_settable( L , -3 );

  lua_pushstring( L , "new" );
  lua_pushcfunction( L , &javaNew );
  lua_settable( L , -3 );

  lua_pushstring( L , "newInstance" );
  lua_pushcfunction( L , &javaNewInstance );
  lua_settable( L , -3 );

  lua_pushstring( L , "loadLib" );
  lua_pushcfunction( L , &javaLoadLib );
  lua_settable( L , -3 );

  lua_pushstring( L , "createProxy" );
  lua_pushcfunction( L , &createProxy );
  lua_settable( L , -3 );

  lua_pop( L , 1 );

  if ( luajava_api_class == NULL )
  {
    tempClass = ( *env )->FindClass( env , "org/keplerproject/luajava/LuaJavaAPI" );

    if ( tempClass == NULL )
    {
      fprintf( stderr , "Could not find LuaJavaAPI class\n" );
      exit( 1 );
    }

    if ( ( luajava_api_class = ( *env )->NewGlobalRef( env , tempClass ) ) == NULL )
    {
      fprintf( stderr , "Could not bind to LuaJavaAPI class\n" );
      exit( 1 );
    }
  }

  if ( java_function_class == NULL )
  {
    tempClass = ( *env )->FindClass( env , "org/keplerproject/luajava/JavaFunction" );

    if ( tempClass == NULL )
    {
      fprintf( stderr , "Could not find JavaFunction interface\n" );
      exit( 1 );
    }

    if ( ( java_function_class = ( *env )->NewGlobalRef( env , tempClass ) ) == NULL )
    {
      fprintf( stderr , "Could not bind to JavaFunction interface\n" );
      exit( 1 );
    }
  }

  if ( java_function_method == NULL )
  {
    java_function_method = ( *env )->GetMethodID( env , java_function_class , "execute" , "()I");
    if ( !java_function_method )
    {
      fprintf( stderr , "Could not find <execute> method in JavaFunction\n" );
      exit( 1 );
    }
  }

  if ( throwable_class == NULL )
  {
    tempClass = ( *env )->FindClass( env , "java/lang/Throwable" );

    if ( tempClass == NULL )
    {
      fprintf( stderr , "Error. Couldn't bind java class java.lang.Throwable\n" );
      exit( 1 );
    }

    throwable_class = ( *env )->NewGlobalRef( env , tempClass );

    if ( throwable_class == NULL )
    {
      fprintf( stderr , "Error. Couldn't bind java class java.lang.Throwable\n" );
      exit( 1 );
    }
  }

  if ( get_message_method == NULL )
  {
    get_message_method = ( *env )->GetMethodID( env , throwable_class , "getMessage" ,
                                                "()Ljava/lang/String;" );

    if ( get_message_method == NULL )
    {
      fprintf(stderr, "Could not find <getMessage> method in java.lang.Throwable\n");
      exit(1);
    }
  }

  if ( java_lang_class == NULL )
  {
    tempClass = ( *env )->FindClass( env , "java/lang/Class" );

    if ( tempClass == NULL )
    {
      fprintf( stderr , "Error. Coundn't bind java class java.lang.Class\n" );
      exit( 1 );
    }

    java_lang_class = ( *env )->NewGlobalRef( env , tempClass );

    if ( java_lang_class == NULL )
    {
      fprintf( stderr , "Error. Couldn't bind java class java.lang.Throwable\n" );
      exit( 1 );
    }
  }

  pushJNIEnv( env , L );
}

/************************************************************************
*   JNI Called function
*      LuaJava API Function
************************************************************************/

JNIEXPORT jobject JNICALL Java_org_keplerproject_luajava_LuaState__1getObjectFromUserdata
  (JNIEnv * env , jobject jobj , jobject cptr , jint index )
{
   /* Get luastate */
   lua_State * L = getStateFromCPtr( env , cptr );
   jobject *   obj;

   if ( !isJavaObject( L , index ) )
   {
      ( *env )->ThrowNew( env , ( *env )->FindClass( env , "java/lang/Exception" ) ,
                          "Index is not a java object" );
      return NULL;
   }

   obj = ( jobject * ) lua_touserdata( L , index );

   return *obj;
}


/************************************************************************
*   JNI Called function
*      LuaJava API Function
************************************************************************/

JNIEXPORT jboolean JNICALL Java_org_keplerproject_luajava_LuaState__1isObject
  (JNIEnv * env , jobject jobj , jobject cptr , jint index )
{
   /* Get luastate */
   lua_State * L = getStateFromCPtr( env , cptr );

   return (isJavaObject( L , index ) ? JNI_TRUE : JNI_FALSE );
}


/************************************************************************
*   JNI Called function
*      LuaJava API Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1pushJavaObject
  (JNIEnv * env , jobject jobj , jobject cptr , jobject obj )
{
   /* Get luastate */
   lua_State* L = getStateFromCPtr( env , cptr );

   pushJavaObject( L , obj );
}


/************************************************************************
*   JNI Called function
*      LuaJava API Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1pushJavaArray
  (JNIEnv * env , jobject jobj , jobject cptr , jobject obj )
{
   /* Get luastate */
   lua_State* L = getStateFromCPtr( env , cptr );

    pushJavaArray( L , obj );
}


/************************************************************************
*   JNI Called function
*      LuaJava API Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1pushJavaFunction
  (JNIEnv * env , jobject jobj , jobject cptr , jobject obj )
{
   /* Get luastate */
   lua_State* L = getStateFromCPtr( env , cptr );

   jobject * userData , globalRef;

   globalRef = ( *env )->NewGlobalRef( env , obj );

   userData = ( jobject * ) lua_newuserdata( L , sizeof( jobject ) );
   *userData = globalRef;

   /* Creates metatable */
   lua_newtable( L );

   /* pushes the __index metamethod */
   lua_pushstring( L , LUACALLMETAMETHODTAG );
   lua_pushcfunction( L , &luaJavaFunctionCall );
   lua_rawset( L , -3 );

   /* pusher the __gc metamethod */
   lua_pushstring( L , LUAGCMETAMETHODTAG );
   lua_pushcfunction( L , &gc );
   lua_rawset( L , -3 );

   lua_pushstring( L , LUAJAVAOBJECTIND );
   lua_pushboolean( L , 1 );
   lua_rawset( L , -3 );

   if ( lua_setmetatable( L , -2 ) == 0 )
   {
      ( *env )->ThrowNew( env , ( *env )->FindClass( env , "org/keplerproject/luajava/LuaException" ) ,
                          "Index is not a java object" );
   }
}


/************************************************************************
*   JNI Called function
*      LuaJava API Function
************************************************************************/

JNIEXPORT jboolean JNICALL Java_org_keplerproject_luajava_LuaState__1isJavaFunction
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   /* Get luastate */
   lua_State* L = getStateFromCPtr( env , cptr );
   jobject * obj;

   if ( !isJavaObject( L , idx ) )
   {
      return JNI_FALSE;
   }

   obj = ( jobject * ) lua_touserdata( L , idx );

   return ( *env )->IsInstanceOf( env , *obj , java_function_class );

}


/*********************** LUA API FUNCTIONS ******************************/

/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jobject JNICALL Java_org_keplerproject_luajava_LuaState__1open
  (JNIEnv * env , jobject jobj)
{
   lua_State * L = lua_open();

   jobject obj;
   jclass tempClass;

   tempClass = ( *env )->FindClass( env , "org/keplerproject/luajava/CPtr" );
    
   obj = ( *env )->AllocObject( env , tempClass );
   if ( obj )
   {
      ( *env )->SetLongField( env , obj , ( *env )->GetFieldID( env , tempClass , "peer", "J" ) , ( jlong ) L );
   }
   return obj;

}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1openBase
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   //luaopen_base( L );
   lua_pushcfunction( L , luaopen_base );
   lua_pushstring( L , "" );
   lua_call(L , 1 , 0 );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1openTable
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   //luaopen_table( L );
   lua_pushcfunction( L , luaopen_table );
   lua_pushstring( L , LUA_TABLIBNAME );
   lua_call(L , 1 , 0 );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1openIo
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   //luaopen_io( L );
   lua_pushcfunction( L , luaopen_io );
   lua_pushstring( L , LUA_IOLIBNAME );
   lua_call(L , 1 , 0 );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1openOs
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   //luaopen_os( L );
   lua_pushcfunction( L , luaopen_os );
   lua_pushstring( L , LUA_OSLIBNAME );
   lua_call(L , 1 , 0 );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1openString
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   //luaopen_string( L );
   lua_pushcfunction( L , luaopen_string );
   lua_pushstring( L , LUA_STRLIBNAME );
   lua_call(L , 1 , 0 );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1openMath
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   //luaopen_math( L );
   lua_pushcfunction( L , luaopen_math );
   lua_pushstring( L , LUA_MATHLIBNAME );
   lua_call(L , 1 , 0 );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1openDebug
  (JNIEnv * env, jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   //luaopen_debug( L );
   lua_pushcfunction( L , luaopen_debug );
   lua_pushstring( L , LUA_DBLIBNAME );
   lua_call(L , 1 , 0 );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1openPackage
  (JNIEnv * env, jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   //luaopen_package( L );
   lua_pushcfunction( L , luaopen_package );
   lua_pushstring( L , LUA_LOADLIBNAME );
   lua_call(L , 1 , 0 );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1openLibs
  (JNIEnv * env, jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   luaL_openlibs( L );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1close
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_close( L );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jobject JNICALL Java_org_keplerproject_luajava_LuaState__1newthread
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );
   lua_State * newThread;
   
   jobject obj;
   jclass tempClass;
    
   newThread = lua_newthread( L );

   tempClass = ( *env )->FindClass( env , "org/keplerproject/luajava/CPtr" );
   obj = ( *env )->AllocObject( env , tempClass );
   if ( obj )
   {
      ( *env )->SetLongField( env , obj , ( *env )->GetFieldID( env , tempClass ,
                                                        "peer" , "J" ), ( jlong ) L );
   }

   return obj;

}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1getTop
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_gettop( L );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1setTop
  (JNIEnv * env , jobject jobj , jobject cptr , jint top)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_settop( L , ( int ) top );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1pushValue
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_pushvalue( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1remove
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_remove( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1insert
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_insert( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1replace
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_replace( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1checkStack
  (JNIEnv * env , jobject jobj , jobject cptr , jint sz)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_checkstack( L , ( int ) sz );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1xmove
  (JNIEnv * env , jobject jobj , jobject from , jobject to , jint n)
{
   lua_State * fr = getStateFromCPtr( env , from );
   lua_State * t  = getStateFromCPtr( env , to );

   lua_xmove( fr , t , ( int ) n );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1isNumber
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_isnumber( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1isString
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_isstring( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1isFunction
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_isfunction( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1isCFunction
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_iscfunction( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1isUserdata
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_isuserdata( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1isTable
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_istable( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1isBoolean
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_isboolean( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1isNil
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_isnil( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1isNone
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_isnone( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1isNoneOrNil
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_isnoneornil( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1type
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_type( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jstring JNICALL Java_org_keplerproject_luajava_LuaState__1typeName
  (JNIEnv * env , jobject jobj , jobject cptr , jint tp)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   const char * name = lua_typename( L , tp );

   return ( *env )->NewStringUTF( env , name );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1equal
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx1 , jint idx2)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_equal( L , idx1 , idx2 );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1rawequal
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx1 , jint idx2)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_rawequal( L , idx1 , idx2 );
}

  
/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1lessthan
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx1 , jint idx2)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_lessthan( L , idx1 ,idx2 );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jdouble JNICALL Java_org_keplerproject_luajava_LuaState__1toNumber
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jdouble ) lua_tonumber( L , idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1toInteger
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_tointeger( L , idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1toBoolean
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_toboolean( L , idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jstring JNICALL Java_org_keplerproject_luajava_LuaState__1toString
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   const char * str = lua_tostring( L , idx );

   return ( *env )->NewStringUTF( env , str );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1strlen
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_strlen( L , idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1objlen
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_objlen( L , idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jobject JNICALL Java_org_keplerproject_luajava_LuaState__1toThread
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L , * thr;

   jobject obj;
   jclass tempClass;

   L = getStateFromCPtr( env , cptr );

   thr = lua_tothread( L , ( int ) idx );

   tempClass = ( *env )->FindClass( env , "org/keplerproject/luajava/CPtr" );
    
   obj = ( *env )->AllocObject( env , tempClass );
   if ( obj )
   {
      ( *env )->SetLongField( env , obj , ( *env )->GetFieldID( env , tempClass , "peer", "J" ) , ( jlong ) thr );
   }
   return obj;

}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1pushNil
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_pushnil( L );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1pushNumber
  (JNIEnv * env , jobject jobj , jobject cptr , jdouble number)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_pushnumber( L , ( lua_Number ) number );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1pushInteger
  (JNIEnv * env , jobject jobj , jobject cptr , jint number)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_pushinteger( L, ( lua_Integer ) number );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1pushString__Lorg_keplerproject_luajava_CPtr_2Ljava_lang_String_2
  (JNIEnv * env , jobject jobj , jobject cptr , jstring str)
{
   lua_State * L = getStateFromCPtr( env , cptr );
   const char * uniStr;

   uniStr =  ( *env )->GetStringUTFChars( env , str , NULL );

   if ( uniStr == NULL )
      return;

   lua_pushstring( L , uniStr );
   
   ( *env )->ReleaseStringUTFChars( env , str , uniStr );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1pushString__Lorg_keplerproject_luajava_CPtr_2_3BI
  (JNIEnv * env , jobject jobj , jobject cptr , jbyteArray bytes , jint n)
{
   lua_State * L = getStateFromCPtr( env , cptr );
   char * cBytes;
   
   cBytes = ( char * ) ( *env )->GetByteArrayElements( env , bytes, NULL );
   
   lua_pushlstring( L , cBytes , n );
   
   ( *env )->ReleaseByteArrayElements( env , bytes , cBytes , 0 );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1pushBoolean
  (JNIEnv * env , jobject jobj , jobject cptr , jint jbool)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_pushboolean( L , ( int ) jbool );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1getTable
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_gettable( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1getField
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx , jstring k)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   const char * uniStr;
   uniStr =  ( *env )->GetStringUTFChars( env , k , NULL );

   lua_getfield( L , ( int ) idx , uniStr );
   
   ( *env )->ReleaseStringUTFChars( env , k , uniStr );
}

/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1rawGet
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_rawget( L , (int)idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1rawGetI
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx, jint n)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_rawgeti( L , idx , n );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1createTable
  (JNIEnv * env , jobject jobj , jobject cptr , jint narr , jint nrec)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_createtable( L , ( int ) narr , ( int ) nrec );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1newTable
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_newtable( L );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1getMetaTable
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return lua_getmetatable( L , idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1getFEnv
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_getfenv( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1setTable
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_settable( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1setField
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx , jstring k)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   const char * uniStr;
   uniStr =  ( *env )->GetStringUTFChars( env , k , NULL );

   lua_setfield( L , ( int ) idx , uniStr );
   
   ( *env )->ReleaseStringUTFChars( env , k , uniStr );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1rawSet
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_rawset( L , (int)idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1rawSetI
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx, jint n)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_rawseti( L , idx , n );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1setMetaTable
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return lua_setmetatable( L , idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1setFEnv
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return lua_setfenv( L , idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1call
  (JNIEnv * env , jobject jobj , jobject cptr , jint nArgs , jint nResults)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_call( L , nArgs , nResults );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1pcall
  (JNIEnv * env , jobject jobj , jobject cptr , jint nArgs , jint nResults , jint errFunc)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_pcall( L , nArgs , nResults , errFunc );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1yield
  (JNIEnv * env , jobject jobj , jobject cptr , jint nResults)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_yield( L , nResults );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1resume
  (JNIEnv * env , jobject jobj , jobject cptr , jint nArgs)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_resume( L , nArgs );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1status
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_status( L );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1gc
  (JNIEnv * env , jobject jobj , jobject cptr , jint what , jint data)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_gc( L , what , data );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1getGcCount
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_getgccount( L );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1next
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_next( L , idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1error
  (JNIEnv * env , jobject jobj , jobject cptr)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) lua_error( L );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1concat
  (JNIEnv * env , jobject jobj , jobject cptr , jint n)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_concat( L , n );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1pop
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   lua_pop( L , ( int ) idx );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1setGlobal
  (JNIEnv * env , jobject jobj , jobject cptr , jstring name)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   const char * str = ( *env )->GetStringUTFChars( env , name, NULL );

   lua_setglobal( L , str );

   ( *env )->ReleaseStringUTFChars( env , name , str );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1getGlobal
  (JNIEnv * env , jobject jobj , jobject cptr , jstring name)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   const char * str = ( *env )->GetStringUTFChars( env , name, NULL );

   lua_getglobal( L , str );

   ( *env )->ReleaseStringUTFChars( env , name , str );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1LdoFile
  (JNIEnv * env , jobject jobj , jobject cptr , jstring fileName)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   const char * file = ( *env )->GetStringUTFChars( env , fileName, NULL );

   int ret;

   ret = luaL_dofile( L , file );

   ( *env )->ReleaseStringUTFChars( env , fileName , file );

   return ( jint ) ret;
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1LdoString
  (JNIEnv * env , jobject jobj , jobject cptr , jstring str)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   const char * utfStr = ( * env )->GetStringUTFChars( env , str , NULL );

   int ret;

   ret = luaL_dostring( L , utfStr );

   return ( jint ) ret;
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1LgetMetaField
  (JNIEnv * env , jobject jobj , jobject cptr , jint obj , jstring e)
{
   lua_State * L    = getStateFromCPtr( env , cptr );
   const char * str = ( *env )->GetStringUTFChars( env , e , NULL );
   int ret;

   ret = luaL_getmetafield( L , ( int ) obj , str );

   ( *env )->ReleaseStringUTFChars( env , e , str );

   return ( jint ) ret;
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1LcallMeta
  (JNIEnv * env , jobject jobj , jobject cptr , jint obj , jstring e)
{
   lua_State * L    = getStateFromCPtr( env , cptr );
   const char * str = ( *env )->GetStringUTFChars( env , e , NULL );
   int ret;

   ret = luaL_callmeta( L , ( int ) obj, str );

   ( *env )->ReleaseStringUTFChars( env , e , str );

   return ( jint ) ret;
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1Ltyperror
  (JNIEnv * env , jobject jobj , jobject cptr , jint nArg , jstring tName)
{
   lua_State * L     = getStateFromCPtr( env , cptr );
   const char * name = ( *env )->GetStringUTFChars( env , tName , NULL );
   int ret;

   ret = luaL_typerror( L , ( int ) nArg , name );

   ( *env )->ReleaseStringUTFChars( env , tName , name );

   return ( jint ) ret;
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1LargError
  (JNIEnv * env , jobject jobj , jobject cptr , jint numArg , jstring extraMsg)
{
   lua_State * L    = getStateFromCPtr( env , cptr );
   const char * msg = ( *env )->GetStringUTFChars( env , extraMsg , NULL );
   int ret;

   ret = luaL_argerror( L , ( int ) numArg , msg );

   ( *env )->ReleaseStringUTFChars( env , extraMsg , msg );

   return ( jint ) ret;;
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jstring JNICALL Java_org_keplerproject_luajava_LuaState__1LcheckString
  (JNIEnv * env , jobject jobj , jobject cptr , jint numArg)
{
   lua_State * L = getStateFromCPtr( env , cptr );
   const char * res;

   res = luaL_checkstring( L , ( int ) numArg );

   return ( *env )->NewStringUTF( env , res );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jstring JNICALL Java_org_keplerproject_luajava_LuaState__1LoptString
  (JNIEnv * env , jobject jobj , jobject cptr , jint numArg , jstring def)
{
   lua_State * L  = getStateFromCPtr( env , cptr );
   const char * d = ( *env )->GetStringUTFChars( env , def , NULL );
   const char * res;
   jstring ret;

   res = luaL_optstring( L , ( int ) numArg , d );

   ret = ( *env )->NewStringUTF( env , res );

   ( *env )->ReleaseStringUTFChars( env , def , d );

   return ret;
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jdouble JNICALL Java_org_keplerproject_luajava_LuaState__1LcheckNumber
  (JNIEnv * env , jobject jobj , jobject cptr , jint numArg)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jdouble ) luaL_checknumber( L , ( int ) numArg );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jdouble JNICALL Java_org_keplerproject_luajava_LuaState__1LoptNumber
  (JNIEnv * env , jobject jobj , jobject cptr , jint numArg , jdouble def)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jdouble ) luaL_optnumber( L , ( int ) numArg , ( lua_Number ) def );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1LcheckInteger
  (JNIEnv * env , jobject jobj , jobject cptr , jint numArg)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) luaL_checkinteger( L , ( int ) numArg );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1LoptInteger
  (JNIEnv * env , jobject jobj , jobject cptr , jint numArg , jint def)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) luaL_optinteger( L , ( int ) numArg , ( lua_Integer ) def );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1LcheckStack
  (JNIEnv * env , jobject jobj , jobject cptr , jint sz , jstring msg)
{
   lua_State * L  = getStateFromCPtr( env , cptr );
   const char * m = ( *env )->GetStringUTFChars( env , msg , NULL );

   luaL_checkstack( L , ( int ) sz , m );

   ( *env )->ReleaseStringUTFChars( env , msg , m );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1LcheckType
  (JNIEnv * env , jobject jobj , jobject cptr , jint nArg , jint t)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   luaL_checktype( L , ( int ) nArg , ( int ) t );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1LcheckAny
  (JNIEnv * env , jobject jobj , jobject cptr , jint nArg)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   luaL_checkany( L , ( int ) nArg );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1LnewMetatable
  (JNIEnv * env , jobject jobj , jobject cptr , jstring tName)
{
   lua_State * L     = getStateFromCPtr( env , cptr );
   const char * name = ( *env )->GetStringUTFChars( env , tName , NULL );
   int ret;

   ret = luaL_newmetatable( L , name );

   ( *env )->ReleaseStringUTFChars( env , tName , name );

   return ( jint ) ret;;
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1LgetMetatable
  (JNIEnv * env , jobject jobj , jobject cptr , jstring tName)
{
   lua_State * L     = getStateFromCPtr( env , cptr );
   const char * name = ( *env )->GetStringUTFChars( env , tName , NULL );

   luaL_getmetatable( L , name );

   ( *env )->ReleaseStringUTFChars( env , tName , name );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1Lwhere
  (JNIEnv * env , jobject jobj , jobject cptr , jint lvl)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   luaL_where( L , ( int ) lvl );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1Lref
  (JNIEnv * env , jobject jobj , jobject cptr , jint t)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) luaL_ref( L , ( int ) t );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1LunRef
  (JNIEnv * env , jobject jobj , jobject cptr , jint t , jint ref)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   luaL_unref( L , ( int ) t , ( int ) ref );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1LgetN
  (JNIEnv * env , jobject jobj , jobject cptr , jint t)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   return ( jint ) luaL_getn( L , ( int ) t );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT void JNICALL Java_org_keplerproject_luajava_LuaState__1LsetN
  (JNIEnv * env , jobject jobj , jobject cptr , jint t , jint n)
{
   lua_State * L = getStateFromCPtr( env , cptr );

   luaL_setn( L , ( int ) t , ( int ) n );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1LloadFile
  (JNIEnv * env , jobject jobj , jobject cptr , jstring fileName)
{
   lua_State * L   = getStateFromCPtr( env , cptr );
   const char * fn = ( *env )->GetStringUTFChars( env , fileName , NULL );
   int ret;

   ret = luaL_loadfile( L , fn );

   ( *env )->ReleaseStringUTFChars( env , fileName , fn );

   return ( jint ) ret;
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1LloadBuffer
  (JNIEnv * env , jobject jobj , jobject cptr , jbyteArray buff , jlong sz , jstring n)
{
   lua_State * L = getStateFromCPtr( env , cptr );
   jbyte * cBuff = ( *env )->GetByteArrayElements( env , buff, NULL );
   const char * name = ( * env )->GetStringUTFChars( env , n , NULL );
   int ret;

   ret = luaL_loadbuffer( L , ( const char * ) cBuff, ( int ) sz, name );

   ( *env )->ReleaseStringUTFChars( env , n , name );

   ( *env )->ReleaseByteArrayElements( env , buff , cBuff , 0 );

   return ( jint ) ret;
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jint JNICALL Java_org_keplerproject_luajava_LuaState__1LloadString
  (JNIEnv * env , jobject jobj , jobject cptr , jstring str)
{
   lua_State * L   = getStateFromCPtr( env , cptr );
   const char * fn = ( *env )->GetStringUTFChars( env , str , NULL );
   int ret;

   ret = luaL_loadstring( L , fn );

   ( *env )->ReleaseStringUTFChars( env , str , fn );

   return ( jint ) ret;
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jstring JNICALL Java_org_keplerproject_luajava_LuaState__1Lgsub
  (JNIEnv * env , jobject jobj , jobject cptr , jstring s , jstring p , jstring r)
{
   lua_State * L   = getStateFromCPtr( env , cptr );
   const char * utS = ( *env )->GetStringUTFChars( env , s , NULL );
   const char * utP = ( *env )->GetStringUTFChars( env , p , NULL );
   const char * utR = ( *env )->GetStringUTFChars( env , r , NULL );

   const char * sub = luaL_gsub( L , utS , utP , utR );

   ( *env )->ReleaseStringUTFChars( env , s , utS );
   ( *env )->ReleaseStringUTFChars( env , p , utP );
   ( *env )->ReleaseStringUTFChars( env , r , utR );

   return ( *env )->NewStringUTF( env , sub );
}


/************************************************************************
*   JNI Called function
*      Lua Exported Function
************************************************************************/

JNIEXPORT jstring JNICALL Java_org_keplerproject_luajava_LuaState__1LfindTable
  (JNIEnv * env , jobject jobj , jobject cptr , jint idx , jstring fname , jint szhint)
{
   lua_State * L   = getStateFromCPtr( env , cptr );
   const char * name = ( *env )->GetStringUTFChars( env , fname , NULL );

   const char * sub = luaL_findtable( L , ( int ) idx , name , ( int ) szhint );

   ( *env )->ReleaseStringUTFChars( env , fname , name );

   return ( *env )->NewStringUTF( env , sub );
}

