#include <pthread.h>
#include "VFrameSmooth.h"
#include "VAlgorithm.h"
#include <errno.h>
#include <math.h>
#include <sched.h>
#include <unistd.h>

#include <android/sensor.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "VFrameSmooth.h"
#include "Android/LogUtils.h"
#include "Android/JniUtils.h"
#include "Vsync.h"
#include "VLensDistortion.h"
#include "api/VGlOperation.h"
#include "../core/VString.h"

#include "../embedded/oculus_loading_indicator.h"

#include "VLockless.h"
#include "VGlGeometry.h"
#include "VGlShader.h"
#include "VKernel.h"

ovrSensorState ovr_GetSensorStateInternal( double absTime );
bool ovr_ProcessLatencyTest( unsigned char rgbColorOut[3] );
const char * ovr_GetLatencyTestResult();


NV_NAMESPACE_BEGIN

struct warpSource_t
{
    long long			MinimumVsync;				// Never pick up a source if it is from the current vsync.
    long long			FirstDisplayedVsync[2];		// External velocity is added after this vsync.
    bool				disableChromaticCorrection;	// Disable correction for chromatic aberration.
    EGLSyncKHR			GpuSync;					// When this sync completes, the textures are done rendering.
    ovrTimeWarpParms	WarpParms;					// passed into WarpSwap()
};

struct swapProgram_t
{

    bool	singleThread;

    bool	dualMonoDisplay;


    float	deltaVsync[2];

    float	predictionPoints[2][2];
};


struct SwapState
{
    SwapState() : VsyncCount(0),EyeBufferCount(0) {}
    long long		VsyncCount;
    long long		EyeBufferCount;
};

struct eyeLog_t
{

    bool		skipped;

    int			bufferNum;

    float		issueFinish;
    float		completeFinish;

    float		poseLatencySeconds;
};


swapProgram_t	spAsyncSwappedBufferPortrait = {
    false,	false, { 0.0, 0.5},	{ {1.0, 1.5}, {1.5, 2.0} }
};


swapProgram_t	spSyncFrontBufferPortrait = {
    true,	false, { 0.5, 1.0},	{ {1.0, 1.5}, {1.5, 2.0} }
};


swapProgram_t	spSyncSwappedBufferPortrait = {
    true,	false, { 0.0, 0.0},	{ {2.0, 2.5}, {2.5, 3.0} }
};



void EyeRectLandscape( const VDevice *device,int eye,int &x, int &y, int &width, int &height )
{

    int scissorX = ( eye == 0 ) ? 0 : device->widthbyPixels / 2;
    int scissorY = 0;
    int scissorWidth = device->widthbyPixels / 2;
    int scissorHeight = device->heightbyPixels;
    x = scissorX;
    y = scissorY;
    width = scissorWidth;
    height = scissorHeight;
    return;    
}

void EyeRect( const VDevice *device,const int eye,
              int &x, int &y, int &width, int &height )
{
    int	lx, ly, lWidth, lHeight;
    EyeRectLandscape( device, eye, lx, ly, lWidth, lHeight );

    x = lx;
    y = ly;
    width = lWidth;
    height = lHeight;
}

VR4Matrixf CalculateTimeWarpMatrix2( const VQuatf &inFrom, const VQuatf &inTo )
{    
    VQuatf from = inFrom;
    VQuatf to = inTo;

    bool fromValid = from.LengthSq() > 0.95f;
    bool toValid = to.LengthSq() > 0.95f;
    if ( !fromValid )
    {
        if ( toValid )
        {
            from = to;
        }
        else
        {
            from = VQuatf( 0.0f, 0.0f, 0.0f, 1.0f );
        }
    }
    if ( !toValid )
    {
        if ( fromValid )
        {
            to = from;
        }
        else
        {
            to = VQuatf( 0.0f, 0.0f, 0.0f, 1.0f );
        }
    }

    VR4Matrixf		lastSensorMatrix = VR4Matrixf( to );
    VR4Matrixf		lastViewMatrix = VR4Matrixf( from );

    return ( lastSensorMatrix.Inverted() * lastViewMatrix ).Inverted();
}


static bool IsContextPriorityExtensionPresent()
{
    EGLint currentPriorityLevel = -1;
    if ( !eglQueryContext( eglGetCurrentDisplay(), eglGetCurrentContext(), EGL_CONTEXT_PRIORITY_LEVEL_IMG, &currentPriorityLevel )
         || currentPriorityLevel == -1 )
    {

        return false;
    }
    return true;
}

//=========================================================================================

struct VFrameSmooth::Private
{
    Private(bool async,VDevice *device):
            m_untexturedMvpProgram(),
            m_debugLineProgram(),
            m_warpPrograms(),
            m_blackTexId( 0 ),
            m_defaultLoadingIconTexId( 0 ),
            m_hasEXT_sRGB_write_control( false ),
            m_sStartupTid( 0 ),
            m_jni( NULL ),
            m_eglDisplay( 0 ),
            m_eglPbufferSurface( 0 ),
            m_eglMainThreadSurface( 0 ),
            m_eglConfig( 0 ),
            m_eglClientVersion( 0 ),
            m_eglShareContext( 0 ),
            m_eglWarpContext( 0 ),
            m_contextPriority( 0 ),
            m_eyeLog(),
            m_lastEyeLog( 0 ),
            m_logEyeWarpGpuTime(),
            m_smoothThread( 0 ),
            m_smoothThreadTid( 0 ),
            m_lastSwapVsyncCount( 0 )
    {

        static_assert( ( WP_PROGRAM_MAX & 1 ) == 0, "WP_PROGRAM_MAX" );
        static_assert( ( WP_CHROMATIC - WP_SIMPLE ) ==
                             ( WP_PROGRAM_MAX - WP_CHROMATIC ) , "WP_CHROMATIC");
        VGlOperation glOperation;
        m_shutdownRequest.setState( false );
        m_eyeBufferCount.setState( 0 );
        memset( m_warpPrograms, 0, sizeof( m_warpPrograms ) );

        pthread_mutex_init( &m_smoothMutex, NULL /* default attributes */ );
        pthread_cond_init( &m_smoothIslocked, NULL /* default attributes */ );
        vInfo("-------------------- VFrameSmooth() --------------------");

        m_sStartupTid = gettid();
        m_lastsmoothTimeInSeconds.setState( ovr_GetTimeInSeconds() );
        m_smoothThread = 0;
        m_async = async;
        m_device = device;
        m_eyeBufferCount.setState( 0 );
        m_eglDisplay = eglGetCurrentDisplay();
        if ( m_eglDisplay == EGL_NO_DISPLAY )
        {
            vFatal("EGL_NO_DISPLAY");
        }
        m_eglMainThreadSurface = eglGetCurrentSurface( EGL_DRAW );
        if ( m_eglMainThreadSurface == EGL_NO_SURFACE )
        {
            vFatal("EGL_NO_SURFACE");
        }
        m_eglShareContext = eglGetCurrentContext();
        if ( m_eglShareContext == EGL_NO_CONTEXT )
        {
            vFatal("EGL_NO_CONTEXT");
        }
        EGLint configID;
        if ( !eglQueryContext( m_eglDisplay, m_eglShareContext, EGL_CONFIG_ID, &configID ) )
        {
            vFatal("eglQueryContext EGL_CONFIG_ID failed");
        }
        m_eglConfig = glOperation.eglConfigForConfigID( m_eglDisplay, configID );
        if ( m_eglConfig == NULL )
        {
            vFatal("EglConfigForConfigID failed");
        }
        if ( !eglQueryContext( m_eglDisplay, m_eglShareContext, EGL_CONTEXT_CLIENT_VERSION, (EGLint *)&m_eglClientVersion ) )
        {
            vFatal("eglQueryContext EGL_CONTEXT_CLIENT_VERSION failed");
        }

        vInfo("Current EGL_CONTEXT_CLIENT_VERSION:" << m_eglClientVersion);

        EGLint depthSize = 0;
        eglGetConfigAttrib( m_eglDisplay, m_eglConfig, EGL_DEPTH_SIZE, &depthSize );
        if ( depthSize != 0 )
        {
            vInfo("Share context eglConfig has " << depthSize << " depth bits -- should be 0");
        }
        EGLint samples = 0;
        eglGetConfigAttrib( m_eglDisplay, m_eglConfig, EGL_SAMPLES, &samples );
        if ( samples != 0 )
        {

            vInfo("Share context eglConfig has " << samples << " samples -- should be 0");
        }

        m_hasEXT_sRGB_write_control = glOperation.glIsExtensionString( "GL_EXT_sRGB_write_control",
                                                                          (const char *)glGetString( GL_EXTENSIONS ) );
        if ( !m_async )
        {
            initRenderEnvironment();
            createFrameworkGraphics();
            vInfo("Skipping thread setup because !AsynchronousTimeWarp");
        }
        else
        {
            //---------------------------------------------------------
            // Thread initialization
            //---------------------------------------------------------

            if ( IsContextPriorityExtensionPresent() )
            {
                vInfo("Requesting EGL_CONTEXT_PRIORITY_HIGH_IMG");
                m_contextPriority = EGL_CONTEXT_PRIORITY_HIGH_IMG;
            }
            else
            {

                vInfo("IMG_Context_Priority doesn't seem to be present.");

                m_contextPriority = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
            }


            const EGLint attrib_list[] =
                    {
                            EGL_WIDTH, 16,
                            EGL_HEIGHT, 16,
                            EGL_NONE
                    };
            m_eglPbufferSurface = eglCreatePbufferSurface( m_eglDisplay, m_eglConfig, attrib_list );
            if ( m_eglPbufferSurface == EGL_NO_SURFACE )
            {
                vFatal("eglCreatePbufferSurface failed: " << glOperation.getEglErrorString());
            }

            if ( eglMakeCurrent( m_eglDisplay, m_eglPbufferSurface, m_eglPbufferSurface,
                                 m_eglShareContext ) == EGL_FALSE )
            {
                vFatal("eglMakeCurrent: eglMakeCurrent pbuffer failed");
            }
            m_shutdownRequest.setState( false );
            pthread_mutex_lock( &m_smoothMutex );
            const int createErr = pthread_create( &m_smoothThread, NULL /* default attributes */, &ThreadStarter, this );
            if ( createErr != 0 )
            {
                vFatal("pthread_create returned " << createErr);
            }
            pthread_cond_wait( &m_smoothIslocked, &m_smoothMutex );
            pthread_mutex_unlock( &m_smoothMutex );
        }

        vInfo("----------------- VFrameSmooth() End -----------------");
    }

    void destroy()
    {
    vInfo("---------------- ~VFrameSmooth() Start ----------------");
        if ( m_smoothThread != 0 )
        {


            m_shutdownRequest.setState( true );

            void * data;
            pthread_join( m_smoothThread, &data );

            m_smoothThread = 0;



            VGlOperation glOperation;
            if ( eglGetCurrentSurface( EGL_DRAW ) != m_eglPbufferSurface )
            {
                vInfo("eglGetCurrentSurface( EGL_DRAW ) != eglPbufferSurface");
            }
            if ( eglMakeCurrent( m_eglDisplay, m_eglMainThreadSurface,
                                 m_eglMainThreadSurface, m_eglShareContext ) == EGL_FALSE)
            {
                vFatal("eglMakeCurrent to window failed: " << glOperation.getEglErrorString());
            }
            if ( EGL_FALSE == eglDestroySurface( m_eglDisplay, m_eglPbufferSurface ) )
            {
                vWarn("Failed to destroy pbuffer.");
            }
            else
            {
                vInfo("Destroyed pbuffer.");
            }
        }
        else
        {

            destroyFrameworkGraphics();
        }

        vInfo("---------------- ~VFrameSmooth() End ----------------");
    }
    static void *	ThreadStarter( void * parm );

    void			threadFunction();
    void 			smoothThreadInit();
    void			smoothThreadShutdown();
    void			smoothInternal( const ovrTimeWarpParms & parms );
    void            smoothInternal();
    void			buildSmoothProgPair( ovrTimeWarpProgram simpleIndex,
                                       const char * simpleVertex, const char * simpleFragment,
                                       const char * chromaticVertex, const char * chromaticFragment );

    void			buildSmoothProgMatchedPair( ovrTimeWarpProgram simpleIndex,
                                            const char * vertex, const char * fragment );
    void 			buildSmoothProgs();
    void			createFrameworkGraphics();
    void			destroyFrameworkGraphics();
    void			drawFrameworkGraphicsToWindow( const int eye, const int swapOptions);

