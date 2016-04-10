#include "App.h"
#include "core/VTimer.h"
#include <android/keycodes.h>
#include <math.h>
#include <jni.h>
#include <sstream>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>

#include <3rdparty/stb/stb_image_write.h>

#include "api/VEglDriver.h"
#include "android/JniUtils.h"
#include "android/VOsBuild.h"

#include "VAlgorithm.h"
#include "BitmapFont.h"
#include "DebugLines.h"
#include "EyePostRender.h"
#include "GazeCursor.h"
#include "GazeCursorLocal.h"		// necessary to instantiate the gaze cursor
#include "GlTexture.h"
#include "GuiSys.h"
#include "GuiSysLocal.h"		// necessary to instantiate the gui system
#include "ModelView.h"
#include "SurfaceTexture.h"

#include "TypesafeNumber.h"
#include "VBasicmath.h"
#include "VolumePopup.h"
#include "api/VDevice.h"
#include "api/VFrameSmooth.h"
#include "api/VKernel.h"
#include "VrLocale.h"
#include "VRMenuMgr.h"
#include "VUserSettings.h"

#include "VApkFile.h"
#include "VJson.h"
#include "VLog.h"
#include "VMainActivity.h"
#include "VThread.h"
#include "VStandardPath.h"
#include "VColor.h"
#include "VScene.h"

//#define TEST_TIMEWARP_WATCHDOG
#define EGL_PROTECTED_CONTENT_EXT 0x32c0

NV_NAMESPACE_BEGIN

static const char * activityClassName = "com/vrseen/nervgear/VrActivity";

// some parameters from the intent can be empty strings, which cannot be represented as empty strings for sscanf
// so we encode them as EMPTY_INTENT_STR.
// Because the message queue handling uses sscanf() to parse the message, the JSON text is
// always placed at the end of the message because it can contain spaces while the package
// name and URI cannot. The handler will use sscanf() to parse the first two strings, then
// assume the JSON text is everything immediately following the space after the URI string.
static const char * EMPTY_INTENT_STR = "<EMPTY>";

static int buttonMappings[] = {
    96, 	// BUTTON_A
    97,		// BUTTON_B
    99, 	// BUTTON_X
    100,	// BUTTON_Y
    108, 	// BUTTON_START
    4,		// BUTTON_BACK
    109, 	// BUTTON_SELECT
    82, 	// BUTTON_MENU
    103, 	// BUTTON_RIGHT_TRIGGER
    102, 	// BUTTON_LEFT_TRIGGER
    19,		// BUTTON_DPAD_UP
    20,		// BUTTON_DPAD_DOWN
    21,		// BUTTON_DPAD_LEFT
    22,		// BUTTON_DPAD_RIGHT
    200,	// BUTTON_LSTICK_UP
    201,	// BUTTON_LSTICK_DOWN
    202,	// BUTTON_LSTICK_LEFT
    203,	// BUTTON_LSTICK_RIGHT
    204,	// BUTTON_RSTICK_UP
    205,	// BUTTON_RSTICK_DOWN
    206,	// BUTTON_RSTICK_LEFT
    207,	// BUTTON_RSTICK_RIGHT
    9999,	// touch is handled separately

    -1
};

static V3Vectf ViewOrigin(const VR4Matrixf & view)
{
    return V3Vectf(view.M[0][3], view.M[1][3], view.M[2][3]);
}

static V3Vectf ViewForward(const VR4Matrixf & view)
{
    return V3Vectf(-view.M[0][2], -view.M[1][2], -view.M[2][2]);
}

// Always make the panel upright, even if the head was tilted when created
static VR4Matrixf PanelMatrix(const VR4Matrixf & lastViewMatrix, const float popupDistance,
        const float popupScale, const int width, const int height)
{
    // TODO: this won't be valid until a frame has been rendered
    const VR4Matrixf invView = lastViewMatrix.Inverted();
    const V3Vectf forward = ViewForward(invView);
    const V3Vectf levelforward = V3Vectf(forward.x, 0.0f, forward.z).Normalized();
    // TODO: check degenerate case
    const V3Vectf up(0.0f, 1.0f, 0.0f);
    const V3Vectf right = levelforward.Cross(up);

    const V3Vectf center = ViewOrigin(invView) + levelforward * popupDistance;
    const float xScale = (float)width / 768.0f * popupScale;
    const float yScale = (float)height / 768.0f * popupScale;
    const VR4Matrixf panelMatrix = VR4Matrixf(
            xScale * right.x, yScale * up.x, forward.x, center.x,
            xScale * right.y, yScale * up.y, forward.y, center.y,
            xScale * right.z, yScale * up.z, forward.z, center.z,
            0, 0, 0, 1);

//	vInfo("PanelMatrix center:" << center.xcenter.ycenter.z);
//	LogMatrix("PanelMatrix", panelMatrix);

    return panelMatrix;
}

extern void DebugMenuBounds(void * appPtr, const char * cmd);
extern void DebugMenuHierarchy(void * appPtr, const char * cmd);
extern void DebugMenuPoses(void * appPtr, const char * cmd);

static VEyeBuffer::EyeParms DefaultVrParmsForRenderer(const VEglDriver & glOperation)
{
    VEyeBuffer::EyeParms vrParms;

    vrParms.resolution = 1024;
    vrParms.multisamples = (glOperation.m_gpuType == VEglDriver::GPU_TYPE_ADRENO_330) ? 2 : 4;
    vrParms.colorFormat = VColor::COLOR_8888;
    vrParms.commonParameterDepth = VEyeBuffer::CommonParameter::DEPTHFORMAT_DEPTH_24;

    return vrParms;
}

static bool ChromaticAberrationCorrection(const VEglDriver & glOperation)
{
    return (glOperation.m_gpuType & VEglDriver::GPU_TYPE_ADRENO) != 0 && (glOperation.m_gpuType >= VEglDriver::GPU_TYPE_ADRENO_420);
}



struct App::Private
{
    App *self;
    // Primary apps will exit(0) when they get an onDestroy() so we
    // never leave any cpu-sucking process running, but the platformUI
    // needs to just return to the primary activity.
    volatile bool	vrThreadSynced;
    volatile bool	createdSurface;
    volatile bool	readyToExit;		// start exit procedure

    // Most calls in from java should communicate through this.
    VEventLoop	eventLoop;

    // Egl context and surface for rendering
    VEglDriver  m_vrGlStatus;

    // Handles creating, destroying, and re-configuring the buffers
    // for drawing the eye views, which might be in different texture
    // configurations for CPU warping, etc.
    VEyeBuffer *	eyeTargets;

    GLuint			loadingIconTexId;

    JavaVM *		javaVM;

    JNIEnv *		uiJni;			// for use by the Java UI thread
    JNIEnv *		vrJni;			// for use by the VR thread

    jclass			vrActivityClass;		// must be looked up from main thread or FindClass() will fail

    jmethodID		createVrToastMethodId;
    jmethodID		clearVrToastsMethodId;
    jmethodID		playSoundPoolSoundMethodId;

    VString			launchIntentURI;			// URI app was launched with
    VString			launchIntentJSON;			// extra JSON data app was launched with
    VString			launchIntentFromPackage;	// package that sent us the launch intent

    VString			packageCodePath;	// path to apk to open as zip and load resources

    bool			paused;				// set/cleared by onPause / onResume

    float 			popupDistance;
    float 			popupScale;


    // Every application gets a basic dialog surface.
    SurfaceTexture * dialogTexture;

    // Current joypad state, without pressed / released calculation
    VrInput			joypad;

    // drawing parameters
    int				dialogWidth;
    int				dialogHeight;

    // Dialogs will be oriented base down in the view when they
    // were generated.

    VR4Matrixf		lastViewMatrix;

    ANativeWindow * nativeWindow;
    EGLSurface 		windowSurface;

    bool			drawCalibrationLines;	// currently toggled by right trigger
    bool			calibrationLinesDrawn;	// after draw, go to static time warp test
    bool			showVignette;			// render the vignette

    bool			framebufferIsSrgb;			// requires KHR_gl_colorspace
    bool			framebufferIsProtected;		// requires GPU trust zone extension

    // Only render a single eye view, which will get warped for both
    // screen eyes.
    bool			renderMonoMode;

    VrFrame			lastVrFrame;

    VEyeBuffer::EyeParms		vrParms;



    VGlShader		untexturedMvpProgram;
    VGlShader		untexturedScreenSpaceProgram;
    VGlShader		overlayScreenFadeMaskProgram;
    VGlShader		overlayScreenDirectProgram;

