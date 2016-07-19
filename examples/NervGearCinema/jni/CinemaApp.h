#include "App.h"
#include "ShaderManager.h"
#include "SceneManager.h"

#include "VMainActivity.h"

NV_USING_NAMESPACE

namespace OculusCinema {

class MovieDef
{
public:
	VString			Filename;

	VString			Title;

	bool			Is3D;
	MovieFormat 	Format;

//    VTexture Poster;

//	VString			Theater;
//	MovieCategory	Category;

	bool            IsEncrypted;
	bool			AllowTheaterSelection;


    MovieDef() : Filename(), Title(), Is3D( false ), Format( VT_2D ),
			IsEncrypted( false ), AllowTheaterSelection( false ) {}
};


class CinemaApp : public VMainActivity
{
public:
    CinemaApp(JNIEnv *jni, jclass activityClass, jobject activityObject);

    void init(const VString &fromPackage, const VString &launchIntentJSON, const VString &launchIntentURI ) override;
    void shutdown() override;

    VR4Matrixf drawEyeView( const int eye, const float fovDegrees ) override;

    void configureVrMode(VKernel* kernel) override;

    void command(const VEvent &event) override;

	// Called by App loop
    VR4Matrixf onNewFrame( const VFrame m_vrFrame ) override;

    void			    	setMovie( const MovieDef * nextMovie );
    const MovieDef *		currentMovie() const { return m_currentMovie; }


    void 					startMoviePlayback();
    bool 					isMovieFinished() const;
public:
    double					startTime;

    SceneManager			sceneMgr;
    ShaderManager 			shaderMgr;

    bool					inLobby;
    bool					allowDebugControls;

private:
    VFrame					m_vrFrame;
    int						m_frameCount;

    const MovieDef *		m_currentMovie;
    VArray<const MovieDef *> m_playList;

    bool					m_shouldResumeMovie;
    bool					m_movieFinishedPlaying;
};

} // namespace OculusCinema