    // 平滑的参数：
     bool m_async;
     bool testc;
     ovrTimeWarpImage 			m_images[2][3];
     int 						m_smoothOptions;
     VR4Matrixf					m_externalVelocity;
     int							m_minimumVsyncs;
     float						m_preScheduleSeconds;
     ovrTimeWarpProgram			m_smoothProgram;
     float						m_programParms[4];



     long long			m_minimumVsync;
     long long			m_firstDisplayedVsync[2];
     bool				m_disableChromaticCorrection;
     EGLSyncKHR			m_gpuSync;
    //平滑参数结束；
    VDevice *m_device;

    VGlShader		m_untexturedMvpProgram;
    VGlShader		m_debugLineProgram;
    VGlShader		m_warpPrograms[ WP_PROGRAM_MAX ];
    GLuint			m_blackTexId;
    GLuint			m_defaultLoadingIconTexId;
    VGlGeometry	m_calibrationLines2;		// simple cross
    VGlGeometry	m_smoothMesh;
    VGlGeometry	m_slicesmoothMesh;
    VGlGeometry	m_cursorMesh;

    static const int NUM_SLICES_PER_EYE = 4;
    static const int NUM_SLICES_PER_SCREEN = NUM_SLICES_PER_EYE*2;


    void			renderToDisplay( const double vsyncBase, const swapProgram_t & swap );
    void			renderToDisplayBySliced( const double vsyncBase, const swapProgram_t & swap );

    const VGlShader & chooseProgram(const bool disableChromaticCorrection ) const;
    void			setSmoothpState( ) const;
    void			bindSmoothProgram( const VR4Matrixf timeWarps[2][2],
                                     const VR4Matrixf rollingWarp, const int eye, const double vsyncBase ) const;
    void			bindCursorProgram() const;
    void            bindEyeTextures( const int eye );
    void            initRenderEnvironment();
    void            swapBuffers();

    int                m_window_width;
    int                m_window_height;
    EGLDisplay			m_window_display;
    EGLSurface			m_window_surface;



    bool			m_hasEXT_sRGB_write_control;


    pid_t			m_sStartupTid;


    JNIEnv *		m_jni;


    VLockless<double>		m_lastsmoothTimeInSeconds;


    EGLDisplay		m_eglDisplay;

    EGLSurface		m_eglPbufferSurface;
    EGLSurface		m_eglMainThreadSurface;
    EGLConfig		m_eglConfig;
    EGLint			m_eglClientVersion;
    EGLContext		m_eglShareContext;


    EGLContext		m_eglWarpContext;
    GLuint			m_contextPriority;

    static const int EYE_LOG_COUNT = 512;
    eyeLog_t		m_eyeLog[EYE_LOG_COUNT];
    long long		m_lastEyeLog;


    LogGpuTime<NUM_SLICES_PER_SCREEN>	m_logEyeWarpGpuTime;


    VLockless<bool>		m_shutdownRequest;

    pthread_t		m_smoothThread;
    int				m_smoothThreadTid;


    pthread_mutex_t m_smoothMutex;
    pthread_cond_t	m_smoothIslocked;
    VLockless<long long>			m_eyeBufferCount;


    VLockless<SwapState>		m_swapVsync;

    long long			m_lastSwapVsyncCount;
};
void VFrameSmooth::setSmoothEyeTexture(ushort i,ushort j,ovrTimeWarpImage m_images)

{

         {
           d->m_images[i][j].PlanarTexId[0] = m_images.PlanarTexId[0];
                   d->m_images[i][j].PlanarTexId[1] = m_images.PlanarTexId[1];
                    d->m_images[i][j].PlanarTexId[2] = m_images.PlanarTexId[2];
                    for(int m=0;m<4;m++)
                        for(int n=0;n<4;n++)
                        {
                    d->m_images[i][j].TexCoordsFromTanAngles.M[m][n] = m_images.TexCoordsFromTanAngles.M[m][n];
                        }
                     d->m_images[i][j].TexId = m_images.TexId;
                     d->m_images[i][j].Pose.AngularAcceleration.x =m_images.Pose.AngularAcceleration.x;
                     d->m_images[i][j].Pose.AngularAcceleration.y =m_images.Pose.AngularAcceleration.y;
                     d->m_images[i][j].Pose.AngularAcceleration.z =m_images.Pose.AngularAcceleration.z;
             d->m_images[i][j].Pose.AngularVelocity.x =m_images.Pose.AngularVelocity.x;
               d->m_images[i][j].Pose.AngularVelocity.y =m_images.Pose.AngularVelocity.y;
                 d->m_images[i][j].Pose.AngularVelocity.z =m_images.Pose.AngularVelocity.z;
             d->m_images[i][j].Pose.LinearAcceleration.x =m_images.Pose.LinearAcceleration.x;
            d->m_images[i][j].Pose.LinearAcceleration.y =m_images.Pose.LinearAcceleration.y;
            d->m_images[i][j].Pose.LinearAcceleration.z =m_images.Pose.LinearAcceleration.z;
             d->m_images[i][j].Pose.LinearVelocity.x =m_images.Pose.LinearVelocity.x;
               d->m_images[i][j].Pose.LinearVelocity.y =m_images.Pose.LinearVelocity.y;
                d->m_images[i][j].Pose.LinearVelocity.z =m_images.Pose.LinearVelocity.z;
               d->m_images[i][j].Pose.Pose.Orientation.w =m_images.Pose.Pose.Orientation.w;
               d->m_images[i][j].Pose.Pose.Orientation.x =m_images.Pose.Pose.Orientation.x;
               d->m_images[i][j].Pose.Pose.Orientation.y =m_images.Pose.Pose.Orientation.y;
               d->m_images[i][j].Pose.Pose.Orientation.z =m_images.Pose.Pose.Orientation.z;


               d->m_images[i][j].Pose.Pose.Position.x =m_images.Pose.Pose.Position.x;
               d->m_images[i][j].Pose.Pose.Position.y =m_images.Pose.Pose.Position.y;
              d->m_images[i][j].Pose.Pose.Position.z =m_images.Pose.Pose.Position.z;


               d->m_images[i][j].Pose.TimeInSeconds =m_images.Pose.TimeInSeconds;
        }
}
void VFrameSmooth::setSmoothEyeTexture(unsigned int texID,ushort eye,ushort layer)
{
     d->m_images[eye][layer].TexId =  texID;
}
void VFrameSmooth::setSmoothOption(int option)
{
    d->m_smoothOptions = option;

}
void VFrameSmooth::setMinimumVsncs( int vsnc)
{

    d->m_minimumVsyncs = vsnc;
}

void VFrameSmooth::setExternalVelocity(VR4Matrixf extV)

{
    for(int i=0;i<4;i++)
        for( int j=0;j<4;j++)
    {
 d->m_externalVelocity.M[i][j] = extV.M[i][j];


}
}
void VFrameSmooth::setPreScheduleSeconds(float pres)
{

    d->m_preScheduleSeconds = pres;

}
void VFrameSmooth::setSmoothProgram(ovrTimeWarpProgram program)
{

   d->m_smoothProgram = program;


}
void VFrameSmooth::setProgramParms( float * proParms)
{
   //d->m_programParms[0] = proParms[0];
  // d->m_programParms[1] = proParms[1];
  // d->m_programParms[2] = proParms[2];
  //  d->m_programParms[3] = proParms[3];
    for(int i=0;i<4;i++)
    {
       d->m_programParms[i]= proParms[i];

    }
}


void *VFrameSmooth::Private::ThreadStarter( void * parm )
{
    VFrameSmooth::Private & tw = *(VFrameSmooth::Private *)parm;
    tw.threadFunction();
    return NULL;
}

void VFrameSmooth::Private::threadFunction()
{

   LOG( "-------smooth sub thread start()" );
    smoothThreadInit();


    pthread_mutex_lock( &m_smoothMutex );
    pthread_cond_signal( &m_smoothIslocked );
    pthread_mutex_unlock( &m_smoothMutex );

    vInfo("WarpThreadLoop()");

    bool removedSchedFifo = false;


    for ( double vsync = 0; ; vsync++ )
    {

         LOG( "-------smooth sub thread main loop()" );
        const double current = ceil( GetFractionalVsync() );
        if ( abs( current - vsync ) > 2.0 )
        {
            vInfo("Changing vsync from " << vsync << " to " << current);
            vsync = current;
        }
        if ( m_shutdownRequest.state() )
        {
            vInfo("ShutdownRequest received");
            break;
        }


       const double currentTime = ovr_GetTimeInSeconds();
       const double lastWarpTime = m_lastsmoothTimeInSeconds.state();
       if ( removedSchedFifo )
       {
           if ( lastWarpTime > currentTime - 0.1 )
           {
               removedSchedFifo = false;
           }
       }
       else
       {
           if ( lastWarpTime < currentTime - 1.0 )
           {
               removedSchedFifo = true;
           }
       }
 LOG( "WarpThreadLoop enter rendertodisplay");
        renderToDisplay( vsync,spAsyncSwappedBufferPortrait);
    }

    smoothThreadShutdown();

    vInfo("Exiting WarpThreadLoop()");
}


VFrameSmooth::VFrameSmooth(bool async,VDevice *device)
        : d(new Private(async,device))
{

}

VFrameSmooth::~VFrameSmooth()
{
    d->destroy();
}

void VFrameSmooth::Private::smoothThreadInit()
{
    vInfo("WarpThreadInit()");

    pthread_setname_np( pthread_self(), "NervGear::VFrameSmooth" );



    EGLint contextAttribs[] =
    {
        EGL_CONTEXT_CLIENT_VERSION, m_eglClientVersion,
        EGL_NONE, EGL_NONE,
        EGL_NONE
    };

    if ( m_contextPriority != EGL_CONTEXT_PRIORITY_MEDIUM_IMG )
    {
        contextAttribs[2] = EGL_CONTEXT_PRIORITY_LEVEL_IMG;
        contextAttribs[3] = m_contextPriority;
    }

    VGlOperation glOperation;
    m_eglWarpContext = eglCreateContext( m_eglDisplay, m_eglConfig, m_eglShareContext, contextAttribs );
    if ( m_eglWarpContext == EGL_NO_CONTEXT )
    {
        vFatal("eglCreateContext failed: " << glOperation.getEglErrorString());
    }
    vInfo("eglWarpContext: " << m_eglWarpContext);
    if ( m_contextPriority != EGL_CONTEXT_PRIORITY_MEDIUM_IMG )
    {
        // See what context priority we actually got
        EGLint actualPriorityLevel;
        eglQueryContext( m_eglDisplay, m_eglWarpContext, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &actualPriorityLevel );
        switch ( actualPriorityLevel )
        {
        case EGL_CONTEXT_PRIORITY_HIGH_IMG: vInfo("Context is EGL_CONTEXT_PRIORITY_HIGH_IMG"); break;
        case EGL_CONTEXT_PRIORITY_MEDIUM_IMG: vInfo("Context is EGL_CONTEXT_PRIORITY_MEDIUM_IMG"); break;
        case EGL_CONTEXT_PRIORITY_LOW_IMG: vInfo("Context is EGL_CONTEXT_PRIORITY_LOW_IMG"); break;
        default: vInfo("Context has unknown priority level"); break;
        }
    }


    vInfo("eglMakeCurrent on " << m_eglMainThreadSurface);

    if ( eglMakeCurrent( m_eglDisplay, m_eglMainThreadSurface,
                         m_eglMainThreadSurface, m_eglWarpContext ) == EGL_FALSE )
    {
        vFatal("eglMakeCurrent failed: " << glOperation.getEglErrorString());
    }

    initRenderEnvironment();
    createFrameworkGraphics();
    m_smoothThreadTid = gettid();

    vInfo("WarpThreadInit() - End");
}

void VFrameSmooth::Private::smoothThreadShutdown()
{

    vInfo("smoothThreadShutdown()");


    destroyFrameworkGraphics();
    VGlOperation glOperation;
    for ( int i = 0; i < 1; i++ )
    {
        //warpSource_t & ws = m_warpSources[i];
        if ( m_gpuSync )
        {
            if ( EGL_FALSE == glOperation.eglDestroySyncKHR( m_eglDisplay, m_gpuSync) )
            {
                vInfo("eglDestroySyncKHR returned EGL_FALSE");
            }
            m_gpuSync = 0;
        }
    }


    if ( eglMakeCurrent( m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE,
                         EGL_NO_CONTEXT ) == EGL_FALSE )
    {
        vFatal("eglMakeCurrent: shutdown failed");
    }

    if ( eglDestroyContext( m_eglDisplay, m_eglWarpContext ) == EGL_FALSE )
    {
        vFatal("eglDestroyContext: shutdown failed");
    }
    m_eglWarpContext = 0;


    vInfo("WarpThreadShutdown() - End");
}