    VGlGeometry		unitCubeLines;		// 12 lines that outline a 0 to 1 unit cube, intended to be scaled to cover bounds.
    VGlGeometry		unitSquare;			// -1 to 1 in x and Y, 0 to 1 in texcoords
    VGlGeometry		fadedScreenMaskSquare;// faded screen mask for overlay rendering

    EyePostRender	eyeDecorations;

    ovrSensorState	sensorForNextWarp;

    VThread *renderThread;
    int				vrThreadTid;		// linux tid

    bool			showVolumePopup;	// true to show volume popup when volume changes

    VrViewParms		viewParms;

    float 			touchpadTimer;
    V2Vectf		touchOrigin;
    float 			lastTouchpadTime;
    bool 			lastTouchDown;
    int 			touchState;

    bool			enableDebugOptions;	// enable debug key-commands for development testing

    long long 		recenterYawFrameStart;	// Enables reorient before sensor data is read.  Allows apps to reorient without having invalid orientation information for that frame.

    // Manages sound assets
     VSoundManager	soundManager;

    OvrGuiSys *         guiSys;
    OvrGazeCursor *     gazeCursor;
    BitmapFont *        defaultFont;
    BitmapFontSurface * worldFontSurface;
    BitmapFontSurface * menuFontSurface;
    OvrVRMenuMgr *      vrMenuMgr;
    OvrVolumePopup *	volumePopup;
    OvrDebugLines *     debugLines;
    KeyState backKeyState;
    VStandardPath *storagePaths;

    GlTexture errorTexture;
    int errorTextureSize;
    double errorMessageEndTime;

    jobject javaObject;
    VMainActivity *appInterface;

    VMainActivity *activity;
    VKernel*        kernel;
    VScene *scene;

    Private(App *self)
        : self(self)
        , vrThreadSynced(false)
        , createdSurface(false)
        , readyToExit(false)
        , eventLoop(100)
        , eyeTargets(nullptr)
        , loadingIconTexId(0)
        , javaVM(VrLibJavaVM)
        , uiJni(nullptr)
        , vrJni(nullptr)
        , vrActivityClass(nullptr)
        , createVrToastMethodId(nullptr)
        , clearVrToastsMethodId(nullptr)
        , playSoundPoolSoundMethodId(nullptr)
        , paused(true)
        , popupDistance(2.0f)
        , popupScale(1.0f)
        , dialogWidth(0)
        , dialogHeight(0)
        , nativeWindow(nullptr)
        , windowSurface(EGL_NO_SURFACE)
        , drawCalibrationLines(false)
        , calibrationLinesDrawn(false)
        , showVignette(true)
        , framebufferIsSrgb(false)
        , framebufferIsProtected(false)
        , renderMonoMode(false)
        , vrThreadTid(0)
        , showVolumePopup(true)
        , touchpadTimer(0.0f)
        , lastTouchpadTime(0.0f)
        , lastTouchDown(false)
        , touchState(0)
        , enableDebugOptions(false)
        , recenterYawFrameStart(0)
        , guiSys(nullptr)
        , gazeCursor(nullptr)
        , defaultFont(nullptr)
        , worldFontSurface(nullptr)
        , menuFontSurface(nullptr)
        , vrMenuMgr(nullptr)
        , volumePopup(nullptr)
        , debugLines(nullptr)
        , backKeyState(0.25f, 0.75f)
        , storagePaths(nullptr)
        , errorTextureSize(0)
        , errorMessageEndTime(-1.0)
        , javaObject(nullptr)
        , appInterface(nullptr)
        , activity(nullptr)
        , scene(new VScene)
    {
    }

    ~Private()
    {
        delete scene;
    }

    void initFonts()
    {
        defaultFont = BitmapFont::Create();

        VString fontName;
        VrLocale::GetString(vrJni, javaObject, "@string/font_name", "efigs.fnt", fontName);
        fontName.prepend("res/raw/");
        if (!defaultFont->Load(packageCodePath, fontName))
        {
            // reset the locale to english and try to load the font again
            jmethodID setDefaultLocaleId = vrJni->GetMethodID(vrActivityClass, "setDefaultLocale", "()V");
            if (setDefaultLocaleId != nullptr)
            {
                vInfo("AppLocal::Init CallObjectMethod");
                vrJni->CallVoidMethod(javaObject, setDefaultLocaleId);
                if (vrJni->ExceptionOccurred())
                {
                    vrJni->ExceptionClear();
                    vWarn("Exception occurred in setDefaultLocale");
                }
                // re-get the font name for the new locale
                VrLocale::GetString(vrJni, javaObject, "@string/font_name", "efigs.fnt", fontName);
                fontName.prepend("res/raw/");
                // try to load the font
                if (!defaultFont->Load(packageCodePath, fontName))
                {
                    vFatal("Failed to load font for default locale!");
                }
            }
        }

        worldFontSurface = BitmapFontSurface::Create();
        menuFontSurface = BitmapFontSurface::Create();

        worldFontSurface->Init(8192);
        menuFontSurface->Init(8192);
    }

    void shutdownFonts()
    {
        BitmapFont::Free(defaultFont);
        BitmapFontSurface::Free(worldFontSurface);
        BitmapFontSurface::Free(menuFontSurface);
    }

    // Error checks and exits on failure
    jmethodID GetMethodID(const char * name, const char *signature) const
    {
        jmethodID mid = uiJni->GetMethodID(vrActivityClass, name, signature);
        if (!mid) {
            vFatal("couldn't get" << name);
        }
        return mid;
    }

    void pause()
    {
        appInterface->onPause();

        kernel->exit();
    }

    void resume()
    {
        vInfo("OVRTimer AppLocal::Resume");

        // always reload the dev config on a resume
        JniUtils::LoadDevConfig(true);


        // Make sure the window surface is current, which it won't be
        // if we were previously in async mode
        // (Not needed now?)
        if (eglMakeCurrent(m_vrGlStatus.m_display, windowSurface, windowSurface, m_vrGlStatus.m_context) == EGL_FALSE)
        {
            vFatal("eglMakeCurrent failed:" << m_vrGlStatus.getEglErrorString());
        }

        // Allow the app to override
        appInterface->configureVrMode(kernel);

        // Clear cursor trails
        gazeCursor->HideCursorForFrames(10);

        // Start up TimeWarp and the various performance options
        kernel->run();

        appInterface->onResume();
    }

    void initGlObjects()
    {
        vrParms = DefaultVrParmsForRenderer(m_vrGlStatus);



          kernel->setSmoothProgram(ChromaticAberrationCorrection(m_vrGlStatus) ? VK_DEFAULT_CB : VK_DEFAULT);
        m_vrGlStatus.logExtensions();

        self->panel.externalTextureProgram2.initShader( VGlShader::getAdditionalVertexShaderSource(), VGlShader::getAdditionalFragmentShaderSource() );
        untexturedMvpProgram.initShader( VGlShader::getUntextureMvpVertexShaderSource(),VGlShader::getUntexturedFragmentShaderSource()  );
        untexturedScreenSpaceProgram.initShader( VGlShader::getUniformColorVertexShaderSource(), VGlShader::getUntexturedFragmentShaderSource() );
        overlayScreenFadeMaskProgram.initShader(VGlShader::getUntextureInverseColorVertexShaderSource(),VGlShader::getUntexturedFragmentShaderSource() );
        overlayScreenDirectProgram.initShader(VGlShader::getSingleTextureVertexShaderSource(),VGlShader::getSingleTextureFragmentShaderSource() );


        self->panel.panelGeometry.createPlaneQuadGrid( 32, 16 );
        unitSquare.createPlaneQuadGrid(1, 1 );
        unitCubeLines.createUnitCubeGrid();


        eyeDecorations.Init();
    }

    void shutdownGlObjects()
    {
        self->panel.externalTextureProgram2.destroy();
        untexturedMvpProgram.destroy();
        untexturedScreenSpaceProgram.destroy();
        overlayScreenFadeMaskProgram.destroy();
        overlayScreenDirectProgram.destroy();

        self->panel.panelGeometry.destroy();
        unitSquare.destroy();
        unitCubeLines.destroy();
        fadedScreenMaskSquare.destroy();

        eyeDecorations.Shutdown();
    }