const VGlShader & VFrameSmooth::Private::chooseProgram( const bool disableChromaticCorrection ) const
{
    int program = VAlgorithm::Clamp( (int)m_smoothProgram, (int)WP_SIMPLE, (int)WP_PROGRAM_MAX - 1 );

    if ( disableChromaticCorrection && program >= WP_CHROMATIC )
    {
        program -= ( WP_CHROMATIC - WP_SIMPLE );
    }
    return m_warpPrograms[program];
}
void VFrameSmooth::Private::setSmoothpState( ) const
{
    glDepthMask( GL_FALSE );	// don't write to depth, even if Unity has depth on window
    glDisable( GL_DEPTH_TEST );
    glDisable( GL_CULL_FACE );
    glDisable( GL_BLEND );
    glEnable( GL_SCISSOR_TEST );


    if ( m_hasEXT_sRGB_write_control )
    {
        if ( m_smoothOptions & SWAP_OPTION_INHIBIT_SRGB_FRAMEBUFFER )
        {
            glDisable( VGlOperation::GL_FRAMEBUFFER_SRGB_EXT );
        }
        else
        {
            glEnable( VGlOperation::GL_FRAMEBUFFER_SRGB_EXT );
        }
    }
    VGlOperation glOperation;
    glOperation.logErrorsEnum( "SetWarpState" );
}

void VFrameSmooth::Private::bindSmoothProgram( const VR4Matrixf timeWarps[2][2], const VR4Matrixf rollingWarp,
                                     const int eye, const double vsyncBase /* for spinner */ ) const
{
     const VR4Matrixf landscapeOrientationMatrix(
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f );

    // Select the warp program.
    const VGlShader & warpProg = chooseProgram(m_disableChromaticCorrection );
    glUseProgram( warpProg.program );

    // Set the shader parameters.
    glUniform1f( warpProg.uniformColor, m_programParms[0] );

    glUniformMatrix4fv( warpProg.uniformModelViewProMatrix, 1, GL_FALSE, landscapeOrientationMatrix.Transposed().M[0] );
    glUniformMatrix4fv( warpProg.uniformTexMatrix, 1, GL_FALSE, timeWarps[0][0].Transposed().M[0] );
    glUniformMatrix4fv( warpProg.uniformTexMatrix2, 1, GL_FALSE, timeWarps[0][1].Transposed().M[0] );
    if ( warpProg.uniformTexMatrix3 > 0 )
    {
        glUniformMatrix4fv( warpProg.uniformTexMatrix3, 1, GL_FALSE, timeWarps[1][0].Transposed().M[0] );
        glUniformMatrix4fv( warpProg.uniformTexMatrix4, 1, GL_FALSE, timeWarps[1][1].Transposed().M[0] );
    }
    if ( warpProg.uniformTexMatrix5 > 0 )
    {
        glUniformMatrix4fv( warpProg.uniformTexMatrix5, 1, GL_FALSE, rollingWarp.Transposed().M[0] );
    }
    if ( warpProg.uniformTexClamp > 0 )
    {
        // split screen clamping for UE4
        const V2Vectf clamp( eye * 0.5f, (eye+1)* 0.5f );
        glUniform2fv( warpProg.uniformTexClamp, 1, &clamp.x );
    }
    if ( warpProg.uniformRotateScale > 0 )
    {
        const float angle = FramePointTimeInSeconds( vsyncBase ) * M_PI * m_programParms[0];
        const V4Vectf RotateScale( sinf( angle ), cosf( angle ), m_programParms[1], 1.0f );
        glUniform4fv( warpProg.uniformRotateScale, 1, &RotateScale[0] );
    }
}

void VFrameSmooth::Private::bindCursorProgram() const
{
    const VR4Matrixf landscapeOrientationMatrix(
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f );

    const VGlShader & warpProg = m_warpPrograms[ WP_SIMPLE ];
    glUseProgram( warpProg.program );
    glUniform1f( warpProg.uniformColor, 1.0f );
    glUniformMatrix4fv( warpProg.uniformModelViewProMatrix, 1, GL_FALSE, landscapeOrientationMatrix.Transposed().M[0] );
    glUniformMatrix4fv( warpProg.uniformTexMatrix, 1, GL_FALSE, VR4Matrixf::Identity().M[0] );
    glUniformMatrix4fv( warpProg.uniformTexMatrix2, 1, GL_FALSE, VR4Matrixf::Identity().M[0] );
}

int CameraTimeWarpLatency = 4;
bool CameraTimeWarpPause;

void VFrameSmooth::Private::bindEyeTextures( const int eye )
{
    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( GL_TEXTURE_2D, m_images[eye][0].TexId );
    if ( m_smoothOptions & SWAP_OPTION_INHIBIT_SRGB_FRAMEBUFFER )
    {
        glTexParameteri( GL_TEXTURE_2D, VGlOperation::GL_TEXTURE_SRGB_DECODE_EXT, VGlOperation::GL_SKIP_DECODE_EXT );
    }
    else
    {
        glTexParameteri( GL_TEXTURE_2D, VGlOperation::GL_TEXTURE_SRGB_DECODE_EXT, VGlOperation::GL_DECODE_EXT );
    }

    if ( m_smoothProgram == WP_MASKED_PLANE
         || m_smoothProgram == WP_CHROMATIC_MASKED_PLANE
         || m_smoothProgram == WP_OVERLAY_PLANE
         || m_smoothProgram == WP_CHROMATIC_OVERLAY_PLANE
         || m_smoothProgram == WP_OVERLAY_PLANE_SHOW_LOD
         || m_smoothProgram == WP_CHROMATIC_OVERLAY_PLANE_SHOW_LOD
         )
    {
        glActiveTexture( GL_TEXTURE1 );
        glBindTexture( GL_TEXTURE_2D, m_images[eye][1].TexId );
        if ( m_smoothOptions & SWAP_OPTION_INHIBIT_SRGB_FRAMEBUFFER )
        {
            glTexParameteri( GL_TEXTURE_2D, VGlOperation::GL_TEXTURE_SRGB_DECODE_EXT, VGlOperation::GL_SKIP_DECODE_EXT );
        }
        else
        {
            glTexParameteri( GL_TEXTURE_2D, VGlOperation::GL_TEXTURE_SRGB_DECODE_EXT, VGlOperation::GL_DECODE_EXT );
        }
    }
    if ( m_smoothProgram == WP_MASKED_PLANE_EXTERNAL
         || m_smoothProgram == WP_CHROMATIC_MASKED_PLANE_EXTERNAL
         || m_smoothProgram == WP_CAMERA
         || m_smoothProgram == WP_CHROMATIC_CAMERA )
    {
        glActiveTexture( GL_TEXTURE1 );
        glBindTexture( GL_TEXTURE_EXTERNAL_OES, m_images[eye][1].TexId );

            if ( m_smoothOptions & SWAP_OPTION_INHIBIT_SRGB_FRAMEBUFFER )
            {
                glTexParameteri( GL_TEXTURE_EXTERNAL_OES, VGlOperation::GL_TEXTURE_SRGB_DECODE_EXT, VGlOperation::GL_SKIP_DECODE_EXT );
            }
            else
            {
                glTexParameteri( GL_TEXTURE_EXTERNAL_OES, VGlOperation::GL_TEXTURE_SRGB_DECODE_EXT, VGlOperation::GL_DECODE_EXT );
            }
    }
    if ( m_smoothProgram == WP_MASKED_CUBE || m_smoothProgram == WP_CHROMATIC_MASKED_CUBE )
    {
        glActiveTexture( GL_TEXTURE1 );
        glBindTexture( GL_TEXTURE_CUBE_MAP, m_images[eye][1].TexId );
            if ( m_smoothOptions & SWAP_OPTION_INHIBIT_SRGB_FRAMEBUFFER )
            {
                glTexParameteri( GL_TEXTURE_CUBE_MAP, VGlOperation::GL_TEXTURE_SRGB_DECODE_EXT, VGlOperation::GL_SKIP_DECODE_EXT );
            }
            else
            {
                glTexParameteri( GL_TEXTURE_CUBE_MAP, VGlOperation::GL_TEXTURE_SRGB_DECODE_EXT, VGlOperation::GL_DECODE_EXT );
            }
    }

    if ( m_smoothProgram == WP_CUBE || m_smoothProgram == WP_CHROMATIC_CUBE )
    {
        for ( int i = 0; i < 3; i++ )
        {
            glActiveTexture( GL_TEXTURE1 + i );
            glBindTexture( GL_TEXTURE_CUBE_MAP, m_images[eye][1].PlanarTexId[i] );
                if ( m_smoothOptions & SWAP_OPTION_INHIBIT_SRGB_FRAMEBUFFER )
                {
                    glTexParameteri( GL_TEXTURE_CUBE_MAP, VGlOperation::GL_TEXTURE_SRGB_DECODE_EXT, VGlOperation::GL_SKIP_DECODE_EXT );
                }
                else
                {
                    glTexParameteri( GL_TEXTURE_CUBE_MAP, VGlOperation::GL_TEXTURE_SRGB_DECODE_EXT, VGlOperation::GL_DECODE_EXT );
                }
        }
    }

    if ( m_smoothProgram == WP_LOADING_ICON || m_smoothProgram == WP_CHROMATIC_LOADING_ICON )
    {
        glActiveTexture( GL_TEXTURE1 );
        glBindTexture( GL_TEXTURE_2D, m_images[eye][1].TexId );
            if ( m_smoothOptions & SWAP_OPTION_INHIBIT_SRGB_FRAMEBUFFER )
            {
                glTexParameteri( GL_TEXTURE_2D, VGlOperation::GL_TEXTURE_SRGB_DECODE_EXT, VGlOperation::GL_SKIP_DECODE_EXT );
            }
            else
            {
                glTexParameteri( GL_TEXTURE_2D, VGlOperation::GL_TEXTURE_SRGB_DECODE_EXT, VGlOperation::GL_DECODE_EXT );
            }
    }
}

static void UnbindEyeTextures()
{
    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( GL_TEXTURE_2D, 0 );

    glActiveTexture( GL_TEXTURE1 );
    glBindTexture( GL_TEXTURE_2D, 0 );
    glBindTexture( GL_TEXTURE_EXTERNAL_OES, 0 );
    glBindTexture( GL_TEXTURE_CUBE_MAP, 0 );

    for ( int i = 1; i < 3; i++ )
    {
        glActiveTexture( GL_TEXTURE1 + i );
        glBindTexture( GL_TEXTURE_CUBE_MAP, 0 );
    }
}

void VFrameSmooth::Private::renderToDisplay( const double vsyncBase_, const swapProgram_t & swap )
{
    static double lastReportTime = 0;
    const double timeNow = floor( ovr_GetTimeInSeconds() );
    if ( timeNow > lastReportTime )
    {

        vInfo(" GPU time: " << m_logEyeWarpGpuTime.totalTime() << " ms");

        lastReportTime = timeNow;
    }

    if ( m_smoothOptions & SWAP_OPTION_USE_SLICED_WARP )
    {
        renderToDisplayBySliced( vsyncBase_, swap );
        return;
    }

    const double vsyncBase = vsyncBase_;



    glViewport( 0, 0, m_window_width, m_window_height );
    glScissor( 0, 0, m_window_width, m_window_height );

    VGlOperation glOperation;

    for ( int eye = 0; eye <= 1; ++eye )
    {

        //vInfo("Eye " << eye << ": now=" << GetFractionalVsync() << "  sleepTo=" << vsyncBase + swap.deltaVsync[eye]);


        const double sleepTargetVsync = vsyncBase + swap.deltaVsync[eye];

//        const double sleepTargetTime = FramePointTimeInSeconds( sleepTargetVsync );
        //const float secondsToSleep = SleepUntilTimePoint( sleepTargetTime, false );
//        const double preFinish = ovr_GetTimeInSeconds();

        //vInfo("Vsync " << vsyncBase << ":" << eye << " sleep " << secondsToSleep);


        long long thisEyeBufferNum = 0;
        int	back;

        if ( eye == 0 )
        {
            const long long latestEyeBufferNum = m_eyeBufferCount.state();
            for ( back = 0; back < 3; back++ )
            {
                thisEyeBufferNum = latestEyeBufferNum - back;
                if ( thisEyeBufferNum <= 0 )
                {

                    vInfo("WarpToScreen: No valid Eye Buffers");

                    break;
                }

                if ( m_minimumVsync > vsyncBase )
                {

                    continue;
                }
                if ( m_gpuSync == 0 )
                {
                    vInfo("thisEyeBufferNum " << thisEyeBufferNum << " had 0 sync");
                    break;
                }

                if ( VQuatf( m_images[eye][0].Pose.Pose.Orientation ).LengthSq() < 1e-18f )
                {
                    vInfo("Bad Pose.Orientation in bufferNum " << thisEyeBufferNum << "!");
                    break;
                }

                const EGLint wait = glOperation.eglClientWaitSyncKHR( m_eglDisplay, m_gpuSync,
                                                                      EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 0 );
                if ( wait == EGL_TIMEOUT_EXPIRED_KHR )
                {
                    continue;
                }
                if ( wait == EGL_FALSE )
                {
                    vInfo("eglClientWaitSyncKHR returned EGL_FALSE");
                }


                if ( m_firstDisplayedVsync[eye] == 0 )
                {
                    m_firstDisplayedVsync[eye] = (long long)vsyncBase;
                }

                break;
            }


            {
                SwapState	state;
                state.VsyncCount = (long long)vsyncBase;
                state.EyeBufferCount = thisEyeBufferNum;
                m_swapVsync.setState( state );


                if ( !pthread_mutex_trylock( &m_smoothMutex ) )
                {
                    pthread_cond_signal( &m_smoothIslocked );
                    pthread_mutex_unlock( &m_smoothMutex );
                }
            }

            if ( m_images[eye][0].TexId == 0 )
            {

                vInfo("WarpToScreen: Nothing valid to draw");

                SleepUntilTimePoint( FramePointTimeInSeconds( sleepTargetVsync + 1.0f ), false );
                break;
            }
        }


        VR4Matrixf velocity;
        const int velocitySteps = std::min( 3, (int)((long long)vsyncBase - m_minimumVsync) );
        for ( int i = 0; i < velocitySteps; i++ )
        {
            velocity = velocity * m_externalVelocity;
        }


        const bool dualLayer = ( m_images[eye][1].TexId > 0 );


        VR4Matrixf timeWarps[2][2];
        ovrSensorState sensor[2];
        for ( int scan = 0; scan < 2; scan++ )
        {
            const double vsyncPoint = vsyncBase + swap.predictionPoints[eye][scan];
            const double timePoint = FramePointTimeInSeconds( vsyncPoint );
            sensor[scan] = ovr_GetSensorStateInternal( timePoint );
            const VR4Matrixf warp = CalculateTimeWarpMatrix2(
                        m_images[eye][0].Pose.Pose.Orientation,
                    sensor[scan].Predicted.Pose.Orientation ) * velocity;
            timeWarps[0][scan] = VR4Matrixf( m_images[eye][0].TexCoordsFromTanAngles ) * warp;
            if ( dualLayer )
            {
                if ( m_smoothOptions & SWAP_OPTION_FIXED_OVERLAY )
                {
                    timeWarps[1][scan] = VR4Matrixf( m_images[eye][1].TexCoordsFromTanAngles );
                }
                else
                {
                    const VR4Matrixf warp2 = CalculateTimeWarpMatrix2(
                                m_images[eye][1].Pose.Pose.Orientation,
                            sensor[scan].Predicted.Pose.Orientation ) * velocity;
                    timeWarps[1][scan] = VR4Matrixf( m_images[eye][1].TexCoordsFromTanAngles ) * warp2;
                }
            }
        }


        const VR4Matrixf rollingWarp = CalculateTimeWarpMatrix2(
                    sensor[0].Predicted.Pose.Orientation,
                sensor[1].Predicted.Pose.Orientation );



        m_logEyeWarpGpuTime.begin( eye );
        m_logEyeWarpGpuTime.printTime( eye, "GPU time for eye time warp" );

        setSmoothpState( );

        bindSmoothProgram( timeWarps, rollingWarp, eye, vsyncBase );

        bindEyeTextures( eye );

        glScissor(eye * m_window_width/2, 0, m_window_width/2, m_window_height);


        glOperation.glBindVertexArrayOES( m_smoothMesh.vertexArrayObject );
        const int indexCount = m_smoothMesh.indexCount / 2;
        const int indexOffset = eye * indexCount;
        glDrawElements( GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, (void *)(indexOffset * 2 ) );


        if ( m_smoothOptions & SWAP_OPTION_SHOW_CURSOR )
        {
            bindCursorProgram();
            glEnable( GL_BLEND );
            glOperation.glBindVertexArrayOES( m_cursorMesh.vertexArrayObject );
            const int indexCount = m_cursorMesh.indexCount / 2;
            const int indexOffset = eye * indexCount;
            glDrawElements( GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, (void *)(indexOffset * 2 ) );
            glDisable( GL_BLEND );
        }


        drawFrameworkGraphicsToWindow( eye, m_smoothOptions);

        glFlush();

        m_logEyeWarpGpuTime.end( eye );

        const double justBeforeFinish = ovr_GetTimeInSeconds();
        const double postFinish = ovr_GetTimeInSeconds();

        const float latency = postFinish - justBeforeFinish;
        if ( latency > 0.008f )
        {
            vInfo("Frame " << (int)vsyncBase << " Eye " << eye << " latency " << latency);
        }
    }

    UnbindEyeTextures();

    glUseProgram( 0 );

    glOperation.glBindVertexArrayOES( 0 );

    swapBuffers();
}

void VFrameSmooth::Private::renderToDisplayBySliced( const double vsyncBase, const swapProgram_t & swap )
{

    const VsyncState vsyncState = UpdatedVsyncState.state();
    if ( vsyncState.vsyncBaseNano == 0 )
    {
        return;
    }

    VGlOperation glOperation;

    double	sliceTimes[NUM_SLICES_PER_SCREEN+1];

    static const double startBias = 0.0;
    static const double activeFraction = 112.0 / 135;
    for ( int i = 0; i <= NUM_SLICES_PER_SCREEN; i++ )
    {
        const double framePoint = vsyncBase + activeFraction * (float)i / NUM_SLICES_PER_SCREEN;
        sliceTimes[i] = ( vsyncState.vsyncBaseNano +
                          ( framePoint - vsyncState.vsyncCount ) * vsyncState.vsyncPeriodNano )
                * 0.000000001 + startBias;
    }

    glViewport( 0, 0, m_window_width, m_window_height );
    glScissor( 0, 0, m_window_width, m_window_height );

    int	back = 0;

    long long thisEyeBufferNum = 0;
    for ( int screenSlice = 0; screenSlice < NUM_SLICES_PER_SCREEN; screenSlice++ )
    {
        const int	eye = (int)( screenSlice / NUM_SLICES_PER_EYE );



        //vInfo("slice " << screenSlice << " targ " << sleepTargetTime << " slept " << secondsToSleep);

        if ( screenSlice == 0 )
        {
            const long long latestEyeBufferNum = m_eyeBufferCount.state();
            for ( back = 0; back < 3; back++ )
            {
                thisEyeBufferNum = latestEyeBufferNum - back;
                if ( thisEyeBufferNum <= 0 )
                {

                    vInfo("WarpToScreen: No valid Eye Buffers");
                    break;
                }


                if ( m_minimumVsync > vsyncBase )
                {
                    // a full frame got completed in less time than a single eye; don't use it to avoid stuttering
                    continue;
                }

                if ( m_gpuSync == 0 )
                {
                    vInfo("thisEyeBufferNum " << thisEyeBufferNum << " had 0 sync");
                    break;
                }

                if ( VQuatf( m_images[eye][0].Pose.Pose.Orientation ).LengthSq() < 1e-18f )
                {
                    vInfo("Bad Predicted.Pose.Orientation!");
                    continue;
                }

                const EGLint wait = glOperation.eglClientWaitSyncKHR( m_eglDisplay, m_gpuSync,
                                                                      EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 0 );
                if ( wait == EGL_TIMEOUT_EXPIRED_KHR )
                {
                    continue;
                }
                if ( wait == EGL_FALSE )
                {
                    vInfo("eglClientWaitSyncKHR returned EGL_FALSE");
                }


                if ( m_firstDisplayedVsync[eye] == 0 )
                {
                    m_firstDisplayedVsync[eye] = (long long)vsyncBase;
                }

                break;
            }


            if ( screenSlice == 0 )
            {
                SwapState	state;
                state.VsyncCount = (long long)vsyncBase;
                state.EyeBufferCount = thisEyeBufferNum;
                m_swapVsync.setState( state );


                if ( !pthread_mutex_trylock( &m_smoothMutex ) )
                {
                    pthread_cond_signal( &m_smoothIslocked );
                    pthread_mutex_unlock( &m_smoothMutex );
                }
            }

            if ( m_images[eye][0].TexId == 0 )
            {

                vInfo("WarpToScreen: Nothing valid to draw");

                SleepUntilTimePoint( FramePointTimeInSeconds( vsyncBase + 1.0f ), false );
                break;
            }
        }


        VR4Matrixf velocity;
        const int velocitySteps = std::min( 3, (int)((long long)vsyncBase - m_minimumVsync) );
        for ( int i = 0; i < velocitySteps; i++ )
        {
            velocity = velocity * m_externalVelocity;
        }

        const bool dualLayer = ( m_images[eye][1].TexId > 0 );


        VR4Matrixf timeWarps[2][2];
        static ovrSensorState sensor[2];
        for ( int scan = 0; scan < 2; scan++ )
        {

            static VR4Matrixf	warp;
            if ( scan == 1 || screenSlice == 0 || screenSlice == NUM_SLICES_PER_EYE )
            {
                const double timePoint = sliceTimes[screenSlice + scan];
                sensor[scan] = ovr_GetSensorStateInternal( timePoint );
                warp = CalculateTimeWarpMatrix2(
                            m_images[eye][0].Pose.Pose.Orientation,
                        sensor[scan].Predicted.Pose.Orientation ) * velocity;
            }
            timeWarps[0][scan] = VR4Matrixf( m_images[eye][0].TexCoordsFromTanAngles ) * warp;
            if ( dualLayer )
            {
                if ( m_smoothOptions & SWAP_OPTION_FIXED_OVERLAY )
                {
                    timeWarps[1][scan] = VR4Matrixf( m_images[eye][1].TexCoordsFromTanAngles );
                }
                else
                {
                    const VR4Matrixf warp2 = CalculateTimeWarpMatrix2(
                                m_images[eye][1].Pose.Pose.Orientation,
                            sensor[scan].Predicted.Pose.Orientation ) * velocity;
                    timeWarps[1][scan] = VR4Matrixf( m_images[eye][1].TexCoordsFromTanAngles ) * warp2;
                }
            }
        }

        const VR4Matrixf rollingWarp = CalculateTimeWarpMatrix2(
                    sensor[0].Predicted.Pose.Orientation,
                sensor[1].Predicted.Pose.Orientation );


        m_logEyeWarpGpuTime.begin( screenSlice );
        m_logEyeWarpGpuTime.printTime( screenSlice, "GPU time for eye time warp" );

        setSmoothpState();

        bindSmoothProgram( timeWarps, rollingWarp, eye, vsyncBase );

        if ( screenSlice == 0 || screenSlice == NUM_SLICES_PER_EYE )
        {
            bindEyeTextures( eye );
        }

        const int sliceSize = m_window_width / NUM_SLICES_PER_SCREEN;

        glScissor( sliceSize*screenSlice, 0, sliceSize, m_window_height );

        const VGlGeometry & mesh = m_slicesmoothMesh;
        glOperation.glBindVertexArrayOES( mesh.vertexArrayObject );
        const int indexCount = mesh.indexCount / NUM_SLICES_PER_SCREEN;
        const int indexOffset = screenSlice * indexCount;
        glDrawElements( GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, (void *)(indexOffset * 2 ) );

        if ( 0 )
        {
            const int cycleColor = (int)vsyncBase + screenSlice;
            glClearColor( cycleColor & 1, ( cycleColor >> 1 ) & 1, ( cycleColor >> 2 ) & 1, 1 );
            glClear( GL_COLOR_BUFFER_BIT );
        }


        drawFrameworkGraphicsToWindow( eye, m_smoothOptions);

        glFlush();

        m_logEyeWarpGpuTime.end( screenSlice );

        const double justBeforeFinish = ovr_GetTimeInSeconds();
        const double postFinish = ovr_GetTimeInSeconds();

        const float latency = postFinish - justBeforeFinish;
        if ( latency > 0.008f )
        {
            vInfo("Frame " << (int)vsyncBase << " Eye " << eye << " latency " << latency);
        }
    }

    UnbindEyeTextures();

    glUseProgram( 0 );

    glOperation.glBindVertexArrayOES( 0 );

    swapBuffers();
}