    void interpretTouchpad(VrInput &input)
    {
        // 1) Down -> Up w/ Motion = Slide
        // 2) Down -> Up w/out Motion -> Timeout = Single Tap
        // 3) Down -> Up w/out Motion -> Down -> Timeout = Nothing
        // 4) Down -> Up w/out Motion -> Down -> Up = Double Tap
        static const float timer_finger_down = 0.3f;
        static const float timer_finger_up = 0.3f;
        static const float min_swipe_distance = 100.0f;

        float currentTime = VTimer::Seconds();
        float deltaTime = currentTime - lastTouchpadTime;
        lastTouchpadTime = currentTime;
        touchpadTimer = touchpadTimer + deltaTime;

        bool down = false, up = false;
        bool currentTouchDown = input.buttonState & BUTTON_TOUCH;

        if (currentTouchDown && !lastTouchDown)
        {
            //CreateToast("DOWN");
            down = true;
            touchOrigin = input.touch;
        }

        if (!currentTouchDown && lastTouchDown)
        {
            //CreateToast("UP");
            up = true;
        }

        lastTouchDown = currentTouchDown;

        input.touchRelative = input.touch - touchOrigin;
        float touchMagnitude = input.touchRelative.Length();
        input.swipeFraction = touchMagnitude / min_swipe_distance;

        switch (touchState)
        {
        case 0:
            //CreateToast("0 - %f", touchpadTimer);
            if (down)
            {
                touchState = 1;
                touchpadTimer = 0.0f;
            }
            break;
        case 1:
            //CreateToast("1 - %f", touchpadTimer);
            //CreateToast("1 - %f", touchMagnitude);
            if (touchMagnitude >= min_swipe_distance)
            {
                int dir = 0;
                if (fabs(input.touchRelative[0]) > fabs(input.touchRelative[1]))
                {
                    if (input.touchRelative[0] < 0)
                    {
                        //CreateToast("SWIPE FORWARD");
                        dir = BUTTON_SWIPE_FORWARD | BUTTON_TOUCH_WAS_SWIPE;
                    }
                    else
                    {
                        //CreateToast("SWIPE BACK");
                        dir = BUTTON_SWIPE_BACK | BUTTON_TOUCH_WAS_SWIPE;
                    }
                }
                else
                {
                    if (input.touchRelative[1] > 0)
                    {
                        //CreateToast("SWIPE DOWN");
                        dir = BUTTON_SWIPE_DOWN | BUTTON_TOUCH_WAS_SWIPE;
                    }
                    else
                    {
                        //CreateToast("SWIPE UP");
                        dir = BUTTON_SWIPE_UP | BUTTON_TOUCH_WAS_SWIPE;
                    }
                }
                input.buttonPressed |= dir;
                input.buttonReleased |= dir & ~BUTTON_TOUCH_WAS_SWIPE;
                input.buttonState |= dir;
                touchState = 0;
                touchpadTimer = 0.0f;
            }
            else if (up)
            {
                if (touchpadTimer < timer_finger_down)
                {
                    touchState = 2;
                    touchpadTimer = 0.0f;
                }
                else
                {
                    //CreateToast("SINGLE TOUCH");
                    input.buttonPressed |= BUTTON_TOUCH_SINGLE;
                    input.buttonReleased |= BUTTON_TOUCH_SINGLE;
                    input.buttonState |= BUTTON_TOUCH_SINGLE;
                    touchState = 0;
                    touchpadTimer = 0.0f;
                }
            }
            break;
        case 2:
            //CreateToast("2 - %f", touchpadTimer);
            if (touchpadTimer >= timer_finger_up)
            {
                //CreateToast("SINGLE TOUCH");
                input.buttonPressed |= BUTTON_TOUCH_SINGLE;
                input.buttonReleased |= BUTTON_TOUCH_SINGLE;
                input.buttonState |= BUTTON_TOUCH_SINGLE;
                touchState = 0;
                touchpadTimer = 0.0f;
            }
            else if (down)
            {
                touchState = 3;
                touchpadTimer = 0.0f;
            }
            break;
        case 3:
            //CreateToast("3 - %f", touchpadTimer);
            if (touchpadTimer >= timer_finger_down)
            {
                touchState = 0;
                touchpadTimer = 0.0f;
            }
            else if (up)
            {
                //CreateToast("DOUBLE TOUCH");
                input.buttonPressed |= BUTTON_TOUCH_DOUBLE;
                input.buttonReleased |= BUTTON_TOUCH_DOUBLE;
                input.buttonState |= BUTTON_TOUCH_DOUBLE;
                touchState = 0;
                touchpadTimer = 0.0f;
            }
            break;
        }
    }

    void frameworkButtonProcessing(const VrInput &input)
    {
        // Toggle calibration lines
        bool const rightTrigger = input.buttonState & BUTTON_RIGHT_TRIGGER;
        bool const leftTrigger = input.buttonState & BUTTON_LEFT_TRIGGER;
        if (leftTrigger && rightTrigger && (input.buttonPressed & BUTTON_START) != 0)
        {
            time_t rawTime;
            time(&rawTime);
            struct tm * timeInfo = localtime(&rawTime);
            char timeStr[128];
            strftime(timeStr, sizeof(timeStr), "%H:%M:%S", timeInfo);
            vInfo("QAEvent " << timeStr << " (" << VTimer::Seconds() << ") - QA event occurred");
        }

        // Display tweak testing, only when holding right trigger
        if (enableDebugOptions && rightTrigger)
        {
            if (input.buttonPressed & BUTTON_DPAD_RIGHT)
            {
                jclass vmDebugClass = vrJni->FindClass("dalvik/system/VMDebug");
                jmethodID dumpId = vrJni->GetStaticMethodID(vmDebugClass, "dumpReferenceTables", "()V");
                vrJni->CallStaticVoidMethod(vmDebugClass, dumpId);
                vrJni->DeleteLocalRef(vmDebugClass);
            }

            if (input.buttonPressed & BUTTON_Y)
            {	// display current scheduler state and clock rates
                //const char * str = ovr_CreateSchedulingReport(OvrMobile);
                //CreateToast("%s", str);
            }

            if (input.buttonPressed & BUTTON_B)
            {
                if (kernel->m_smoothOptions & VK_USE_S)
                {
                    kernel->m_smoothOptions &= ~VK_USE_S;
                    self->createToast("eye warp");
                }
                else
                {
                    kernel->m_smoothOptions |= VK_USE_S;
                    self->createToast("slice warp");
                }
            }

            if (kernel->m_smoothOptions & VK_USE_S)
            {
                extern float calibrateFovScale;

                if (input.buttonPressed & BUTTON_DPAD_LEFT)
                {
                    kernel->m_preScheduleSeconds -= 0.001f;
                    self->createToast("Schedule: %f",  kernel->m_preScheduleSeconds);
                }
                if (input.buttonPressed & BUTTON_DPAD_RIGHT)
                {
                    kernel->m_preScheduleSeconds += 0.001f;
                    self->createToast("Schedule: %f",  kernel->m_preScheduleSeconds);
                }
                if (input.buttonPressed & BUTTON_DPAD_UP)
                {
                    calibrateFovScale -= 0.01f;
                    self->createToast("calibrateFovScale: %f", calibrateFovScale);
                    pause();
                    resume();
                }
                if (input.buttonPressed & BUTTON_DPAD_DOWN)
                {
                    calibrateFovScale += 0.01f;
                    self->createToast("calibrateFovScale: %f", calibrateFovScale);
                    pause();
                    resume();
                }
            }
        }
    }

    void command(const VEvent &event)
    {
        // Always include the space in MatchesHead to prevent problems
        // with commands that have matching prefixes.

        if (event.name == "joy") {
            vAssert(event.data.isArray());
            joypad.sticks[0][0] = event.data.at(0).toFloat();
            joypad.sticks[0][1] = event.data.at(1).toFloat();
            joypad.sticks[1][0] = event.data.at(2).toFloat();
            joypad.sticks[1][1] = event.data.at(3).toFloat();
            return;
        }

        if (event.name == "touch") {
            vAssert(event.data.isArray());
            int	action = event.data.at(0).toInt();
            joypad.touch[0] = event.data.at(1).toFloat();
            joypad.touch[1] = event.data.at(2).toFloat();
            if (action == 0) {
                joypad.buttonState |= BUTTON_TOUCH;
            }
            if (action == 1) {
                joypad.buttonState &= ~BUTTON_TOUCH;
            }
            return;
        }

        if (event.name == "key") {
            vAssert(event.data.isArray());
            int	key = event.data.at(0).toInt();
            int down = event.data.at(1).toInt();
            int repeatCount = event.data.at(2).toInt();
            onKeyEvent(key, down, repeatCount);
            return;
        }

        if (event.name == "surfaceChanged") {
            vInfo(event.name);
            if (windowSurface != EGL_NO_SURFACE)
            {	// Samsung says this is an Android problem, where surfaces are reported as
                // created multiple times.
                vWarn("Skipping create work because window hasn't been destroyed.");
                return;
            }
            nativeWindow = static_cast<ANativeWindow *>(event.data.toPointer());

            EGLint attribs[100];
            int		numAttribs = 0;

            // Set the colorspace on the window
            windowSurface = EGL_NO_SURFACE;
            if (appInterface->wantSrgbFramebuffer())
            {
                attribs[numAttribs++] = VEglDriver::EGL_GL_COLORSPACE_KHR;
                attribs[numAttribs++] = VEglDriver::EGL_GL_COLORSPACE_SRGB_KHR;
            }
            // Ask for TrustZone rendering support
            if (appInterface->wantProtectedFramebuffer())
            {
                attribs[numAttribs++] = EGL_PROTECTED_CONTENT_EXT;
                attribs[numAttribs++] = EGL_TRUE;
            }
            attribs[numAttribs++] = EGL_NONE;

            // Android doesn't let the non-standard extensions show up in the
            // extension string, so we need to try it blind.
            windowSurface = eglCreateWindowSurface(m_vrGlStatus.m_display, m_vrGlStatus.m_config,
                    nativeWindow, attribs);


            if (windowSurface == EGL_NO_SURFACE)
            {
                const EGLint attribs2[] =
                {
                    EGL_NONE
                };
                windowSurface = eglCreateWindowSurface(m_vrGlStatus.m_display, m_vrGlStatus.m_config,
                        nativeWindow, attribs2);
                if (windowSurface == EGL_NO_SURFACE)
                {
                    vFatal("eglCreateWindowSurface failed:" << m_vrGlStatus.getEglErrorString());
                }
                framebufferIsSrgb = false;
                framebufferIsProtected = false;
            }
            else
            {
                framebufferIsSrgb = appInterface->wantSrgbFramebuffer();
                framebufferIsProtected = appInterface->wantProtectedFramebuffer();
            }

            if (eglMakeCurrent(m_vrGlStatus.m_display, windowSurface, windowSurface, m_vrGlStatus.m_context) == EGL_FALSE)
            {
                vFatal("eglMakeCurrent failed:" << m_vrGlStatus.getEglErrorString());
            }

            createdSurface = true;

            // Let the client app setup now
            appInterface->onWindowCreated();

            // Resume
            if (!paused)
            {
                resume();
            }
            return;
        }

        if (event.name == "surfaceDestroyed") {
            vInfo("surfaceDestroyed");

            // Let the client app shutdown first.
            appInterface->onWindowDestroyed();

            // Handle it ourselves.
            if (eglMakeCurrent(m_vrGlStatus.m_display, m_vrGlStatus.m_pbufferSurface, m_vrGlStatus.m_pbufferSurface,
                    m_vrGlStatus.m_context) == EGL_FALSE)
            {
                vFatal("RC_SURFACE_DESTROYED: eglMakeCurrent pbuffer failed");
            }

            if (windowSurface != EGL_NO_SURFACE)
            {
                eglDestroySurface(m_vrGlStatus.m_display, windowSurface);
                windowSurface = EGL_NO_SURFACE;
            }
            if (nativeWindow != nullptr)
            {
                ANativeWindow_release(nativeWindow);
                nativeWindow = nullptr;
            }
            return;
        }

        if (event.name == "pause") {
            vInfo("pause");
            if (!paused)
            {
                paused = true;
                pause();
            }
        }

        if (event.name == "resume") {
            vInfo("resume");
            paused = false;
            // Don't actually do the resume operations if we don't have
            // a window yet.  They will be done when the window is created.
            if (windowSurface != EGL_NO_SURFACE)
            {
                resume();
            }
            else
            {
                vInfo("Skipping resume because windowSurface not set yet");
            }
        }

        if (event.name == "intent") {
            vAssert(event.data.isArray());
            VString fromPackageName = event.data.at(0).toString();
            VString uri = event.data.at(1).toString();
            VString json = event.data.at(2).toString();

            if (fromPackageName == EMPTY_INTENT_STR) {
                fromPackageName.clear();
            }
            if (uri == EMPTY_INTENT_STR) {
                uri.clear();
            }

            // assign launchIntent to the intent command
            launchIntentFromPackage = fromPackageName;
            launchIntentJSON = json;
            launchIntentURI = uri;

            // when the PlatformActivity is launched, this is how it gets its command to start
            // a particular UI.
            appInterface->onNewIntent(fromPackageName, json, uri);
            return;
        }

        if (event.name == "popup") {
            vAssert(event.data.isArray());
            int width = event.data.at(0).toInt();
            int height = event.data.at(1).toInt();
            float seconds = event.data.at(2).toFloat();

            dialogWidth = width;
            dialogHeight = height;
            self->dialog.dialogStopSeconds = VTimer::Seconds() + seconds;

            self->dialog.dialogMatrix = PanelMatrix(lastViewMatrix, popupDistance, popupScale, width, height);

            glActiveTexture(GL_TEXTURE0);
            vInfo("RC_UPDATE_POPUP dialogTexture" << self->dialog.dialogTexture->textureId);
            self->dialog.dialogTexture->Update();
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

            return;
        }

        if (event.name == "sync") {
            return;
        }

        if (event.name == "quit") {
            kernel->exit();
            readyToExit = true;
            vInfo("VrThreadSynced=" << vrThreadSynced << " CreatedSurface=" << createdSurface << " ReadyToExit=" << readyToExit);
        }

        // Pass it on to the client app.
        appInterface->command(event);
    }