static uint64_t GetNanoSecondsUint64()
{
    struct timespec now;
    clock_gettime( CLOCK_MONOTONIC, &now );
    return (uint64_t) now.tv_sec * 1000000000LL + now.tv_nsec;
}

void VFrameSmooth::Private::smoothInternal( )
{
    if ( gettid() != m_sStartupTid )
    {
        vFatal("WarpSwap: Called with tid " << gettid() << " instead of " << m_sStartupTid);
    }

    m_lastsmoothTimeInSeconds.setState( ovr_GetTimeInSeconds() );


    glBindFramebuffer( GL_FRAMEBUFFER, 0 );


    VGlOperation glOperation;

    const long long lastBufferCount = m_eyeBufferCount.state();

    m_minimumVsync = m_lastSwapVsyncCount + 2 * m_minimumVsyncs;	// don't use it if from same frame to avoid problems with very fast frames
    m_firstDisplayedVsync[0] = 0;			// will be set when it becomes the currentSource
    m_firstDisplayedVsync[1] = 0;			// will be set when it becomes the currentSource
    m_disableChromaticCorrection = ( ( glOperation.eglGetGpuType() & NervGear::VGlOperation::GPU_TYPE_MALI_T760_EXYNOS_5433 ) != 0 );

    if ( ( m_smoothProgram & SWAP_OPTION_DEFAULT_IMAGES ) != 0 )
    {
        for ( int eye = 0; eye < 2; eye++ )
        {
            if ( m_images[eye][0].TexId == 0 )
            {
                m_images[eye][0].TexId = m_blackTexId;
            }
            if ( m_images[eye][1].TexId == 0 )
            {
                m_images[eye][1].TexId = m_defaultLoadingIconTexId;
            }
        }
    }


    if ( m_gpuSync != EGL_NO_SYNC_KHR )
    {
        if ( EGL_FALSE == glOperation.eglDestroySyncKHR( m_eglDisplay, m_gpuSync ) )
        {
            LOG( "eglDestroySyncKHR returned EGL_FALSE" );
        }
    }


    m_gpuSync = glOperation.eglCreateSyncKHR( m_eglDisplay, EGL_SYNC_FENCE_KHR, NULL );
    if ( m_gpuSync == EGL_NO_SYNC_KHR )
    {
        FAIL( "eglCreateSyncKHR_():EGL_NO_SYNC_KHR" );
    }


    if ( EGL_FALSE == glOperation.eglClientWaitSyncKHR( m_eglDisplay, m_gpuSync,
                                                        EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 0 ) )
    {
        LOG( "eglClientWaitSyncKHR returned EGL_FALSE" );
    }


    m_eyeBufferCount.setState( lastBufferCount + 1 );


    if ( !m_async )
    {

        VGlOperation glOperation;
        glOperation.glFinish();

        swapProgram_t * swapProg;
        swapProg = &spSyncSwappedBufferPortrait;

        renderToDisplay( floor( GetFractionalVsync() ), *swapProg );

        const SwapState state = m_swapVsync.state();
        m_lastSwapVsyncCount = state.VsyncCount;

        return;
    }

    for ( ; ; )
    {
        const uint64_t startSuspendNanoSeconds = GetNanoSecondsUint64();


        pthread_mutex_lock( &m_smoothMutex );

        pthread_cond_wait( &m_smoothIslocked, &m_smoothMutex );

        pthread_mutex_unlock( &m_smoothMutex );

        const uint64_t endSuspendNanoSeconds = GetNanoSecondsUint64();

        const SwapState state = m_swapVsync.state();
        if ( state.EyeBufferCount >= lastBufferCount )
        {

            m_lastSwapVsyncCount = std::max( state.VsyncCount, m_lastSwapVsyncCount + m_minimumVsyncs );

            const uint64_t suspendNanoSeconds = endSuspendNanoSeconds - startSuspendNanoSeconds;
            if ( suspendNanoSeconds < 1000 * 1000 )
            {
                const uint64_t suspendMicroSeconds = ( 1000 * 1000 - suspendNanoSeconds ) / 1000;
                LOG( "WarpSwap: usleep( %lld )", suspendMicroSeconds );
                usleep( suspendMicroSeconds );
            }
            return;
        }
    }
}




void VFrameSmooth::Private::smoothInternal( const ovrTimeWarpParms & parms )
{
    if ( gettid() != m_sStartupTid )
    {
        FAIL( "WarpSwap: Called with tid %i instead of %i", gettid(), m_sStartupTid );
    }


    m_lastsmoothTimeInSeconds.setState( ovr_GetTimeInSeconds() );


    glBindFramebuffer( GL_FRAMEBUFFER, 0 );
    const int minimumVsyncs =  parms.MinimumVsyncs;

    VGlOperation glOperation;
        LOG( "smooth internal before eyebuffercount ");
    const long long lastBufferCount = m_eyeBufferCount.state();

     LOG( "smooth eye buffercount ( %lld )", lastBufferCount );

    m_minimumVsync = m_lastSwapVsyncCount + 2 * minimumVsyncs;
    m_firstDisplayedVsync[0] = 0;
    m_firstDisplayedVsync[1] = 0;
    m_disableChromaticCorrection = ( ( glOperation.eglGetGpuType() & NervGear::VGlOperation::GPU_TYPE_MALI_T760_EXYNOS_5433 ) != 0 );

    if ( ( m_smoothOptions & SWAP_OPTION_DEFAULT_IMAGES ) != 0 )
    {
        for ( int eye = 0; eye < 2; eye++ )
        {
            if (m_images[eye][0].TexId == 0 )
            {
                m_images[eye][0].TexId = m_blackTexId;
            }
            if ( m_images[eye][1].TexId == 0 )
            {
                m_images[eye][1].TexId = m_defaultLoadingIconTexId;
            }
        }
    }

    if ( m_gpuSync != EGL_NO_SYNC_KHR )
    {
        if ( EGL_FALSE == glOperation.eglDestroySyncKHR( m_eglDisplay, m_gpuSync ) )
        {
            vInfo("eglDestroySyncKHR returned EGL_FALSE");
        }
    }

    m_gpuSync = glOperation.eglCreateSyncKHR( m_eglDisplay, EGL_SYNC_FENCE_KHR, NULL );
    if ( m_gpuSync == EGL_NO_SYNC_KHR )
    {
        vFatal("eglCreateSyncKHR_():EGL_NO_SYNC_KHR");
    }

    if ( EGL_FALSE == glOperation.eglClientWaitSyncKHR( m_eglDisplay, m_gpuSync,
                                                        EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 0 ) )
    {
        vInfo("eglClientWaitSyncKHR returned EGL_FALSE");
    }


    m_eyeBufferCount.setState( lastBufferCount + 1 );

    if ( !m_async )
    {


        VGlOperation glOperation;
        glOperation.glFinish();

        swapProgram_t * swapProg;
        swapProg = &spSyncSwappedBufferPortrait;

        renderToDisplay( floor( GetFractionalVsync() ), *swapProg );

        const SwapState state = m_swapVsync.state();
        m_lastSwapVsyncCount = state.VsyncCount;

        return;
    }

    for ( ; ; )
    {
        const uint64_t startSuspendNanoSeconds = GetNanoSecondsUint64();


        pthread_mutex_lock( &m_smoothMutex );


        pthread_cond_wait( &m_smoothIslocked, &m_smoothMutex );


        pthread_mutex_unlock( &m_smoothMutex );

        const uint64_t endSuspendNanoSeconds = GetNanoSecondsUint64();

        const SwapState state = m_swapVsync.state();
        if ( state.EyeBufferCount >= lastBufferCount )
        {

            m_lastSwapVsyncCount = std::max( state.VsyncCount, m_lastSwapVsyncCount + minimumVsyncs );


            const uint64_t suspendNanoSeconds = endSuspendNanoSeconds - startSuspendNanoSeconds;
            if ( suspendNanoSeconds < 1000 * 1000 )
            {
                const uint64_t suspendMicroSeconds = ( 1000 * 1000 - suspendNanoSeconds ) / 1000;
                vInfo("WarpSwap: usleep( " << suspendMicroSeconds << " )");
                usleep( suspendMicroSeconds );
            }
            return;
        }
    }
}


void	VFrameSmooth::doSmooth()
{


    const int count = ( ( d->m_smoothOptions & SWAP_OPTION_FLUSH ) != 0 ) ? 3 : 1;
    for ( int i = 0; i < count; i++ )
    {


        d->smoothInternal();
    }

}
struct lineVert_t
{
    unsigned short	x, y;
    unsigned int	color;
};

int ColorAsInt( const int r, const int g, const int b, const int a )
{
    return r | (g<<8) | (b<<16) | (a<<24);
}

VGlGeometry CreateTimingGraphGeometry( const int lineVertCount )
{
    VGlGeometry geo;
    VGlOperation glOperation;

    glOperation.glGenVertexArraysOES( 1, &geo.vertexArrayObject );
    glOperation.glBindVertexArrayOES( geo.vertexArrayObject );

    lineVert_t	* verts = new lineVert_t[lineVertCount];
    const int byteCount = lineVertCount * sizeof( verts[0] );
    memset( verts, 0, byteCount );

    glGenBuffers( 1, &geo.vertexBuffer );
    glBindBuffer( GL_ARRAY_BUFFER, geo.vertexBuffer );
    glBufferData( GL_ARRAY_BUFFER, byteCount, (void *) verts, GL_DYNAMIC_DRAW );

    glEnableVertexAttribArray( VERTEX_POSITION );
    glVertexAttribPointer( VERTEX_POSITION, 2, GL_SHORT, false, sizeof( lineVert_t ), (void *)0 );

    glEnableVertexAttribArray( VERTEX_COLOR );
    glVertexAttribPointer( VERTEX_COLOR, 4, GL_UNSIGNED_BYTE, true, sizeof( lineVert_t ), (void *)4 );

    delete[] verts;


    geo.indexCount = lineVertCount;

    glOperation.glBindVertexArrayOES( 0 );

    return geo;
}


float calibrateFovScale = 1.0f;

void VFrameSmooth::Private::createFrameworkGraphics()
{

    unsigned char blackData[4] = {};
    glGenTextures( 1, &m_blackTexId );
    glBindTexture( GL_TEXTURE_2D, m_blackTexId );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blackData );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    glBindTexture( GL_TEXTURE_2D, 0 );

    // Default loading icon.
    glGenTextures( 1, &m_defaultLoadingIconTexId );
    glBindTexture( GL_TEXTURE_2D, m_defaultLoadingIconTexId );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, oculus_loading_indicator_width, oculus_loading_indicator_height,
                  0, GL_RGBA, GL_UNSIGNED_BYTE, oculus_loading_indicator_bufferData );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    glBindTexture( GL_TEXTURE_2D, 0 );


    m_smoothMesh = VLensDistortion::createDistortionGrid( m_device, 1, calibrateFovScale, false );

    m_slicesmoothMesh = VLensDistortion::createDistortionGrid( m_device, NUM_SLICES_PER_EYE, calibrateFovScale, false );

    m_cursorMesh = VLensDistortion::createDistortionGrid( m_device, 1, calibrateFovScale, true );

    if ( m_smoothMesh.indexCount == 0 || m_slicesmoothMesh.indexCount == 0 )
    {
        vFatal("WarpMesh failed to load");
    }


   // m_timingGraph = CreateTimingGraphGeometry( (256+10)*2 );


    m_calibrationLines2.createCalibrationGrid( 0, false );

    m_untexturedMvpProgram.initShader(VGlShader::getUntextureMvpVertexShaderSource(),VGlShader::getUntexturedFragmentShaderSource());
    m_debugLineProgram.initShader(VGlShader::getVertexColorVertexShaderSource(),VGlShader::getUntexturedFragmentShaderSource());
    buildSmoothProgs();
}