    void run()
    {
        // Set the name that will show up in systrace
        pthread_setname_np(pthread_self(), "NervGear::VrThread");

        // Initialize the VR thread
        {
            vInfo("AppLocal::VrThreadFunction - init");

            // Get the tid for setting the scheduler
            vrThreadTid = gettid();

            // The Java VM needs to be attached on each thread that will use
            // it.  We need it to call UpdateTexture on surfaceTextures, which
            // must be done on the thread with the openGL context that created
            // the associated texture object current.
            vInfo("javaVM->AttachCurrentThread");
            const jint rtn = javaVM->AttachCurrentThread(&vrJni, 0);
            if (rtn != JNI_OK)
            {
                vFatal("javaVM->AttachCurrentThread returned" << rtn);
            }

            // Set up another thread for making longer-running java calls
            // to avoid hitches.
            activity->Init(javaVM);

            // Create a new context and pbuffer surface
            const int windowDepth = 0;
            const int windowSamples = 0;
            const GLuint contextPriority = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
            m_vrGlStatus.eglInit(EGL_NO_CONTEXT, GL_ES_VERSION,
                    8,8,8, windowDepth, windowSamples, contextPriority);

            // Create our GL data objects
            initGlObjects();

            eyeTargets = new VEyeBuffer;
            guiSys = new OvrGuiSysLocal;
            gazeCursor = new OvrGazeCursorLocal;
            vrMenuMgr = OvrVRMenuMgr::Create();
            debugLines = OvrDebugLines::Create();

            int w = 0;
            int h = 0;
            loadingIconTexId = LoadTextureFromApplicationPackage("res/raw/loading_indicator.png",
                                            TextureFlags_t(TEXTUREFLAG_NO_MIPMAPS), w, h);

            // Create the SurfaceTexture for dialog rendering.
            self->dialog.dialogTexture = new SurfaceTexture(vrJni);

            initFonts();

            soundManager.loadSoundAssets();

            debugLines->Init();

            gazeCursor->Init();

            vrMenuMgr->init();

            guiSys->init(self, *vrMenuMgr, *defaultFont);

            volumePopup = OvrVolumePopup::Create(self, *vrMenuMgr, *defaultFont);

            lastTouchpadTime = VTimer::Seconds();
        }

        // FPS counter information
        int countApplicationFrames = 0;
        double lastReportTime = ceil(VTimer::Seconds());

        while(!(vrThreadSynced && createdSurface && readyToExit))
        {
            //SPAM("FRAME START");

            gazeCursor->BeginFrame();

            // Process incoming messages until queue is empty
            forever {
                VEvent event = eventLoop.next();
                if (!event.isValid()) {
                    break;
                }
                command(event);
            }

            // handle any pending system activity events
            size_t const MAX_EVENT_SIZE = 4096;
            VString eventBuffer;

            for (eVrApiEventStatus status = ovr_nextPendingEvent(eventBuffer, MAX_EVENT_SIZE);
                status >= VRAPI_EVENT_PENDING;
                status = ovr_nextPendingEvent(eventBuffer, MAX_EVENT_SIZE))
            {
                if (status != VRAPI_EVENT_PENDING)
                {
                    if (status != VRAPI_EVENT_CONSUMED)
                    {
                        vWarn("Error" << status << "handing System Activities Event");
                    }
                    continue;
                }

                std::stringstream s;
                s << eventBuffer;
                VJson jsonObj;
                s >> jsonObj;
                if (jsonObj.type() == VJson::Object)
                {
                    VString command = jsonObj["Command"].toString();
                    if (command == SYSTEM_ACTIVITY_EVENT_REORIENT)
                    {
                        // for reorient, we recenter yaw natively, then pass the event along so that the client
                        // application can also handle the event (for instance, to reposition menus)
                        vInfo("VtThreadFunction: Acting on System Activity reorient event.");
                        self->recenterYaw(false);
                    }
                    else
                    {
                        // In the case of the returnToLauncher event, we always handler it internally and pass
                        // along an empty buffer so that any remaining events still get processed by the client.
                        vInfo("Unhandled System Activity event:" << command);
                    }
                }
                else
                {
                    // a malformed event string was pushed! This implies an error in the native code somewhere.
                    vWarn("Error parsing System Activities Event:");
                }
            }

            // update volume popup
            if (showVolumePopup)
            {
                volumePopup->checkForVolumeChange(self);
            }

            // If we don't have a surface yet, or we are paused, sleep until
            // something shows up on the message queue.
            if (windowSurface == EGL_NO_SURFACE || paused)
            {
                if (!(vrThreadSynced && createdSurface && readyToExit))
                {
                    eventLoop.wait();
                }
                continue;
            }

            // if there is an error condition, warp swap and nothing else
            if (errorTexture != 0)
            {
                if (VTimer::Seconds() >= errorMessageEndTime)
                {
                    kernel->destroy(EXIT_TYPE_FINISH_AFFINITY);
                }
                else
                {
                    //ovrTimeWarpParms warpSwapMessageParms = kernel->InitTimeWarpParms(WARP_INIT_MESSAGE, errorTexture.texture);
                    //warpSwapMessageParms.ProgramParms[0] = 0.0f;						// rotation in radians
                    //warpSwapMessageParms.ProgramParms[1] = 1024.0f / errorTextureSize;	// message size factor
                    //kernel->doSmooth(&warpSwapMessageParms);

                    kernel->InitTimeWarpParms();
                    kernel->setSmoothOption( VK_INHIBIT_SRGB_FB | VK_FLUSH | VK_IMAGE);

                    kernel->setSmoothProgram(VK_LOGO);
                    float mprogramParms[4];
                    mprogramParms[0] = 0.0f;		// rotation in radians per second
                    mprogramParms[1] = 2.0f;
                    kernel->setProgramParms(mprogramParms);
                            // icon size factor smaller than fullscreen
                    for ( int eye = 0; eye < 2; eye++ )
                    {
                       kernel->setSmoothEyeTexture(0,eye, 0);
                       kernel->setSmoothEyeTexture(errorTexture.texture,eye,1);

                    }

                    kernel->doSmooth();
                    kernel->setSmoothProgram(VK_DEFAULT);

                }
                continue;
            }

            // Let the client app initialize only once by calling OneTimeInit() when the windowSurface is valid.
            if (!self->oneTimeInitCalled)
            {
                if (appInterface->showLoadingIcon())
                {
                   // const ovrTimeWarpParms warpSwapLoadingIconParms = kernel->InitTimeWarpParms(WARP_INIT_LOADING_ICON, loadingIconTexId);
                   // kernel->doSmooth(&warpSwapLoadingIconParms);


                    kernel->InitTimeWarpParms();
                    kernel->setSmoothOption( VK_INHIBIT_SRGB_FB | VK_FLUSH | VK_IMAGE);
                    kernel->setSmoothProgram(VK_LOGO);
                    float mprogramParms[4];
                    mprogramParms[0] = 1.0f;		// rotation in radians per second
                    mprogramParms[1] = 16.0f;
                    kernel->setProgramParms(mprogramParms);
                            // icon size factor smaller than fullscreen
                    for ( int eye = 0; eye < 2; eye++ )
                    {
                       kernel->setSmoothEyeTexture(0,eye, 0);
                       kernel->setSmoothEyeTexture(loadingIconTexId,eye,1);

                    }

                    kernel->doSmooth();
                    kernel->setSmoothProgram(VK_DEFAULT);

                }
                vInfo("launchIntentJSON:" << launchIntentJSON);
                vInfo("launchIntentURI:" << launchIntentURI);

                appInterface->init(launchIntentFromPackage, launchIntentJSON, launchIntentURI);
                self->oneTimeInitCalled = true;
            }

            // latch the current joypad state and note transitions
            self->text.vrFrame.Input = joypad;
            self->text.vrFrame.Input.buttonPressed = joypad.buttonState & (~lastVrFrame.Input.buttonState);
            self->text.vrFrame.Input.buttonReleased = ~joypad.buttonState & (lastVrFrame.Input.buttonState & ~BUTTON_TOUCH_WAS_SWIPE);

            if (lastVrFrame.Input.buttonState & BUTTON_TOUCH_WAS_SWIPE)
            {
                if (lastVrFrame.Input.buttonReleased & BUTTON_TOUCH)
                {
                    self->text.vrFrame.Input.buttonReleased |= BUTTON_TOUCH_WAS_SWIPE;
                }
                else
                {
                    // keep it around this frame
                    self->text.vrFrame.Input.buttonState |= BUTTON_TOUCH_WAS_SWIPE;
                }
            }

            // Synthesize swipe gestures
            interpretTouchpad(self->text.vrFrame.Input);

            if (recenterYawFrameStart != 0)
            {
                // Perform a reorient before sensor data is read.  Allows apps to reorient without having invalid orientation information for that frame.
                // Do a warp swap black on the frame the recenter started.
                self->recenterYaw(recenterYawFrameStart == (self->text.vrFrame.FrameNumber + 1));  // vrFrame.FrameNumber hasn't been incremented yet, so add 1.
            }

            // Get the latest head tracking state, predicted ahead to the midpoint of the time
            // it will be displayed.  It will always be corrected to the real values by
            // time warp, but the closer we get, the less black will be pulled in at the edges.
            const double now = VTimer::Seconds();
            static double prev;
            const double rawDelta = now - prev;
            prev = now;
            const double clampedPrediction = std::min(0.1, rawDelta * 2);
            sensorForNextWarp = kernel->ovr_GetPredictedSensorState(now + clampedPrediction);

            self->text.vrFrame.PoseState = sensorForNextWarp.Predicted;
            self->text.vrFrame.OvrStatus = sensorForNextWarp.Status;
            self->text.vrFrame.DeltaSeconds   = std::min(0.1, rawDelta);
            self->text.vrFrame.FrameNumber++;

            // Don't allow this to be excessively large, which can cause application problems.
            if (self->text.vrFrame.DeltaSeconds > 0.1f)
            {
                self->text.vrFrame.DeltaSeconds = 0.1f;
            }

            lastVrFrame = self->text.vrFrame;

            // resend any debug lines that have expired
            debugLines->BeginFrame(self->text.vrFrame.FrameNumber);

            // reset any VR menu submissions from previous frame
            vrMenuMgr->beginFrame();

            frameworkButtonProcessing(self->text.vrFrame.Input);

            KeyState::eKeyEventType event = backKeyState.Update(VTimer::Seconds());
            if (event != KeyState::KEY_EVENT_NONE)
            {
                //vInfo("BackKey: event" << KeyState::EventNames[ event ]);
                // always allow the gaze cursor to peek at the event so it can start the gaze timer if necessary
                // update the gaze cursor timer
                if (event == KeyState::KEY_EVENT_DOWN)
                {
                    gazeCursor->StartTimer(backKeyState.GetLongPressTime(), backKeyState.GetDoubleTapTime());
                }
                else if (event == KeyState::KEY_EVENT_DOUBLE_TAP || event == KeyState::KEY_EVENT_SHORT_PRESS)
                {
                    gazeCursor->CancelTimer();
                }
                else if (event == KeyState::KEY_EVENT_LONG_PRESS)
                {
                    //StartSystemActivity(PUI_GLOBAL_MENU);
                    kernel->destroy(EXIT_TYPE_FINISH);
                }

                // let the menu handle it if it's open
                bool consumedKey = guiSys->onKeyEvent(self, AKEYCODE_BACK, event);

                // pass to the app if nothing handled it before this
                if (!consumedKey)
                {
                    consumedKey = appInterface->onKeyEvent(AKEYCODE_BACK, event);
                }
                // if nothing consumed the key and it's a short-press, exit the application to OculusHome
                if (!consumedKey)
                {
                    if (event == KeyState::KEY_EVENT_SHORT_PRESS)
                    {
                        consumedKey = true;
                        vInfo("BUTTON_BACK: confirming quit in platformUI");
                        kernel->destroy(EXIT_TYPE_FINISH);
                    }
                }
            }

            // draw info text
            if (self->text.infoTextEndFrame >= self->text.vrFrame.FrameNumber)
            {
                V3Vectf viewPos = GetViewMatrixPosition(lastViewMatrix);
                V3Vectf viewFwd = GetViewMatrixForward(lastViewMatrix);
                V3Vectf viewUp(0.0f, 1.0f, 0.0f);
                V3Vectf viewLeft = viewUp.Cross(viewFwd);
                V3Vectf newPos = viewPos + viewFwd * self->text.infoTextOffset.z + viewUp * self->text.infoTextOffset.y + viewLeft * self->text.infoTextOffset.x;
                self->text.infoTextPointTracker.Update(VTimer::Seconds(), newPos);

                fontParms_t fp;
                fp.AlignHoriz = HORIZONTAL_CENTER;
                fp.AlignVert = VERTICAL_CENTER;
                fp.Billboard = true;
                fp.TrackRoll = false;
                worldFontSurface->DrawTextBillboarded3Df(*defaultFont, fp, self->text.infoTextPointTracker.GetCurPosition(),
                        1.0f, self->text.infoTextColor, self->text.infoText.toCString());
            }

            // Main loop logic / draw code
            if (!readyToExit)
            {
                lastViewMatrix = appInterface->onNewFrame(self->text.vrFrame);
                scene->update();
            }

            kernel->ovr_HandleDeviceStateChanges();

            // MWC demo hack to allow keyboard swipes
            joypad.buttonState &= ~(BUTTON_SWIPE_FORWARD|BUTTON_SWIPE_BACK);

            // Report frame counts once a second
            countApplicationFrames++;
            const double timeNow = floor(VTimer::Seconds());
            if (timeNow > lastReportTime)
            {

                countApplicationFrames = 0;
                lastReportTime = timeNow;
            }

            //SPAM("FRAME END");
        }

        // Shutdown the VR thread
        {
            vInfo("AppLocal::VrThreadFunction - shutdown");

            // Shut down the message queue so it cannot overflow.
            eventLoop.quit();

            if (errorTexture != 0)
            {
                FreeTexture(errorTexture);
            }

            appInterface->shutdown();

            guiSys->shutdown(*vrMenuMgr);

            vrMenuMgr->shutdown();

            gazeCursor->Shutdown();

            debugLines->Shutdown();

            shutdownFonts();

            delete self->dialog.dialogTexture;
            self->dialog.dialogTexture = nullptr;

            delete eyeTargets;
            eyeTargets = nullptr;

            delete guiSys;
            guiSys = nullptr;

            delete gazeCursor;
            gazeCursor = nullptr;

            OvrVRMenuMgr::Free(vrMenuMgr);
            OvrDebugLines::Free(debugLines);

            shutdownGlObjects();
            m_vrGlStatus.eglExit();

            vInfo("javaVM->DetachCurrentThread");
            const jint rtn = javaVM->DetachCurrentThread();
            if (rtn != JNI_OK)
            {
                vInfo("javaVM->DetachCurrentThread returned" << rtn);
            }
        }
    }

    jclass getGlobalClassReference(const char * className) const
    {
        jclass lc = uiJni->FindClass(className);
        if (lc == 0) {
            vFatal("Failed to find class" << className);
        }
        // Turn it into a global ref, so we can safely use it in the VR thread
        jclass gc = (jclass) uiJni->NewGlobalRef(lc);
        uiJni->DeleteLocalRef(lc);
        return gc;
    }


    void onKeyEvent(const int keyCode, const bool down, const int repeatCount)
    {
        // the back key is special because it has to handle long-press and double-tap
        if (keyCode == AKEYCODE_BACK)
        {
            // back key events, because of special handling for double-tap, short-press and long-press,
            // are handled in AppLocal::VrThreadFunction.
            backKeyState.HandleEvent(VTimer::Seconds(), down, repeatCount);
            return;
        }

        // the app menu is always the first consumer so it cannot be usurped
        bool consumedKey = false;
        if (repeatCount == 0)
        {
            consumedKey = guiSys->onKeyEvent(self, keyCode, down ? KeyState::KEY_EVENT_DOWN : KeyState::KEY_EVENT_UP);
        }

        // for all other keys, allow VrAppInterface the chance to handle and consume the key first
        if (!consumedKey)
        {
            consumedKey = appInterface->onKeyEvent(keyCode, down ? KeyState::KEY_EVENT_DOWN : KeyState::KEY_EVENT_UP);
        }

        // ALL VRLIB KEY HANDLING OTHER THAN APP MENU SHOULD GO HERE
        if (!consumedKey && enableDebugOptions)
        {
            float const IPD_STEP = 0.001f;

            // FIXME: this should set consumedKey = true now that we pass key events via appInterface first,
            // but this would likely break some apps right now that rely on the old behavior
            // consumedKey = true;

            if (down && keyCode == AKEYCODE_RIGHT_BRACKET)
            {
                vInfo("BUTTON_SWIPE_FORWARD");
                joypad.buttonState |= BUTTON_SWIPE_FORWARD;
                return;
            }
            else if (down && keyCode == AKEYCODE_LEFT_BRACKET)
            {
                vInfo("BUTTON_SWIPE_BACK");
                joypad.buttonState |= BUTTON_SWIPE_BACK;
                return;
            }
            else if (keyCode == AKEYCODE_S)
            {
                if (repeatCount == 0 && down) // first down only
                {
                    eyeTargets->ScreenShot();
                    self->createToast("screenshot");
                    return;
                }
            }
            else if (keyCode == AKEYCODE_COMMA && down && repeatCount == 0)
            {
                float const IPD_MIN_CM = 0.0f;
                viewParms.InterpupillaryDistance = std::max(IPD_MIN_CM * 0.01f, viewParms.InterpupillaryDistance - IPD_STEP);
                VString text;
                text.sprintf("%.3f", viewParms.InterpupillaryDistance);
                self->text.show(text, 1.0f);
                return;
            }
            else if (keyCode == AKEYCODE_PERIOD && down && repeatCount == 0)
            {
                float const IPD_MAX_CM = 8.0f;
                viewParms.InterpupillaryDistance = std::min(IPD_MAX_CM * 0.01f, viewParms.InterpupillaryDistance + IPD_STEP);
                VString text;
                text.sprintf("%.3f", viewParms.InterpupillaryDistance);
                self->text.show(text, 1.0f);
                return;
            }
        }

        // Keys always map to joystick buttons right now even if consumed otherwise.
        // This probably should change but it's unclear what this might break right now.
        for (int i = 0 ; buttonMappings[i] != -1 ; i++)
        {
            // joypad buttons come in with 0x10000 as a flag
            if ((keyCode & ~0x10000) == buttonMappings[i])
            {
                if (down)
                {
                    joypad.buttonState |= 1<<i;
                }
                else
                {
                    joypad.buttonState &= ~(1<<i);
                }
                return;
            }
        }
    }
};

/*
 * AppLocal
 *
 * Called once at startup.
 *
 * ?still true?: exit() from here causes an immediate app re-launch,
 * move everything to first surface init?
 */

App *NervGearAppInstance = nullptr;