void VFrameSmooth::Private::destroyFrameworkGraphics()
{
    glDeleteTextures( 1, &m_blackTexId );
    glDeleteTextures( 1, &m_defaultLoadingIconTexId );

    m_calibrationLines2.destroy();
    m_smoothMesh.destroy();
    m_slicesmoothMesh.destroy();
    m_cursorMesh.destroy();


    m_untexturedMvpProgram.destroy();
    m_debugLineProgram.destroy();

    for ( int i = 0; i < WP_PROGRAM_MAX; i++ )
    {
        m_warpPrograms[i].destroy();
    }
}


void VFrameSmooth::Private::drawFrameworkGraphicsToWindow( const int eye,
                                                   const int swapOptions)
{
    VGlOperation glOperation;

    unsigned char latencyTesterColorToDisplay[3];

    if ( ovr_ProcessLatencyTest( latencyTesterColorToDisplay ) )
    {
        glClearColor(
                    latencyTesterColorToDisplay[0] / 255.0f,
                latencyTesterColorToDisplay[1] / 255.0f,
                latencyTesterColorToDisplay[2] / 255.0f,
                1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
    }


    const char * results = ovr_GetLatencyTestResult();
    if ( results != NULL )
    {
        vInfo("LATENCY TESTER: " << results);
    }


    if ( swapOptions & SWAP_OPTION_DRAW_CALIBRATION_LINES )
    {
        const float znear = 0.5f;
        const float zfar = 150.0f;

        const VR4Matrixf projectionMatrix(
                    0, 1, 0, 0,
                    -1, 0, 0, 0,
                    0, 0, zfar / (znear - zfar), (zfar * znear) / (znear - zfar),
                    0, 0, -1, 0 );
        glUseProgram( m_untexturedMvpProgram.program );
        glLineWidth( 2.0f );
        glUniform4f( m_untexturedMvpProgram.uniformColor, 1, 0, 0, 1 );
        glUniformMatrix4fv( m_untexturedMvpProgram.uniformModelViewProMatrix, 1, GL_FALSE,  // not transposed
                            projectionMatrix.Transposed().M[0] );
        glOperation.glBindVertexArrayOES( m_calibrationLines2.vertexArrayObject );


        glViewport( m_window_width/2 * (int)eye, 0, m_window_width/2, m_window_height );
        glDrawElements( GL_LINES, m_calibrationLines2.indexCount, GL_UNSIGNED_SHORT, NULL );
        glViewport( 0, 0, m_window_width, m_window_height );
    }
}

void VFrameSmooth::Private::buildSmoothProgPair( ovrTimeWarpProgram simpleIndex,
                                       const char * simpleVertex, const char * simpleFragment,
                                       const char * chromaticVertex, const char * chromaticFragment
)
{
    m_warpPrograms[ simpleIndex ] = VGlShader( simpleVertex, simpleFragment );
    m_warpPrograms[ simpleIndex + ( WP_CHROMATIC - WP_SIMPLE ) ] = VGlShader( chromaticVertex, chromaticFragment );
}

void VFrameSmooth::Private::buildSmoothProgMatchedPair( ovrTimeWarpProgram simpleIndex,
                                              const char * simpleVertex, const char * simpleFragment
)
{
    m_warpPrograms[ simpleIndex ] = VGlShader( simpleVertex, simpleFragment );
    m_warpPrograms[ simpleIndex + ( WP_CHROMATIC - WP_SIMPLE ) ] = VGlShader( simpleVertex, simpleFragment );
}


void VFrameSmooth::Private::buildSmoothProgs()
{
    buildSmoothProgPair( WP_SIMPLE,
                       // low quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"
                       "attribute vec2 TexCoord1;\n"
                       "varying  vec2 oTexCoord;\n"
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "   vec3 left = vec3( Texm * vec4(TexCoord,-1,1) );\n"
                       "   vec3 right = vec3( Texm2 * vec4(TexCoord,-1,1) );\n"
                       "   vec3 proj = mix( left, right, TexCoord1.x );\n"
                       "	float projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       "}\n"
                       ,
                       "uniform sampler2D Texture0;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "void main()\n"
                       "{\n"
                       "	gl_FragColor = texture2D(Texture0, oTexCoord);\n"
                       "}\n"
                       ,
                       // high quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"
                       "uniform mediump mat4 Texm3;\n"
                       "uniform mediump mat4 Texm4;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"	// green
                       "attribute vec2 TexCoord1;\n"	// .x = interpolated warp frac, .y = intensity scale
                       "attribute vec2 Normal;\n"		// red
                       "attribute vec2 Tangent;\n"		// blue
                       "varying  vec2 oTexCoord1r;\n"
                       "varying  vec2 oTexCoord1g;\n"
                       "varying  vec2 oTexCoord1b;\n"
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "	vec3 proj;\n"
                       "	float projIZ;\n"
                       ""
                       "   proj = mix( vec3( Texm * vec4(Normal,-1,1) ), vec3( Texm2 * vec4(Normal,-1,1) ), TexCoord1.x );\n"
                       "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord1r = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       ""
                       "   proj = mix( vec3( Texm * vec4(TexCoord,-1,1) ), vec3( Texm2 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord1g = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       ""
                       "   proj = mix( vec3( Texm * vec4(Tangent,-1,1) ), vec3( Texm2 * vec4(Tangent,-1,1) ), TexCoord1.x );\n"
                       "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord1b = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       ""
                       "}\n"
                       ,
                       "uniform sampler2D Texture0;\n"
                       "varying highp vec2 oTexCoord1r;\n"
                       "varying highp vec2 oTexCoord1g;\n"
                       "varying highp vec2 oTexCoord1b;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp vec4 color1r = texture2D(Texture0, oTexCoord1r);\n"
                       "	lowp vec4 color1g = texture2D(Texture0, oTexCoord1g);\n"
                       "	lowp vec4 color1b = texture2D(Texture0, oTexCoord1b);\n"
                       "	lowp vec4 color1 = vec4( color1r.x, color1g.y, color1b.z, 1.0 );\n"
                       "	gl_FragColor = color1;\n"
                       "}\n"
                       );

    buildSmoothProgPair( WP_MASKED_PLANE,
                       // low quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"
                       "uniform mediump mat4 Texm3;\n"
                       "uniform mediump mat4 Texm4;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"
                       "attribute vec2 TexCoord1;\n"
                       "varying  vec2 oTexCoord;\n"
                       "varying  vec3 oTexCoord2;\n"	// Must do the proj in fragment shader or you get wiggles when you view the plane at even modest angles.
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "	vec3 proj;\n"
                       "	float projIZ;\n"
                       ""
                       "   proj = mix( vec3( Texm * vec4(TexCoord,-1,1) ), vec3( Texm2 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       ""
                       "   oTexCoord2 = mix( vec3( Texm3 * vec4(TexCoord,-1,1) ), vec3( Texm4 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       ""
                       "}\n"
                       ,
                       "uniform sampler2D Texture0;\n"
                       "uniform sampler2D Texture1;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "varying highp vec3 oTexCoord2;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp vec4 color0 = texture2D(Texture0, oTexCoord);\n"
                       "	{\n"
                       "		lowp vec4 color1 = vec4( texture2DProj(Texture1, oTexCoord2).xyz, 1.0 );\n"
                       "		gl_FragColor = mix( color1, color0, color0.w );\n"	// pass through destination alpha
                       "	}\n"
                       "}\n"
                       ,
                       // high quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"
                       "uniform mediump mat4 Texm3;\n"
                       "uniform mediump mat4 Texm4;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"	// green
                       "attribute vec2 TexCoord1;\n"
                       "attribute vec2 Normal;\n"		// red
                       "attribute vec2 Tangent;\n"		// blue
                       "varying  vec2 oTexCoord;\n"
                       "varying  vec3 oTexCoord2r;\n"	// These must do the proj in fragment shader or you
                       "varying  vec3 oTexCoord2g;\n"	// get wiggles when you view the plane at even
                       "varying  vec3 oTexCoord2b;\n"	// modest angles.
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "	vec3 proj;\n"
                       "	float projIZ;\n"
                       ""
                       "   proj = mix( vec3( Texm * vec4(TexCoord,-1,1) ), vec3( Texm2 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       ""
                       "   oTexCoord2r = mix( vec3( Texm3 * vec4(Normal,-1,1) ), vec3( Texm4 * vec4(Normal,-1,1) ), TexCoord1.x );\n"
                       "   oTexCoord2g = mix( vec3( Texm3 * vec4(TexCoord,-1,1) ), vec3( Texm4 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "   oTexCoord2b = mix( vec3( Texm3 * vec4(Tangent,-1,1) ), vec3( Texm4 * vec4(Tangent,-1,1) ), TexCoord1.x );\n"
                       ""
                       "}\n"
                       ,
                       "uniform sampler2D Texture0;\n"
                       "uniform sampler2D Texture1;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "varying highp vec3 oTexCoord2r;\n"
                       "varying highp vec3 oTexCoord2g;\n"
                       "varying highp vec3 oTexCoord2b;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp vec4 color0 = texture2D(Texture0, oTexCoord);\n"
                       "	{\n"
                       "		lowp vec4 color1r = texture2DProj(Texture1, oTexCoord2r);\n"
                       "		lowp vec4 color1g = texture2DProj(Texture1, oTexCoord2g);\n"
                       "		lowp vec4 color1b = texture2DProj(Texture1, oTexCoord2b);\n"
                       "		lowp vec4 color1 = vec4( color1r.x, color1g.y, color1b.z, 1.0 );\n"
                       "		gl_FragColor = mix( color1, color0, color0.w );\n"	// pass through destination alpha
                       "	}\n"
                       "}\n"
                       );
    buildSmoothProgPair( WP_MASKED_PLANE_EXTERNAL,
                       // low quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"
                       "uniform mediump mat4 Texm3;\n"
                       "uniform mediump mat4 Texm4;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"
                       "attribute vec2 TexCoord1;\n"
                       "varying  vec2 oTexCoord;\n"
                       "varying  vec3 oTexCoord2;\n"	// Must do the proj in fragment shader or you get wiggles when you view the plane at even modest angles.
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "	vec3 proj;\n"
                       "	float projIZ;\n"
                       ""
                       "   proj = mix( vec3( Texm * vec4(TexCoord,-1,1) ), vec3( Texm2 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       ""
                       "   oTexCoord2 = mix( vec3( Texm3 * vec4(TexCoord,-1,1) ), vec3( Texm4 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       ""
                       "}\n"
                       ,
                       "#extension GL_OES_EGL_image_external : require\n"
                       "uniform sampler2D Texture0;\n"
                       "uniform samplerExternalOES Texture1;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "varying highp vec3 oTexCoord2;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp vec4 color0 = texture2D(Texture0, oTexCoord);\n"
                       "	{\n"
                       "		lowp vec4 color1 = vec4( texture2DProj(Texture1, oTexCoord2).xyz, 1.0 );\n"
                       "		gl_FragColor = mix( color1, color0, color0.w );\n"	// pass through destination alpha
                       "	}\n"
                       "}\n"
                       ,
                       // high quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"
                       "uniform mediump mat4 Texm3;\n"
                       "uniform mediump mat4 Texm4;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"	// green
                       "attribute vec2 TexCoord1;\n"
                       "attribute vec2 Normal;\n"		// red
                       "attribute vec2 Tangent;\n"		// blue
                       "varying  vec2 oTexCoord;\n"
                       "varying  vec3 oTexCoord2r;\n"	// These must do the proj in fragment shader or you
                       "varying  vec3 oTexCoord2g;\n"	// get wiggles when you view the plane at even
                       "varying  vec3 oTexCoord2b;\n"	// modest angles.
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "	vec3 proj;\n"
                       "	float projIZ;\n"
                       ""
                       "   proj = mix( vec3( Texm * vec4(TexCoord,-1,1) ), vec3( Texm2 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       ""
                       "   oTexCoord2r = mix( vec3( Texm3 * vec4(Normal,-1,1) ), vec3( Texm4 * vec4(Normal,-1,1) ), TexCoord1.x );\n"
                       "   oTexCoord2g = mix( vec3( Texm3 * vec4(TexCoord,-1,1) ), vec3( Texm4 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "   oTexCoord2b = mix( vec3( Texm3 * vec4(Tangent,-1,1) ), vec3( Texm4 * vec4(Tangent,-1,1) ), TexCoord1.x );\n"
                       ""
                       "}\n"
                       ,
                       "#extension GL_OES_EGL_image_external : require\n"
                       "uniform sampler2D Texture0;\n"
                       "uniform samplerExternalOES Texture1;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "varying highp vec3 oTexCoord2r;\n"
                       "varying highp vec3 oTexCoord2g;\n"
                       "varying highp vec3 oTexCoord2b;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp vec4 color0 = texture2D(Texture0, oTexCoord);\n"
                       "	{\n"
                       "		lowp vec4 color1r = texture2DProj(Texture1, oTexCoord2r);\n"
                       "		lowp vec4 color1g = texture2DProj(Texture1, oTexCoord2g);\n"
                       "		lowp vec4 color1b = texture2DProj(Texture1, oTexCoord2b);\n"
                       "		lowp vec4 color1 = vec4( color1r.x, color1g.y, color1b.z, 1.0 );\n"
                       "		gl_FragColor = mix( color1, color0, color0.w );\n"	// pass through destination alpha
                       "	}\n"
                       "}\n"
                       );
    buildSmoothProgPair( WP_MASKED_CUBE,
                       // low quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"
                       "uniform mediump mat4 Texm3;\n"
                       "uniform mediump mat4 Texm4;\n"
                       "uniform mediump vec2 FrameNum;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"
                       "attribute vec2 TexCoord1;\n"
                       "varying  vec2 oTexCoord;\n"
                       "varying  vec3 oTexCoord2;\n"
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "	vec3 proj;\n"
                       "	float projIZ;\n"
                       ""
                       "   proj = mix( vec3( Texm * vec4(TexCoord,-1,1) ), vec3( Texm2 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       ""
                       "   oTexCoord2 = mix( vec3( Texm3 * vec4(TexCoord,-1,1) ), vec3( Texm4 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       ""
                       "}\n"
                       ,
                       "uniform sampler2D Texture0;\n"
                       "uniform samplerCube Texture1;\n"
                       "uniform lowp float UniformColor;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "varying highp vec3 oTexCoord2;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp vec4 color0 = texture2D(Texture0, oTexCoord);\n"
                       "	lowp vec4 color1 = textureCube(Texture1, oTexCoord2) * UniformColor;\n"
                       "	gl_FragColor = vec4( mix( color1.xyz, color0.xyz, color0.w ), 1.0);\n"	// pass through destination alpha
                       "}\n"
                       ,
                       // high quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"
                       "uniform mediump mat4 Texm3;\n"
                       "uniform mediump mat4 Texm4;\n"
                       "uniform mediump vec2 FrameNum;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"	// green
                       "attribute vec2 TexCoord1;\n"
                       "attribute vec2 Normal;\n"		// red
                       "attribute vec2 Tangent;\n"		// blue
                       "varying  vec2 oTexCoord;\n"
                       "varying  vec3 oTexCoord2r;\n"
                       "varying  vec3 oTexCoord2g;\n"
                       "varying  vec3 oTexCoord2b;\n"
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "	vec3 proj;\n"
                       "	float projIZ;\n"
                       ""
                       "   proj = mix( vec3( Texm * vec4(TexCoord,-1,1) ), vec3( Texm2 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       ""
                       "   oTexCoord2r = mix( vec3( Texm3 * vec4(Normal,-1,1) ), vec3( Texm4 * vec4(Normal,-1,1) ), TexCoord1.x );\n"
                       "   oTexCoord2g = mix( vec3( Texm3 * vec4(TexCoord,-1,1) ), vec3( Texm4 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "   oTexCoord2b = mix( vec3( Texm3 * vec4(Tangent,-1,1) ), vec3( Texm4 * vec4(Tangent,-1,1) ), TexCoord1.x );\n"
                       ""
                       "}\n"
                       ,
                       "uniform sampler2D Texture0;\n"
                       "uniform samplerCube Texture1;\n"
                       "uniform lowp float UniformColor;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "varying highp vec3 oTexCoord2r;\n"
                       "varying highp vec3 oTexCoord2g;\n"
                       "varying highp vec3 oTexCoord2b;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp vec4 color0 = texture2D(Texture0, oTexCoord);\n"
                       "	lowp vec4 color1r = textureCube(Texture1, oTexCoord2r);\n"
                       "	lowp vec4 color1g = textureCube(Texture1, oTexCoord2g);\n"
                       "	lowp vec4 color1b = textureCube(Texture1, oTexCoord2b);\n"
                       "	lowp vec3 color1 = vec3( color1r.x, color1g.y, color1b.z ) * UniformColor;\n"
                       "	gl_FragColor = vec4( mix( color1, color0.xyz, color0.w ), 1.0);\n"	// pass through destination alpha
                       "}\n"
                       );
    buildSmoothProgPair( WP_CUBE,
                       // low quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"
                       "uniform mediump mat4 Texm3;\n"
                       "uniform mediump mat4 Texm4;\n"
                       "uniform mediump vec2 FrameNum;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"
                       "attribute vec2 TexCoord1;\n"
                       "varying  vec3 oTexCoord2;\n"
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "   oTexCoord2 = mix( vec3( Texm3 * vec4(TexCoord,-1,1) ), vec3( Texm4 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "}\n"
                       ,
                       "uniform samplerCube Texture1;\n"
                       "uniform samplerCube Texture2;\n"
                       "uniform samplerCube Texture3;\n"
                       "varying highp vec3 oTexCoord2;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp vec4 color1 = vec4( textureCube(Texture2, oTexCoord2).xyz, 1.0 );\n"
                       "	gl_FragColor = color1;\n"
                       "}\n"
                       ,
                       // high quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"
                       "uniform mediump mat4 Texm3;\n"
                       "uniform mediump mat4 Texm4;\n"
                       "uniform mediump vec2 FrameNum;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"	// green
                       "attribute vec2 TexCoord1;\n"
                       "attribute vec2 Normal;\n"		// red
                       "attribute vec2 Tangent;\n"		// blue
                       "varying  vec3 oTexCoord2r;\n"
                       "varying  vec3 oTexCoord2g;\n"
                       "varying  vec3 oTexCoord2b;\n"
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "   oTexCoord2r = mix( vec3( Texm3 * vec4(Normal,-1,1) ), vec3( Texm4 * vec4(Normal,-1,1) ), TexCoord1.x );\n"
                       "   oTexCoord2g = mix( vec3( Texm3 * vec4(TexCoord,-1,1) ), vec3( Texm4 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "   oTexCoord2b = mix( vec3( Texm3 * vec4(Tangent,-1,1) ), vec3( Texm4 * vec4(Tangent,-1,1) ), TexCoord1.x );\n"
                       "}\n"
                       ,
                       "uniform samplerCube Texture1;\n"
                       "uniform samplerCube Texture2;\n"
                       "uniform samplerCube Texture3;\n"
                       "varying highp vec3 oTexCoord2r;\n"
                       "varying highp vec3 oTexCoord2g;\n"
                       "varying highp vec3 oTexCoord2b;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp float color1r = textureCube(Texture1, oTexCoord2r).x;\n"
                       "	lowp float color1g = textureCube(Texture2, oTexCoord2g).x;\n"
                       "	lowp float color1b = textureCube(Texture3, oTexCoord2b).x;\n"
                       "	gl_FragColor = vec4( color1r, color1g, color1b, 1.0);\n"
                       "}\n"
                       );
    buildSmoothProgPair( WP_LOADING_ICON,
                       // low quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"
                       "attribute vec2 TexCoord1;\n"
                       "varying  vec2 oTexCoord;\n"
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "   vec3 left = vec3( Texm * vec4(TexCoord,-1,1) );\n"
                       "   vec3 right = vec3( Texm2 * vec4(TexCoord,-1,1) );\n"
                       "   vec3 proj = mix( left, right, TexCoord1.x );\n"
                       "	float projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       "}\n"
                       ,
                       "uniform sampler2D Texture0;\n"
                       "uniform sampler2D Texture1;\n"
                       "uniform highp vec4 RotateScale;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp vec4 color = texture2D(Texture0, oTexCoord);\n"
                       "	highp vec2 iconCenter = vec2( 0.5, 0.5 );\n"
                       "	highp vec2 localCoords = oTexCoord - iconCenter;\n"
                       "	highp vec2 iconCoords = vec2(	( localCoords.x * RotateScale.y - localCoords.y * RotateScale.x ) * RotateScale.z + iconCenter.x,\n"
                       "								( localCoords.x * RotateScale.x + localCoords.y * RotateScale.y ) * -RotateScale.z + iconCenter.x );\n"
                       "	if ( iconCoords.x > 0.0 && iconCoords.x < 1.0 && iconCoords.y > 0.0 && iconCoords.y < 1.0 )\n"
                       "	{\n"
                       "		lowp vec4 iconColor = texture2D(Texture1, iconCoords);"
                       "		color.rgb = ( 1.0 - iconColor.a ) * color.rgb + ( iconColor.a ) * iconColor.rgb;\n"
                       "	}\n"
                       "	gl_FragColor = color;\n"
                       "}\n"
                       ,
                       // high quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"
                       "attribute vec2 TexCoord1;\n"
                       "varying  vec2 oTexCoord;\n"
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "   vec3 left = vec3( Texm * vec4(TexCoord,-1,1) );\n"
                       "   vec3 right = vec3( Texm2 * vec4(TexCoord,-1,1) );\n"
                       "   vec3 proj = mix( left, right, TexCoord1.x );\n"
                       "	float projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       "}\n"
                       ,
                       "uniform sampler2D Texture0;\n"
                       "uniform sampler2D Texture1;\n"
                       "uniform highp vec4 RotateScale;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp vec4 color = texture2D(Texture0, oTexCoord);\n"
                       "	highp vec2 iconCenter = vec2( 0.5, 0.5 );\n"
                       "	highp vec2 localCoords = oTexCoord - iconCenter;\n"
                       "	highp vec2 iconCoords = vec2(	( localCoords.x * RotateScale.y - localCoords.y * RotateScale.x ) * RotateScale.z + iconCenter.x,\n"
                       "								( localCoords.x * RotateScale.x + localCoords.y * RotateScale.y ) * -RotateScale.z + iconCenter.x );\n"
                       "	if ( iconCoords.x > 0.0 && iconCoords.x < 1.0 && iconCoords.y > 0.0 && iconCoords.y < 1.0 )\n"
                       "	{\n"
                       "		lowp vec4 iconColor = texture2D(Texture1, iconCoords);"
                       "		color.rgb = ( 1.0 - iconColor.a ) * color.rgb + ( iconColor.a ) * iconColor.rgb;\n"
                       "	}\n"
                       "	gl_FragColor = color;\n"
                       "}\n"
                       );
    buildSmoothProgPair( WP_MIDDLE_CLAMP,
                       // low quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"
                       "attribute vec2 TexCoord1;\n"
                       "varying  vec2 oTexCoord;\n"
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "   vec3 left = vec3( Texm * vec4(TexCoord,-1,1) );\n"
                       "   vec3 right = vec3( Texm2 * vec4(TexCoord,-1,1) );\n"
                       "   vec3 proj = mix( left, right, TexCoord1.x );\n"
                       "	float projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       "}\n"
                       ,
                       "uniform sampler2D Texture0;\n"
                       "uniform highp vec2 TexClamp;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "void main()\n"
                       "{\n"
                       "	gl_FragColor = texture2D(Texture0, vec2( clamp( oTexCoord.x, TexClamp.x, TexClamp.y ), oTexCoord.y ) );\n"
                       "}\n"
                       ,
                       // high quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"
                       "attribute vec2 TexCoord1;\n"
                       "varying  vec2 oTexCoord;\n"
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "   vec3 left = vec3( Texm * vec4(TexCoord,-1,1) );\n"
                       "   vec3 right = vec3( Texm2 * vec4(TexCoord,-1,1) );\n"
                       "   vec3 proj = mix( left, right, TexCoord1.x );\n"
                       "	float projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       "}\n"
                       ,
                       "uniform sampler2D Texture0;\n"
                       "uniform highp vec2 TexClamp;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "void main()\n"
                       "{\n"
                       "	gl_FragColor = texture2D(Texture0, vec2( clamp( oTexCoord.x, TexClamp.x, TexClamp.y ), oTexCoord.y ) );\n"
                       "}\n"
                       );

    buildSmoothProgPair( WP_OVERLAY_PLANE,
                       // low quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"
                       "uniform mediump mat4 Texm3;\n"
                       "uniform mediump mat4 Texm4;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"
                       "attribute vec2 TexCoord1;\n"
                       "varying  vec2 oTexCoord;\n"
                       "varying  vec3 oTexCoord2;\n"	// Must do the proj in fragment shader or you get wiggles when you view the plane at even modest angles.
                       "varying  float clampVal;\n"
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "	vec3 proj;\n"
                       "	float projIZ;\n"
                       ""
                       "   proj = mix( vec3( Texm * vec4(TexCoord,-1,1) ), vec3( Texm2 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       ""
                       "   oTexCoord2 = mix( vec3( Texm3 * vec4(TexCoord,-1,1) ), vec3( Texm4 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       ""
                       // We need to clamp the projected texcoords to keep from getting a mirror
                       // image behind the view, and mip mapped edge clamp (I wish we had CLAMP_TO_BORDER)
                       // issues far off to the sides.
                       "	vec2 clampXY = oTexCoord2.xy / oTexCoord2.z;\n"
                       // this is backwards on Stratum    		"	clampVal = ( oTexCoord2.z > -0.01 || clampXY.x < -0.1 || clampXY.y < -0.1 || clampXY.x > 1.1 || clampXY.y > 1.1 ) ? 1.0 : 0.0;\n"
                       "	clampVal = ( oTexCoord2.z < -0.01 || clampXY.x < -0.1 || clampXY.y < -0.1 || clampXY.x > 1.1 || clampXY.y > 1.1 ) ? 1.0 : 0.0;\n"
                       "}\n"
                       ,
                       "uniform sampler2D Texture0;\n"
                       "uniform sampler2D Texture1;\n"
                       "varying lowp float clampVal;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "varying highp vec3 oTexCoord2;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp vec4 color0 = texture2D(Texture0, oTexCoord);\n"
                       "	if ( clampVal == 1.0 )\n"
                       "	{\n"
                       "		gl_FragColor = color0;\n"
                       "	} else {\n"
                       "		lowp vec4 color1 = texture2DProj(Texture1, oTexCoord2);\n"
                       "		gl_FragColor = mix( color0, color1, color1.w );\n"
                       "	}\n"
                       "}\n"
                       ,
                       // high quality
                       "uniform mediump mat4 Mvpm;\n"
                       "uniform mediump mat4 Texm;\n"
                       "uniform mediump mat4 Texm2;\n"
                       "uniform mediump mat4 Texm3;\n"
                       "uniform mediump mat4 Texm4;\n"

                       "attribute vec4 Position;\n"
                       "attribute vec2 TexCoord;\n"	// green
                       "attribute vec2 TexCoord1;\n"
                       "attribute vec2 Normal;\n"		// red
                       "attribute vec2 Tangent;\n"		// blue
                       "varying  vec2 oTexCoord;\n"
                       "varying  vec3 oTexCoord2r;\n"	// These must do the proj in fragment shader or you
                       "varying  vec3 oTexCoord2g;\n"	// get wiggles when you view the plane at even
                       "varying  vec3 oTexCoord2b;\n"	// modest angles.
                       "varying  float clampVal;\n"
                       "void main()\n"
                       "{\n"
                       "   gl_Position = Mvpm * Position;\n"
                       "	vec3 proj;\n"
                       "	float projIZ;\n"
                       ""
                       "   proj = mix( vec3( Texm * vec4(TexCoord,-1,1) ), vec3( Texm2 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                       "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                       ""
                       "   oTexCoord2r = mix( vec3( Texm3 * vec4(Normal,-1,1) ), vec3( Texm4 * vec4(Normal,-1,1) ), TexCoord1.x );\n"
                       "   oTexCoord2g = mix( vec3( Texm3 * vec4(TexCoord,-1,1) ), vec3( Texm4 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                       "   oTexCoord2b = mix( vec3( Texm3 * vec4(Tangent,-1,1) ), vec3( Texm4 * vec4(Tangent,-1,1) ), TexCoord1.x );\n"
                       ""
                       // We need to clamp the projected texcoords to keep from getting a mirror
                       // image behind the view, and mip mapped edge clamp (I wish we had CLAMP_TO_BORDER)
                       // issues far off to the sides.
                       "	vec2 clampXY = oTexCoord2r.xy / oTexCoord2r.z;\n"
                       "	clampVal = ( oTexCoord2r.z > -0.01 || clampXY.x < -0.1 || clampXY.y < -0.1 || clampXY.x > 1.1 || clampXY.y > 1.1 ) ? 1.0 : 0.0;\n"
                       "}\n"
                       ,
                       "uniform sampler2D Texture0;\n"
                       "uniform sampler2D Texture1;\n"
                       "varying lowp float clampVal;\n"
                       "varying highp vec2 oTexCoord;\n"
                       "varying highp vec3 oTexCoord2r;\n"
                       "varying highp vec3 oTexCoord2g;\n"
                       "varying highp vec3 oTexCoord2b;\n"
                       "void main()\n"
                       "{\n"
                       "	lowp vec4 color0 = texture2D(Texture0, oTexCoord);\n"
                       "	if ( clampVal == 1.0 )\n"
                       "	{\n"
                       "		gl_FragColor = color0;\n"
                       "	} else {\n"
                       "		lowp vec4 color1r = texture2DProj(Texture1, oTexCoord2r);\n"
                       "		lowp vec4 color1g = texture2DProj(Texture1, oTexCoord2g);\n"
                       "		lowp vec4 color1b = texture2DProj(Texture1, oTexCoord2b);\n"
                       "		lowp vec4 color1 = vec4( color1r.x, color1g.y, color1b.z, 1.0 );\n"
                       "		gl_FragColor = mix( color0, color1, vec4( color1r.w, color1g.w, color1b.w, 1.0 ) );\n"
                       "	}\n"
                       "}\n"
                       );


    // Debug program to color tint the overlay for LOD visualization
    buildSmoothProgMatchedPair( WP_OVERLAY_PLANE_SHOW_LOD,
                              "#version 300 es\n"
                              "uniform mediump mat4 Mvpm;\n"
                              "uniform mediump mat4 Texm;\n"
                              "uniform mediump mat4 Texm2;\n"
                              "uniform mediump mat4 Texm3;\n"
                              "uniform mediump mat4 Texm4;\n"

                              "in vec4 Position;\n"
                              "in vec2 TexCoord;\n"
                              "in vec2 TexCoord1;\n"
                              "out vec2 oTexCoord;\n"
                              "out vec3 oTexCoord2;\n"	// Must do the proj in fragment shader or you get wiggles when you view the plane at even modest angles.
                              "out float clampVal;\n"
                              "void main()\n"
                              "{\n"
                              "   gl_Position = Mvpm * Position;\n"
                              "	vec3 proj;\n"
                              "	float projIZ;\n"
                              ""
                              "   proj = mix( vec3( Texm * vec4(TexCoord,-1,1) ), vec3( Texm2 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                              "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                              "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                              ""
                              "   oTexCoord2 = mix( vec3( Texm3 * vec4(TexCoord,-1,1) ), vec3( Texm4 * vec4(TexCoord,-1,1) ), TexCoord1.x );\n"
                              ""
                              // We need to clamp the projected texcoords to keep from getting a mirror
                              // image behind the view, and mip mapped edge clamp (I wish we had CLAMP_TO_BORDER)
                              // issues far off to the sides.
                              "	vec2 clampXY = oTexCoord2.xy / oTexCoord2.z;\n"
                              "	clampVal = ( oTexCoord2.z > -0.01 || clampXY.x < -0.1 || clampXY.y < -0.1 || clampXY.x > 1.1 || clampXY.y > 1.1 ) ? 1.0 : 0.0;\n"
                              "}\n"
                              ,
                              "#version 300 es\n"
                              "uniform sampler2D Texture0;\n"
                              "uniform sampler2D Texture1;\n"
                              "in lowp float clampVal;\n"
                              "in highp vec2 oTexCoord;\n"
                              "in highp vec3 oTexCoord2;\n"
                              "out mediump vec4 fragColor;\n"
                              "void main()\n"
                              "{\n"
                              "	lowp vec4 color0 = texture(Texture0, oTexCoord);\n"
                              "	if ( clampVal == 1.0 )\n"
                              "	{\n"
                              "		fragColor = color0;\n"
                              "	} else {\n"
                              "		highp vec2 proj = vec2( oTexCoord2.x, oTexCoord2.y ) / oTexCoord2.z;\n"
                              "		lowp vec4 color1 = texture(Texture1, proj);\n"
                              "		mediump vec2 stepVal = fwidth( proj ) * vec2( textureSize( Texture1, 0 ) );\n"
                              "		mediump float w = max( stepVal.x, stepVal.y );\n"
                              "		if ( w < 1.0 ) { color1 = mix( color1, vec4( 0.0, 1.0, 0.0, 1.0 ), min( 1.0, 2.0 * ( 1.0 - w ) ) ); }\n"
                              "		else { color1 = mix( color1, vec4( 1.0, 0.0, 0.0, 1.0 ), min( 1.0, w - 1.0 ) ); }\n"
                              "		fragColor = mix( color0, color1, color1.w );\n"
                              "	}\n"
                              "}\n"
                              );

    buildSmoothProgMatchedPair( WP_CAMERA,
                              // low quality
                              "uniform mediump mat4 Mvpm;\n"
                              "uniform mediump mat4 Texm;\n"
                              "uniform mediump mat4 Texm2;\n"
                              "uniform mediump mat4 Texm3;\n"
                              "uniform mediump mat4 Texm4;\n"
                              "uniform mediump mat4 Texm5;\n"

                              "attribute vec4 Position;\n"
                              "attribute vec2 TexCoord;\n"
                              "attribute vec2 TexCoord1;\n"
                              "varying  vec2 oTexCoord;\n"
                              "varying  vec2 oTexCoord2;\n"
                              "void main()\n"
                              "{\n"
                              "   gl_Position = Mvpm * Position;\n"

                              "   vec4 lens = vec4(TexCoord,-1.0,1.0);"
                              "	vec3 proj;\n"
                              "	float projIZ;\n"
                              ""
                              "   proj = mix( vec3( Texm * lens ), vec3( Texm2 * lens ), TexCoord1.x );\n"
                              "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                              "	oTexCoord = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                              ""
                              "   vec4 dir = mix( lens, Texm2 * lens, TexCoord1.x );\n"
                              " dir.xy /= dir.z*-1.0;\n"
                              " dir.z = -1.0;\n"
                              " dir.w = 1.0;\n"
                              "	float rolling = Position.y * -1.5 + 0.5;\n"	// roughly 0 = top of camera, 1 = bottom of camera
                              "   proj = mix( vec3( Texm3 * lens ), vec3( Texm4 * lens ), rolling );\n"
                              "	projIZ = 1.0 / max( proj.z, 0.00001 );\n"
                              "	oTexCoord2 = vec2( proj.x * projIZ, proj.y * projIZ );\n"
                              ""
                              "}\n"
                              ,
                              "#extension GL_OES_EGL_image_external : require\n"
                              "uniform sampler2D Texture0;\n"
                              "uniform samplerExternalOES Texture1;\n"
                              "varying highp vec2 oTexCoord;\n"
                              "varying highp vec2 oTexCoord2;\n"
                              "void main()\n"
                              "{\n"
                              "	lowp vec4 color0 = texture2D(Texture0, oTexCoord);\n"
                              "		lowp vec4 color1 = vec4( texture2D(Texture1, oTexCoord2).xyz, 1.0 );\n"
                              "		gl_FragColor = mix( color1, color0, color0.w );\n"	// pass through destination alpha
                              //		" gl_FragColor = color1;"
                              "}\n"
                              );

}

void VFrameSmooth::Private::initRenderEnvironment()
{
    m_window_display = eglGetDisplay( EGL_DEFAULT_DISPLAY );
    m_window_surface = eglGetCurrentSurface( EGL_DRAW );
    eglQuerySurface( m_window_display, m_window_surface, EGL_WIDTH, &m_window_width );
    eglQuerySurface( m_window_display, m_window_surface, EGL_HEIGHT, &m_window_height );
}

void VFrameSmooth::Private::swapBuffers()
{
    eglSwapBuffers( m_window_display, m_window_surface );
}


NV_NAMESPACE_END