App::App(JNIEnv *jni, jobject activityObject, VMainActivity *activity)
    : oneTimeInitCalled(false)
    , d(new Private(this))
{
    d->activity = activity;

    d->uiJni = jni;
    vInfo("----------------- AppLocal::AppLocal() -----------------");
    vAssert(NervGearAppInstance == nullptr);
    NervGearAppInstance = this;

    d->kernel = VKernel::GetInstance();
    d->storagePaths = new VStandardPath(jni, activityObject);

	//WaitForDebuggerToAttach();

    memset(& d->sensorForNextWarp, 0, sizeof(d->sensorForNextWarp));

    d->sensorForNextWarp.Predicted.Orientation = VQuatf();

    JniUtils::LoadDevConfig(false);

	// Default time warp parms
   d->kernel->InitTimeWarpParms();

	// Default EyeParms
    d->vrParms.resolution = 1024;
    d->vrParms.multisamples = 4;
    d->vrParms.colorFormat = VColor::COLOR_8888;
    d->vrParms.commonParameterDepth = VEyeBuffer::CommonParameter::DEPTHFORMAT_DEPTH_24;

    d->javaObject = d->uiJni->NewGlobalRef(activityObject);

	// A difficulty with JNI is that we can't resolve our (non-Android) package
	// classes on other threads, so lookup everything we need right now.
    d->vrActivityClass = d->getGlobalClassReference(activityClassName);
    VrLocale::VrActivityClass = d->vrActivityClass;

    d->createVrToastMethodId = d->GetMethodID("createVrToastOnUiThread", "(Ljava/lang/String;)V");
    d->clearVrToastsMethodId = d->GetMethodID("clearVrToasts", "()V");
    d->playSoundPoolSoundMethodId = d->GetMethodID("playSoundPoolSound", "(Ljava/lang/String;)V");

	// Get the path to the .apk and package name
    d->packageCodePath = d->activity->getPackageCodePath();

	// Hook the App and AppInterface together
    d->appInterface = activity;

	// Load user profile data relevant to rendering
    VUserSettings config;
    config.load();
    d->viewParms.InterpupillaryDistance = config.ipd;
    d->viewParms.EyeHeight = config.eyeHeight;
    d->viewParms.HeadModelDepth = config.headModelDepth;
    d->viewParms.HeadModelHeight = config.headModelHeight;

    d->renderThread = new VThread([](void *data)->int{
        App::Private *d = static_cast<App::Private *>(data);
        d->run();
        return 0;
    }, d);
}

App::~App()
{
    vInfo("---------- ~AppLocal() ----------");

    if (d->javaObject != 0)
	{
        //d->uiJni->DeleteGlobalRef(d->javaObject);
	}

    if (d->storagePaths != nullptr)
	{
        delete d->storagePaths;
        d->storagePaths = nullptr;
	}

    delete d->activity;
    delete d;
}

void App::execute()
{
    d->renderThread->start();
    d->eventLoop.send("sync");
    d->vrThreadSynced = true;
}

void App::quit()
{
    d->eventLoop.post("quit");
    bool finished = d->renderThread->wait();
    if (!finished) {
        vWarn("failed to wait for VrThread");
	}
}

VEventLoop &App::eventLoop()
{
    return d->eventLoop;
}

void App::createToast(const char * fmt, ...)
{
	char bigBuffer[4096];
	va_list	args;
    va_start(args, fmt);
    vsnprintf(bigBuffer, sizeof(bigBuffer), fmt, args);
    va_end(args);

    JNIEnv *jni = nullptr;
    jstring cmdString = nullptr;
    if (d->javaVM->AttachCurrentThread(&jni, nullptr) == JNI_OK) {
        cmdString = JniUtils::Convert(jni, bigBuffer);

        d->activity->eventLoop().post([=]{
            JNIEnv *jni = nullptr;
            if (d->javaVM->AttachCurrentThread(&jni, nullptr) == JNI_OK) {
                jni->CallVoidMethod(d->javaObject, d->createVrToastMethodId, cmdString);
            }
            jni->DeleteLocalRef(cmdString);
        });
    }
}

void App::playSound(const char *name)
{
	// Get sound from SoundManager
	VString soundFile;
    if (!d->soundManager.getSound(name, soundFile)) {
        soundFile = VString::fromUtf8(name);
    }

    d->activity->eventLoop().post([=]{
        JNIEnv *jni = nullptr;
        if (d->javaVM->AttachCurrentThread(&jni, 0) == JNI_OK) {
            jstring cmdString = JniUtils::Convert(jni, soundFile);
            jni->CallVoidMethod(d->javaObject, d->playSoundPoolSoundMethodId, cmdString);
            jni->DeleteLocalRef(cmdString);
        }
    });
}

//void ToggleScreenColor()
//{
//
//	static int	color;

//	color ^= 1;

//    glEnable(GL_WRITEONLY_RENDERING_QCOM);
//    glClearColor(color, 1-color, 0, 1);
//    glClear(GL_COLOR_BUFFER_BIT);

//	// The Adreno driver has an unfortunate optimization so it doesn't
//	// actually flush if all that was done was a clear.
//    glOperation.GL_Finish();
//    glDisable(GL_WRITEONLY_RENDERING_QCOM);
//}

/*
 * eyeParms()
 */
VEyeBuffer::EyeParms App::eyeParms()
{
    return d->vrParms;
}

/*
 * SetVrParms()
 */
void App::setEyeParms(const VEyeBuffer::EyeParms parms)
{
    d->vrParms = parms;
}

OvrGuiSys & App::guiSys()
{
    return *d->guiSys;
}
OvrGazeCursor & App::gazeCursor()
{
    return *d->gazeCursor;
}
BitmapFont & App::defaultFont()
{
    return *d->defaultFont;
}
BitmapFontSurface & App::worldFontSurface()
{
    return *d->worldFontSurface;
}
BitmapFontSurface & App::menuFontSurface()
{
    return *d->menuFontSurface;
}   // TODO: this could be in the menu system now

OvrVRMenuMgr & App::vrMenuMgr()
{
    return *d->vrMenuMgr;
}
OvrDebugLines & App::debugLines()
{
    return *d->debugLines;
}
const VStandardPath & App::storagePaths()
{
    return *d->storagePaths;
}
 VSoundManager & App::soundMgr()
{
    return d->soundManager;
}

bool App::isGuiOpen() const
{
    return d->guiSys->isAnyMenuOpen();
}

bool App::framebufferIsSrgb() const
{
    return d->framebufferIsSrgb;
}

bool App::framebufferIsProtected() const
{
    return d->framebufferIsProtected;
}

bool App::renderMonoMode() const
{
    return d->renderMonoMode;
}

void App::setRenderMonoMode(bool const mono)
{
    d->renderMonoMode = mono;
}

const VString &App::packageCodePath() const
{
    return d->packageCodePath;
}

VR4Matrixf const & App::lastViewMatrix() const
{
    return d->lastViewMatrix;
}

void App::setLastViewMatrix(VR4Matrixf const & m)
{
    d->lastViewMatrix = m;
}

VEyeBuffer::EyeParms & App::vrParms()
{
    return d->vrParms;
}

void App::setPopupDistance(float const distance)
{
    d->popupDistance = distance;
}

float App::popupDistance() const
{
    return d->popupDistance;
}

void App::setPopupScale(float const s)
{
    d->popupScale = s;
}

float App::popupScale() const
{
    return d->popupScale;
}



JavaVM	* App::javaVM()
{
    return d->javaVM;
}

JNIEnv * App::uiJni()
{
    return d->uiJni;
}

JNIEnv * App::vrJni()
{
    return d->vrJni;
}

jobject	& App::javaObject()
{
    return d->javaObject;
}

jclass & App::vrActivityClass()
{
    return d->vrActivityClass;
}

VKernel* App::kernel()
{
    return  d->kernel;
}

SurfaceTexture * App::dialogTexture()
{
    return dialog.dialogTexture;
}



ovrSensorState const & App::sensorForNextWarp() const
{
    return d->sensorForNextWarp;
}

VMainActivity *App::appInterface()
{
    return d->appInterface;
}

VrViewParms const &	App::vrViewParms() const
{
    return d->viewParms;
}

void App::setVrViewParms(VrViewParms const & parms)
{
    d->viewParms = parms;
}

KeyState & App::backKeyState()
{
    return d->backKeyState;
}

void App::setShowVolumePopup(bool const show)
{
    d->showVolumePopup = show;
}

bool App::showVolumePopup() const
{
    return d->showVolumePopup;
}

void App::recenterYaw(const bool showBlack)
{
    vInfo("AppLocal::RecenterYaw");
    if (showBlack)
	{
        //const ovrTimeWarpParms warpSwapBlackParms = d->kernel->InitTimeWarpParms(WARP_INIT_BLACK);
        //d->kernel->doSmooth(&warpSwapBlackParms);
        d->kernel->InitTimeWarpParms();
        d->kernel->setSmoothOption( VK_INHIBIT_SRGB_FB | VK_FLUSH | VK_IMAGE);
        d->kernel->setSmoothProgram( VK_DEFAULT);
        for ( int eye = 0; eye < 2; eye++ )
        {
           d->kernel->setSmoothEyeTexture(eye,0,0);		// default replaced with a black texture
        }


        d->kernel->doSmooth();


	}
    d->kernel->ovr_RecenterYaw();

	// Change lastViewMatrix to mirror what is done to the sensor orientation by ovr_RecenterYaw.
	// Get the current yaw rotation and cancel it out. This is necessary so that subsystems that
	// rely on lastViewMatrix do not end up using the orientation from before the recenter if they
	// are called before the beginning of the next frame.
	float yaw;
	float pitch;
	float roll;
    d->lastViewMatrix.ToEulerAngles< VAxis_Y, VAxis_X, VAxis_Z, VRotate_CCW, VHanded_R >(&yaw, &pitch, &roll);

	// undo the yaw
    VR4Matrixf unrotYawMatrix(VQuatf(VAxis_Y, -yaw));
    d->lastViewMatrix = d->lastViewMatrix * unrotYawMatrix;

    guiSys().resetMenuOrientations(this, d->lastViewMatrix);
}

void App::setRecenterYawFrameStart(const long long frameNumber)
{
    d->recenterYawFrameStart = frameNumber;
}

long long App::recenterYawFrameStart() const
{
    return d->recenterYawFrameStart;
}

// Debug tool to draw outlines of a 3D bounds
//void App::drawBounds( const V3Vectf &mins, const V3Vectf &maxs, const VR4Matrixf &mvp, const V3Vectf &color )
//{

//
//    VR4Matrixf	scaled = mvp * VR4Matrixf::Translation( mins ) * VR4Matrixf::Scaling( maxs - mins );
//    const VGlShader & prog = d->untexturedMvpProgram;
//    glUseProgram(prog.program);
//    glLineWidth( 1.0f );
//    glUniform4f(prog.uniformColor, color.x, color.y, color.z, 1);
//    glUniformMatrix4fv(prog.uniformModelViewProMatrix, 1, GL_FALSE /* not transposed */,
//            scaled.Transposed().M[0] );
//    glOperation.glBindVertexArrayOES_( d->unitCubeLines.vertexArrayObject );
//    glDrawElements(GL_LINES, d->unitCubeLines.indexCount, GL_UNSIGNED_SHORT, NULL);
//    glOperation.glBindVertexArrayOES_( 0 );
//}

void App::drawEyeViewsPostDistorted( VR4Matrixf const & centerViewMatrix, const int numPresents )
{


    VEglDriver glOperation;
    // update vr lib systems after the app frame, but before rendering anything
    guiSys().frame( this, text.vrFrame, vrMenuMgr(), defaultFont(), menuFontSurface(), centerViewMatrix );
    gazeCursor().Frame( centerViewMatrix, text.vrFrame.DeltaSeconds );

    menuFontSurface().Finish( centerViewMatrix );
    worldFontSurface().Finish( centerViewMatrix );
    vrMenuMgr().finish( centerViewMatrix );

    // Increase the fov by about 10 degrees if we are not holding 60 fps so
    // there is less black pull-in at the edges.
    //
    // Doing this dynamically based just on time causes visible flickering at the
    // periphery when the fov is increased, so only do it if minimumVsyncs is set.
    const float fovDegrees = d->kernel->device->eyeDisplayFov[0] +
            ( ( d->kernel->m_minimumVsyncs > 1 ) ? 10.0f : 0.0f ) +
            ( ( !d->showVignette ) ? 5.0f : 0.0f );

    // DisplayMonoMode uses a single eye rendering for speed improvement
    // and / or high refresh rate double-scan hardware modes.
    const int numEyes = d->renderMonoMode ? 1 : 2;


    // Flush out and report any errors
    glOperation.logErrorsEnum("FrameStart");

    if ( d->drawCalibrationLines && d->calibrationLinesDrawn )
    {
        // doing a time warp test, don't generate new images
        vInfo( "drawCalibrationLines && calibrationLinesDrawn" );
    }
    else
    {
        // possibly change the buffer parameters
        d->eyeTargets->BeginFrame( d->vrParms );

        for ( int eye = 0; eye < numEyes; eye++ )
        {
            d->eyeTargets->BeginRenderingEye( eye );

            // Call back to the app for drawing.
            const VR4Matrixf mvp = d->appInterface->drawEyeView( eye, fovDegrees );

            vrMenuMgr().renderSubmitted( mvp.Transposed(), centerViewMatrix );
            menuFontSurface().Render3D( defaultFont(), mvp.Transposed() );
            worldFontSurface().Render3D( defaultFont(), mvp.Transposed() );

            glDisable( GL_DEPTH_TEST );
            glDisable( GL_CULL_FACE );

            // Optionally draw thick calibration lines into the texture,
            // which will be overlayed by the thinner origin cross when
            // distorted to the window.
            if ( d->drawCalibrationLines )
            {
                d->eyeDecorations.DrawEyeCalibrationLines( fovDegrees, eye );
                d->calibrationLinesDrawn = true;
            }
            else
            {
                d->calibrationLinesDrawn = false;
            }

            dialog.draw( panel, mvp );

            gazeCursor().Render( eye, mvp );

            debugLines().Render( mvp.Transposed() );

            if ( d->showVignette )
            {
                // Draw a thin vignette at the edges of the view so clamping will give black
                // This will not be reflected correctly in overlay planes.
                // EyeDecorations.DrawEyeVignette();

                d->eyeDecorations.FillEdge( d->vrParms.resolution, d->vrParms.resolution );
            }

            d->eyeTargets->EndRenderingEye( eye );
        }
    }

    // This eye set is complete, use it now.
    if ( numPresents > 0 )
    {
        const VEyeBuffer::CompletedEyes eyes = d->eyeTargets->GetCompletedEyes();

        for ( int eye = 0; eye < 2; eye++ )
        {            
           d->kernel->m_texMatrix[eye][0] = VR4Matrixf::TanAngleMatrixFromFov( fovDegrees );
           d->kernel->m_texId[eye][0] = eyes.textures[d->renderMonoMode ? 0 : eye ];
           d->kernel->m_pose[eye][0] = d->sensorForNextWarp.Predicted;
          // d->kernel->m_smoothProgram = ChromaticAberrationCorrection(glOperation) ? WP_CHROMATIC : WP_SIMPLE;

        }

        d->kernel->doSmooth();

    }
}

// Draw a screen to an eye buffer the same way it would be drawn as a
// time warp overlay.
//void App::drawScreenDirect( const GLuint texid, const ovrMatrix4f & mvp )
//{
//
//    const VR4Matrixf mvpMatrix( mvp );
//    glActiveTexture( GL_TEXTURE0 );
//    glBindTexture( GL_TEXTURE_2D, texid );

//    glUseProgram( d->overlayScreenDirectProgram.program );

//    glUniformMatrix4fv( d->overlayScreenDirectProgram.uniformModelViewProMatrix, 1, GL_FALSE, mvpMatrix.Transposed().M[0] );

//    glOperation.glBindVertexArrayOES_( d->unitSquare.vertexArrayObject );
//    glDrawElements( GL_TRIANGLES, d->unitSquare.indexCount, GL_UNSIGNED_SHORT, NULL );

//    glBindTexture( GL_TEXTURE_2D, 0 );	// don't leave it bound
//}

// draw a zero to destination alpha
void App::drawScreenMask( const VR4Matrixf & mvp, const float fadeFracX, const float fadeFracY )
{
    VR4Matrixf mvpMatrix( mvp );

    glUseProgram( d->overlayScreenFadeMaskProgram.program );

    glUniformMatrix4fv( d->overlayScreenFadeMaskProgram.uniformModelViewProMatrix, 1, GL_FALSE, mvpMatrix.Transposed().M[0] );

    if ( d->fadedScreenMaskSquare.vertexArrayObject == 0 )
    {
        d->fadedScreenMaskSquare.createScreenQuad( fadeFracX, fadeFracY );
    }

    glColorMask( 0.0f, 0.0f, 0.0f, 1.0f );
    d->fadedScreenMaskSquare.drawElements();
    glColorMask( 1.0f, 1.0f, 1.0f, 1.0f );
}

NV_NAMESPACE_END
