/************************************************************************************

Filename    :   MovieSelectionView.cpp
Content     :
Created     :	6/19/2014
Authors     :   Jim Dos�

Copyright   :   Copyright 2014 Oculus VR, LLC. All Rights reserved.

This source code is licensed under the BSD-style license found in the
LICENSE file in the Cinema/ directory. An additional grant
of patent rights can be found in the PATENTS file in the same directory.

*************************************************************************************/

#include <android/keycodes.h>
#include "String_Utils.h"
#include "App.h"
#include "MovieSelectionView.h"
#include "CinemaApp.h"
#include "gui/VRMenuMgr.h"
#include "MovieCategoryComponent.h"
#include "MoviePosterComponent.h"
#include "MovieSelectionComponent.h"
#include "SwipeHintComponent.h"
#include "VApkFile.h"
#include "VrCommon.h"
#include "CinemaStrings.h"
#include "BitmapFont.h"
#include "Native.h"

namespace OculusCinema {

static const int PosterWidth = 228;
static const int PosterHeight = 344;

static const Vector3f PosterScale( 4.4859375f * 0.98f );

static const double TimerTotalTime = 10;

static const int NumSwipeTrails = 3;


//=======================================================================================

MovieSelectionView::MovieSelectionView( CinemaApp &cinema ) :
	View( "MovieSelectionView" ),
	Cinema( cinema ),
    SelectionTexture(),
    Is3DIconTexture(),
    ShadowTexture(),
    BorderTexture(),
    SwipeIconLeftTexture(),
    SwipeIconRightTexture(),
    ResumeIconTexture(),
    ErrorIconTexture(),
    SDCardTexture(),
	Menu( NULL ),
	CenterRoot( NULL ),
	ErrorMessage( NULL ),
	SDCardMessage( NULL ),
	PlainErrorMessage( NULL ),
	ErrorMessageClicked( false ),
    MovieRoot( NULL ),
    CategoryRoot( NULL ),
	TitleRoot( NULL ),
	MovieTitle( NULL ),
	SelectionFrame( NULL ),
	CenterPoster( NULL ),
	CenterIndex( 0 ),
	CenterPosition(),
	LeftSwipes(),
	RightSwipes(),
	ResumeIcon( NULL ),
	TimerIcon( NULL ),
	TimerText( NULL ),
	TimerStartTime( 0 ),
	TimerValue( 0 ),
	ShowTimer( false ),
	MoveScreenLabel( NULL ),
	MoveScreenAlpha(),
	SelectionFader(),
	MovieBrowser( NULL ),
	MoviePanelPositions(),
    MoviePosterComponents(),
    Categories(),
    CurrentCategory( CATEGORY_TRAILERS ),
	MovieList(),
	MoviesIndex( 0 ),
	LastMovieDisplayed( NULL ),
	RepositionScreen( false ),
	HadSelection( false )

{
	// This is called at library load time, so the system is not initialized
	// properly yet.
}

MovieSelectionView::~MovieSelectionView()
{
	DeletePointerArray( MovieBrowserItems );
}

void MovieSelectionView::OneTimeInit( const char * launchIntent )
{
	LOG( "MovieSelectionView::OneTimeInit" );

	const double start = ovr_GetTimeInSeconds();

    CreateMenu( Cinema.app, Cinema.app->vrMenuMgr(), Cinema.app->defaultFont() );

	SetCategory( CATEGORY_TRAILERS );

	LOG( "MovieSelectionView::OneTimeInit %3.1f seconds", ovr_GetTimeInSeconds() - start );
}

void MovieSelectionView::OneTimeShutdown()
{
	LOG( "MovieSelectionView::OneTimeShutdown" );
}

void MovieSelectionView::OnOpen()
{
	LOG( "OnOpen" );
	CurViewState = VIEWSTATE_OPEN;

	LastMovieDisplayed = NULL;
	HadSelection = NULL;

	if ( Cinema.inLobby )
	{
		Cinema.sceneMgr.SetSceneModel( *Cinema.modelMgr.BoxOffice );
		Cinema.sceneMgr.UseOverlay = false;

		Vector3f size( PosterWidth * VRMenuObject::DEFAULT_TEXEL_SCALE * PosterScale.x, PosterHeight * VRMenuObject::DEFAULT_TEXEL_SCALE * PosterScale.y, 0.0f );

		Cinema.sceneMgr.SceneScreenBounds = Bounds3f( size * -0.5f, size * 0.5f );
		Cinema.sceneMgr.SceneScreenBounds.Translate( Vector3f(  0.00f, 1.76f,  -7.25f ) );
	}

	Cinema.sceneMgr.LightsOn( 1.5f );

	const double now = ovr_GetTimeInSeconds();
	SelectionFader.Set( now, 0, now + 0.1, 1.0f );

	if ( Cinema.inLobby )
	{
		CategoryRoot->SetVisible( true );
		Menu->SetFlags( VRMENU_FLAG_BACK_KEY_EXITS_APP );
	}
	else
	{
		CategoryRoot->SetVisible( false );
		Menu->SetFlags( VRMenuFlags_t() );
	}

	ResumeIcon->SetVisible( false );
	TimerIcon->SetVisible( false );
	CenterRoot->SetVisible( true );

	MoveScreenLabel->SetVisible( false );

	MovieBrowser->SetSelectionIndex( MoviesIndex );

	RepositionScreen = false;

	UpdateMenuPosition();
	Cinema.sceneMgr.ClearGazeCursorGhosts();
	Menu->Open();

	MoviePosterComponent::ShowShadows = Cinema.inLobby;

	SwipeHintComponent::ShowSwipeHints = true;

    Cinema.app->swapParms().WarpProgram = WP_CHROMATIC;
}

void MovieSelectionView::OnClose()
{
	LOG( "OnClose" );
	ShowTimer = false;
	CurViewState = VIEWSTATE_CLOSED;
	CenterRoot->SetVisible( false );
	Menu->Close();
	Cinema.sceneMgr.ClearMovie();
}

bool MovieSelectionView::Command( const char * msg )
{
	return false;
}

bool MovieSelectionView::OnKeyEvent( const int keyCode, const KeyState::eKeyEventType eventType )
{
	switch ( keyCode )
	{
		case AKEYCODE_BACK:
		{
			switch ( eventType )
			{
				case KeyState::KEY_EVENT_DOUBLE_TAP:
					LOG( "KEY_EVENT_DOUBLE_TAP" );
					return true;
					break;
				default:
					break;
			}
		}
	}
	return false;
}

//=======================================================================================

void MovieSelectionView::CreateMenu( App * app, OvrVRMenuMgr & menuMgr, BitmapFont const & font )
{
	const Quatf forward( Vector3f( 0.0f, 1.0f, 0.0f ), 0.0f );

    // ==============================================================================
    //
	// load textures
	//
	SelectionTexture.LoadTextureFromApplicationPackage( "assets/selection.png" );
	Is3DIconTexture.LoadTextureFromApplicationPackage( "assets/3D_icon.png" );
	ShadowTexture.LoadTextureFromApplicationPackage( "assets/shadow.png" );
    BorderTexture.LoadTextureFromApplicationPackage( "assets/category_border.png" );
	SwipeIconLeftTexture.LoadTextureFromApplicationPackage( "assets/SwipeSuggestionArrowLeft.png" );
	SwipeIconRightTexture.LoadTextureFromApplicationPackage( "assets/SwipeSuggestionArrowRight.png" );
	ResumeIconTexture.LoadTextureFromApplicationPackage( "assets/resume.png" );
	ErrorIconTexture.LoadTextureFromApplicationPackage( "assets/error.png" );
	SDCardTexture.LoadTextureFromApplicationPackage( "assets/sdcard.png" );

    // ==============================================================================
    //
	// create menu
	//
	Menu = new UIMenu( Cinema );
	Menu->Create( "MovieBrowser" );

	CenterRoot = new UIContainer( Cinema );
	CenterRoot->AddToMenu( Menu );
	CenterRoot->SetLocalPose( forward, Vector3f( 0.0f, 0.0f, 0.0f ) );

	MovieRoot = new UIContainer( Cinema );
	MovieRoot->AddToMenu( Menu, CenterRoot );
	MovieRoot->SetLocalPose( forward, Vector3f( 0.0f, 0.0f, 0.0f ) );

	CategoryRoot = new UIContainer( Cinema );
	CategoryRoot->AddToMenu( Menu, CenterRoot );
	CategoryRoot->SetLocalPose( forward, Vector3f( 0.0f, 0.0f, 0.0f ) );

	TitleRoot = new UIContainer( Cinema );
	TitleRoot->AddToMenu( Menu, CenterRoot );
	TitleRoot->SetLocalPose( forward, Vector3f( 0.0f, 0.0f, 0.0f ) );

	// ==============================================================================
    //
	// error message
	//
	ErrorMessage = new UILabel( Cinema );
	ErrorMessage->AddToMenu( Menu, CenterRoot );
	ErrorMessage->SetLocalPose( forward, Vector3f( 0.00f, 1.76f, -7.39f + 0.5f ) );
	ErrorMessage->SetLocalScale( Vector3f( 5.0f ) );
	ErrorMessage->SetFontScale( 0.5f );
	ErrorMessage->SetTextOffset( Vector3f( 0.0f, -48 * VRMenuObject::DEFAULT_TEXEL_SCALE, 0.0f ) );
	ErrorMessage->SetImage( 0, SURFACE_TEXTURE_DIFFUSE, ErrorIconTexture );
	ErrorMessage->SetVisible( false );

    // ==============================================================================
    //
	// sdcard icon
	//
	SDCardMessage = new UILabel( Cinema );
	SDCardMessage->AddToMenu( Menu, CenterRoot );
	SDCardMessage->SetLocalPose( forward, Vector3f( 0.00f, 1.76f + 330.0f * VRMenuObject::DEFAULT_TEXEL_SCALE, -7.39f + 0.5f ) );
	SDCardMessage->SetLocalScale( Vector3f( 5.0f ) );
	SDCardMessage->SetFontScale( 0.5f );
	SDCardMessage->SetTextOffset( Vector3f( 0.0f, -96 * VRMenuObject::DEFAULT_TEXEL_SCALE, 0.0f ) );
	SDCardMessage->SetVisible( false );
	SDCardMessage->SetImage( 0, SURFACE_TEXTURE_DIFFUSE, SDCardTexture );

    // ==============================================================================
    //
	// error without icon
	//
	PlainErrorMessage = new UILabel( Cinema );
	PlainErrorMessage->AddToMenu( Menu, CenterRoot );
	PlainErrorMessage->SetLocalPose( forward, Vector3f( 0.00f, 1.76f + ( 330.0f - 48 ) * VRMenuObject::DEFAULT_TEXEL_SCALE, -7.39f + 0.5f ) );
	PlainErrorMessage->SetLocalScale( Vector3f( 5.0f ) );
	PlainErrorMessage->SetFontScale( 0.5f );
	PlainErrorMessage->SetVisible( false );

    // ==============================================================================
	//
	// movie browser
	//
    MoviePanelPositions.append( PanelPose( forward, Vector3f( -5.59f, 1.76f, -12.55f ), Vector4f( 0.0f, 0.0f, 0.0f, 0.0f ) ) );
    MoviePanelPositions.append( PanelPose( forward, Vector3f( -3.82f, 1.76f, -10.97f ), Vector4f( 0.1f, 0.1f, 0.1f, 1.0f ) ) );
    MoviePanelPositions.append( PanelPose( forward, Vector3f( -2.05f, 1.76f,  -9.39f ), Vector4f( 0.2f, 0.2f, 0.2f, 1.0f ) ) );
    MoviePanelPositions.append( PanelPose( forward, Vector3f(  0.00f, 1.76f,  -7.39f ), Vector4f( 1.0f, 1.0f, 1.0f, 1.0f ) ) );
    MoviePanelPositions.append( PanelPose( forward, Vector3f(  2.05f, 1.76f,  -9.39f ), Vector4f( 0.2f, 0.2f, 0.2f, 1.0f ) ) );
    MoviePanelPositions.append( PanelPose( forward, Vector3f(  3.82f, 1.76f, -10.97f ), Vector4f( 0.1f, 0.1f, 0.1f, 1.0f ) ) );
    MoviePanelPositions.append( PanelPose( forward, Vector3f(  5.59f, 1.76f, -12.55f ), Vector4f( 0.0f, 0.0f, 0.0f, 0.0f ) ) );

    CenterIndex = MoviePanelPositions.size() / 2;
    CenterPosition = MoviePanelPositions[ CenterIndex ].Position;

    MovieBrowser = new CarouselBrowserComponent( MovieBrowserItems, MoviePanelPositions );
    MovieRoot->AddComponent( MovieBrowser );

    // ==============================================================================
    //
    // selection rectangle
	//
    SelectionFrame = new UIImage( Cinema );
    SelectionFrame->AddToMenu( Menu, MovieRoot );
    SelectionFrame->SetLocalPose( forward, CenterPosition + Vector3f( 0.0f, 0.0f, 0.1f ) );
    SelectionFrame->SetLocalScale( PosterScale );
    SelectionFrame->SetImage( 0, SURFACE_TEXTURE_DIFFUSE, SelectionTexture );
    SelectionFrame->AddComponent( new MovieSelectionComponent( this ) );

	const Vector3f selectionBoundsExpandMin = Vector3f( 0.0f );
	const Vector3f selectionBoundsExpandMax = Vector3f( 0.0f, -0.13f, 0.0f );
    SelectionFrame->GetMenuObject()->setLocalBoundsExpand( selectionBoundsExpandMin, selectionBoundsExpandMax );

    // ==============================================================================
    //
    // add shadow and 3D icon to movie poster panels
    //
	VArray<VRMenuObject *> menuObjs;
    for ( uint i = 0; i < MoviePanelPositions.size(); ++i )
	{
		UIContainer *posterContainer = new UIContainer( Cinema );
		posterContainer->AddToMenu( Menu, MovieRoot );
		posterContainer->SetLocalPose( MoviePanelPositions[ i ].Orientation, MoviePanelPositions[ i ].Position );
        posterContainer->GetMenuObject()->addFlags( VRMENUOBJECT_FLAG_NO_FOCUS_GAINED );

		//
		// posters
		//
		UIImage * posterImage = new UIImage( Cinema );
		posterImage->AddToMenu( Menu, posterContainer );
		posterImage->SetLocalPose( forward, Vector3f( 0.0f, 0.0f, 0.0f ) );
		posterImage->SetImage( 0, SURFACE_TEXTURE_DIFFUSE, SelectionTexture.Texture, PosterWidth, PosterHeight );
		posterImage->SetLocalScale( PosterScale );
        posterImage->GetMenuObject()->addFlags( VRMENUOBJECT_FLAG_NO_FOCUS_GAINED );
        posterImage->GetMenuObject()->setLocalBoundsExpand( selectionBoundsExpandMin, selectionBoundsExpandMax );

		if ( i == CenterIndex )
		{
			CenterPoster = posterImage;
		}

		//
		// 3D icon
		//
		UIImage * is3DIcon = new UIImage( Cinema );
		is3DIcon->AddToMenu( Menu, posterContainer );
		is3DIcon->SetLocalPose( forward, Vector3f( 0.75f, 1.3f, 0.02f ) );
		is3DIcon->SetImage( 0, SURFACE_TEXTURE_DIFFUSE, Is3DIconTexture );
		is3DIcon->SetLocalScale( PosterScale );
        is3DIcon->GetMenuObject()->addFlags( VRMenuObjectFlags_t( VRMENUOBJECT_FLAG_NO_FOCUS_GAINED ) | VRMenuObjectFlags_t( VRMENUOBJECT_DONT_HIT_ALL ) );

		//
		// shadow
		//
		UIImage * shadow = new UIImage( Cinema );
		shadow->AddToMenu( Menu, posterContainer );
		shadow->SetLocalPose( forward, Vector3f( 0.0f, -1.97f, 0.00f ) );
		shadow->SetImage( 0, SURFACE_TEXTURE_DIFFUSE, ShadowTexture );
		shadow->SetLocalScale( PosterScale );
        shadow->GetMenuObject()->addFlags( VRMenuObjectFlags_t( VRMENUOBJECT_FLAG_NO_FOCUS_GAINED ) | VRMenuObjectFlags_t( VRMENUOBJECT_DONT_HIT_ALL ) );

		//
		// add the component
		//
		MoviePosterComponent *posterComp = new MoviePosterComponent();
		posterComp->SetMenuObjects( PosterWidth, PosterHeight, posterContainer, posterImage, is3DIcon, shadow );
		posterContainer->AddComponent( posterComp );

        menuObjs.append( posterContainer->GetMenuObject() );
        MoviePosterComponents.append( posterComp );
	}

	MovieBrowser->SetMenuObjects( menuObjs, MoviePosterComponents );

    // ==============================================================================
    //
    // category browser
    //
    Categories.append( MovieCategoryButton( CATEGORY_TRAILERS, CinemaStrings::Category_Trailers ) );
    Categories.append( MovieCategoryButton( CATEGORY_MYVIDEOS, CinemaStrings::Category_MyVideos ) );

    // create the buttons and calculate their size
    const float itemWidth = 1.10f;
    float categoryBarWidth = 0.0f;
    for ( uint i = 0; i < Categories.size(); ++i )
	{
		Categories[ i ].Button = new UILabel( Cinema );
		Categories[ i ].Button->AddToMenu( Menu, CategoryRoot );
		Categories[ i ].Button->SetFontScale( 2.2f );
		Categories[ i ].Button->SetText( Categories[ i ].Text.toCString() );
		Categories[ i ].Button->AddComponent( new MovieCategoryComponent( this, Categories[ i ].Category ) );

        const Bounds3f & bounds = Categories[ i ].Button->GetTextLocalBounds( Cinema.app->defaultFont() );
		Categories[ i ].Width = NervGear::Alg::Max( bounds.GetSize().x, itemWidth ) + 80.0f * VRMenuObject::DEFAULT_TEXEL_SCALE;
		Categories[ i ].Height = bounds.GetSize().y + 108.0f * VRMenuObject::DEFAULT_TEXEL_SCALE;
		categoryBarWidth += Categories[ i ].Width;
	}

	// reposition the buttons and set the background and border
	float startX = categoryBarWidth * -0.5f;
    for ( uint i = 0; i < Categories.size(); ++i )
	{
		VRMenuSurfaceParms panelSurfParms( "",
				BorderTexture.Texture, BorderTexture.Width, BorderTexture.Height, SURFACE_TEXTURE_ADDITIVE,
				0, 0, 0, SURFACE_TEXTURE_MAX,
				0, 0, 0, SURFACE_TEXTURE_MAX );

		panelSurfParms.Border = Vector4f( 14.0f );
		panelSurfParms.Dims = Vector2f( Categories[ i ].Width * VRMenuObject::TEXELS_PER_METER, Categories[ i ].Height * VRMenuObject::TEXELS_PER_METER );

		Categories[ i ].Button->SetImage( 0, panelSurfParms );
		Categories[ i ].Button->SetLocalPose( forward, Vector3f( startX + Categories[ i ].Width * 0.5f, 3.6f, -7.39f ) );
		Categories[ i ].Button->SetLocalBoundsExpand( Vector3f( 0.0f, 0.13f, 0.0f ), Vector3f::ZERO );

    	startX += Categories[ i ].Width;
	}

	// ==============================================================================
    //
    // movie title
    //
	MovieTitle = new UILabel( Cinema );
	MovieTitle->AddToMenu( Menu, TitleRoot );
	MovieTitle->SetLocalPose( forward, Vector3f( 0.0f, 0.045f, -7.37f ) );
    MovieTitle->GetMenuObject()->addFlags( VRMenuObjectFlags_t( VRMENUOBJECT_DONT_HIT_ALL ) );
	MovieTitle->SetFontScale( 2.5f );

	// ==============================================================================
    //
    // swipe icons
    //
	float yPos = 1.76f - ( PosterHeight - SwipeIconLeftTexture.Height ) * 0.5f * VRMenuObject::DEFAULT_TEXEL_SCALE * PosterScale.y;

	for( int i = 0; i < NumSwipeTrails; i++ )
	{
		Vector3f swipeIconPos = Vector3f( 0.0f, yPos, -7.17f + 0.01f * ( float )i );

		LeftSwipes[ i ] = new UIImage( Cinema );
		LeftSwipes[ i ]->AddToMenu( Menu, CenterRoot );
		LeftSwipes[ i ]->SetImage( 0, SURFACE_TEXTURE_DIFFUSE, SwipeIconLeftTexture );
		LeftSwipes[ i ]->SetLocalScale( PosterScale );
        LeftSwipes[ i ]->GetMenuObject()->addFlags( VRMenuObjectFlags_t( VRMENUOBJECT_FLAG_NO_DEPTH ) | VRMenuObjectFlags_t( VRMENUOBJECT_DONT_HIT_ALL ) );
		LeftSwipes[ i ]->AddComponent( new SwipeHintComponent( MovieBrowser, false, 1.3333f, 0.4f + ( float )i * 0.13333f, 5.0f ) );

		swipeIconPos.x = ( ( PosterWidth + SwipeIconLeftTexture.Width * ( i + 2 ) ) * -0.5f ) * VRMenuObject::DEFAULT_TEXEL_SCALE * PosterScale.x;
		LeftSwipes[ i ]->SetLocalPosition( swipeIconPos );

		RightSwipes[ i ] = new UIImage( Cinema );
		RightSwipes[ i ]->AddToMenu( Menu, CenterRoot );
		RightSwipes[ i ]->SetImage( 0, SURFACE_TEXTURE_DIFFUSE, SwipeIconRightTexture );
		RightSwipes[ i ]->SetLocalScale( PosterScale );
        RightSwipes[ i ]->GetMenuObject()->addFlags( VRMenuObjectFlags_t( VRMENUOBJECT_FLAG_NO_DEPTH ) | VRMenuObjectFlags_t( VRMENUOBJECT_DONT_HIT_ALL ) );
		RightSwipes[ i ]->AddComponent( new SwipeHintComponent( MovieBrowser, true, 1.3333f, 0.4f + ( float )i * 0.13333f, 5.0f ) );

		swipeIconPos.x = ( ( PosterWidth + SwipeIconRightTexture.Width * ( i + 2 ) ) * 0.5f ) * VRMenuObject::DEFAULT_TEXEL_SCALE * PosterScale.x;
		RightSwipes[ i ]->SetLocalPosition( swipeIconPos );
    }

	// ==============================================================================
    //
    // resume icon
    //
	ResumeIcon = new UILabel( Cinema );
	ResumeIcon->AddToMenu( Menu, MovieRoot );
	ResumeIcon->SetLocalPose( forward, CenterPosition + Vector3f( 0.0f, 0.0f, 0.5f ) );
	ResumeIcon->SetImage( 0, SURFACE_TEXTURE_DIFFUSE, ResumeIconTexture );
    ResumeIcon->GetMenuObject()->addFlags( VRMenuObjectFlags_t( VRMENUOBJECT_DONT_HIT_ALL ) );
	ResumeIcon->SetFontScale( 0.3f );
	ResumeIcon->SetLocalScale( 6.0f );
	ResumeIcon->SetText( CinemaStrings::MovieSelection_Resume.toCString() );
	ResumeIcon->SetTextOffset( Vector3f( 0.0f, -ResumeIconTexture.Height * VRMenuObject::DEFAULT_TEXEL_SCALE * 0.5f, 0.0f ) );
	ResumeIcon->SetVisible( false );

    // ==============================================================================
    //
    // timer
    //
	TimerIcon = new UILabel( Cinema );
	TimerIcon->AddToMenu( Menu, MovieRoot );
	TimerIcon->SetLocalPose( forward, CenterPosition + Vector3f( 0.0f, 0.0f, 0.5f ) );
    TimerIcon->GetMenuObject()->addFlags( VRMenuObjectFlags_t( VRMENUOBJECT_DONT_HIT_ALL ) );
	TimerIcon->SetFontScale( 1.0f );
	TimerIcon->SetLocalScale( 2.0f );
	TimerIcon->SetText( "10" );
	TimerIcon->SetVisible( false );

	VRMenuSurfaceParms timerSurfaceParms( "timer",
		"assets/timer.tga", SURFACE_TEXTURE_DIFFUSE,
		"assets/timer_fill.tga", SURFACE_TEXTURE_COLOR_RAMP_TARGET,
		"assets/color_ramp_timer.tga", SURFACE_TEXTURE_COLOR_RAMP );

	TimerIcon->SetImage( 0, timerSurfaceParms );

	// text
	TimerText = new UILabel( Cinema );
	TimerText->AddToMenu( Menu, TimerIcon );
	TimerText->SetLocalPose( forward, Vector3f( 0.0f, -0.3f, 0.0f ) );
    TimerText->GetMenuObject()->addFlags( VRMenuObjectFlags_t( VRMENUOBJECT_DONT_HIT_ALL ) );
	TimerText->SetFontScale( 1.0f );
	TimerText->SetText( CinemaStrings::MovieSelection_Next );

    // ==============================================================================
    //
    // reorient message
    //
    MoveScreenLabel = new UILabel( Cinema );
    MoveScreenLabel->AddToMenu( Menu, NULL );
    MoveScreenLabel->SetLocalPose( forward, Vector3f( 0.0f, 0.0f, -1.8f ) );
    MoveScreenLabel->GetMenuObject()->addFlags( VRMenuObjectFlags_t( VRMENUOBJECT_DONT_HIT_ALL ) );
    MoveScreenLabel->SetFontScale( 0.5f );
    MoveScreenLabel->SetText( CinemaStrings::MoviePlayer_Reorient );
    MoveScreenLabel->SetTextOffset( Vector3f( 0.0f, -24 * VRMenuObject::DEFAULT_TEXEL_SCALE, 0.0f ) );  // offset to be below gaze cursor
    MoveScreenLabel->SetVisible( false );
}

Vector3f MovieSelectionView::ScalePosition( const Vector3f &startPos, const float scale, const float menuOffset ) const
{
	const float eyeHieght = Cinema.sceneMgr.Scene.ViewParms.EyeHeight;

	Vector3f pos = startPos;
	pos.x *= scale;
	pos.y = ( pos.y - eyeHieght ) * scale + eyeHieght + menuOffset;
	pos.z *= scale;
	pos += Cinema.sceneMgr.Scene.FootPos;

	return pos;
}

//
// Repositions the menu for the lobby scene or the theater
//
void MovieSelectionView::UpdateMenuPosition()
{
	// scale down when in a theater
	const float scale = Cinema.inLobby ? 1.0f : 0.55f;
    CenterRoot->GetMenuObject()->setLocalScale( Vector3f( scale ) );

	if ( !Cinema.inLobby && Cinema.sceneMgr.SceneInfo.UseFreeScreen )
	{
		Quatf orientation = Quatf( Cinema.sceneMgr.FreeScreenOrientation );
        CenterRoot->GetMenuObject()->setLocalRotation( orientation );
        CenterRoot->GetMenuObject()->setLocalPosition( Cinema.sceneMgr.FreeScreenOrientation.Transform( Vector3f( 0.0f, -1.76f * scale, 0.0f ) ) );
	}
	else
	{
		const float menuOffset = Cinema.inLobby ? 0.0f : 0.5f;
        CenterRoot->GetMenuObject()->setLocalRotation( Quatf() );
        CenterRoot->GetMenuObject()->setLocalPosition( ScalePosition( Vector3f::ZERO, scale, menuOffset ) );
	}
}

//============================================================================================

void MovieSelectionView::SelectMovie()
{
	LOG( "SelectMovie");

	// ignore selection while repositioning screen
	if ( RepositionScreen )
	{
		return;
	}

	int lastIndex = MoviesIndex;
	MoviesIndex = MovieBrowser->GetSelection();
	Cinema.setPlaylist( MovieList, MoviesIndex );

	if ( !Cinema.inLobby )
	{
		if ( lastIndex == MoviesIndex )
		{
			// selected the poster we were just watching
			Cinema.resumeMovieFromSavedLocation();
		}
		else
		{
			Cinema.resumeOrRestartMovie();
		}
	}
	else
	{
		Cinema.theaterSelection();
	}
}

void MovieSelectionView::StartTimer()
{
	const double now = ovr_GetTimeInSeconds();
	TimerStartTime = now;
	TimerValue = -1;
	ShowTimer = true;
}

const MovieDef *MovieSelectionView::GetSelectedMovie() const
{
	int selectedItem = MovieBrowser->GetSelection();
    if ( ( selectedItem >= 0 ) && ( selectedItem < MovieList.length() ) )
	{
		return MovieList[ selectedItem ];
	}

	return NULL;
}

void MovieSelectionView::SetMovieList( const VArray<const MovieDef *> &movies, const MovieDef *nextMovie )
{
    LOG( "SetMovieList: %d movies", movies.size() );

	MovieList = movies;
	DeletePointerArray( MovieBrowserItems );
    for( uint i = 0; i < MovieList.size(); i++ )
	{
		const MovieDef *movie = MovieList[ i ];

		LOG( "AddMovie: %s", movie->Filename.toCString() );

		CarouselItem *item = new CarouselItem();
		item->texture 		= movie->Poster;
		item->textureWidth 	= movie->PosterWidth;
		item->textureHeight	= movie->PosterHeight;
		item->userFlags 	= movie->Is3D ? 1 : 0;
        MovieBrowserItems.append( item );
	}
	MovieBrowser->SetItems( MovieBrowserItems );

	MovieTitle->SetText( "" );
	LastMovieDisplayed = NULL;

	MoviesIndex = 0;
	if ( nextMovie != NULL )
	{
        for( uint i = 0; i < MovieList.size(); i++ )
		{
			if ( movies[ i ] == nextMovie )
			{
				StartTimer();
				MoviesIndex = i;
				break;
			}
		}
	}

	MovieBrowser->SetSelectionIndex( MoviesIndex );

    if ( MovieList.size() == 0 )
	{
		if ( CurrentCategory == CATEGORY_MYVIDEOS )
		{
			SetError( CinemaStrings::Error_NoVideosInMyVideos.toCString(), false, false );
		}
		else
		{
			SetError( CinemaStrings::Error_NoVideosOnPhone.toCString(), true, false );
		}
	}
	else
	{
		ClearError();
	}
}

void MovieSelectionView::SetCategory( const MovieCategory category )
{
	// default to category in index 0
	uint categoryIndex = 0;
    for( uint i = 0; i < Categories.size(); ++i )
	{
		if ( category == Categories[ i ].Category )
		{
			categoryIndex = i;
			break;
		}
	}

	LOG( "SetCategory: %s", Categories[ categoryIndex ].Text.toCString() );
	CurrentCategory = Categories[ categoryIndex ].Category;
    for( uint i = 0; i < Categories.size(); ++i )
	{
		Categories[ i ].Button->SetHilighted( i == categoryIndex );
	}

	// reset all the swipe icons so they match the current poster
	for( int i = 0; i < NumSwipeTrails; i++ )
	{
		SwipeHintComponent * compLeft = LeftSwipes[ i ]->GetMenuObject()->GetComponentByName<SwipeHintComponent>();
		compLeft->Reset( LeftSwipes[ i ]->GetMenuObject() );
		SwipeHintComponent * compRight = RightSwipes[ i ]->GetMenuObject()->GetComponentByName<SwipeHintComponent>();
		compRight->Reset( RightSwipes[ i ]->GetMenuObject() );
	}

	SetMovieList( Cinema.movieMgr.GetMovieList( CurrentCategory ), NULL );

    LOG( "%d movies added", MovieList.size() );
}

void MovieSelectionView::UpdateMovieTitle()
{
	const MovieDef * currentMovie = GetSelectedMovie();
	if ( LastMovieDisplayed != currentMovie )
	{
		if ( currentMovie != NULL )
		{
			MovieTitle->SetText( currentMovie->Title.toCString() );
		}
		else
		{
			MovieTitle->SetText( "" );
		}

		LastMovieDisplayed = currentMovie;
	}
}

void MovieSelectionView::SelectionHighlighted( bool isHighlighted )
{
	if ( isHighlighted && !ShowTimer && !Cinema.inLobby && ( MoviesIndex == MovieBrowser->GetSelection() ) )
	{
		// dim the poster when the resume icon is up and the poster is highlighted
		CenterPoster->SetColor( Vector4f( 0.55f, 0.55f, 0.55f, 1.0f ) );
	}
	else if ( MovieBrowser->HasSelection() )
	{
		CenterPoster->SetColor( Vector4f( 1.0f ) );
	}
}

void MovieSelectionView::UpdateSelectionFrame( const VrFrame & vrFrame )
{
	const double now = ovr_GetTimeInSeconds();
	if ( !MovieBrowser->HasSelection() )
	{
		SelectionFader.Set( now, 0, now + 0.1, 1.0f );
		TimerStartTime = 0;
	}

    if ( !SelectionFrame->GetMenuObject()->isHilighted() )
	{
		SelectionFader.Set( now, 0, now + 0.1, 1.0f );
	}
	else
	{
        MovieBrowser->CheckGamepad( Cinema.app, vrFrame, Cinema.app->vrMenuMgr(), MovieRoot->GetMenuObject() );
	}

	SelectionFrame->SetColor( Vector4f( SelectionFader.Value( now ) ) );

	if ( !ShowTimer && !Cinema.inLobby && ( MoviesIndex == MovieBrowser->GetSelection() ) )
	{
		ResumeIcon->SetColor( Vector4f( SelectionFader.Value( now ) ) );
		ResumeIcon->SetTextColor( Vector4f( SelectionFader.Value( now ) ) );
		ResumeIcon->SetVisible( true );
	}
	else
	{
		ResumeIcon->SetVisible( false );
	}

	if ( ShowTimer && ( TimerStartTime != 0 ) )
	{
		double frac = ( now - TimerStartTime ) / TimerTotalTime;
		if ( frac > 1.0f )
		{
			frac = 1.0f;
			Cinema.setPlaylist( MovieList, MovieBrowser->GetSelection() );
			Cinema.resumeOrRestartMovie();
		}
		Vector2f offset( 0.0f, 1.0f - frac );
		TimerIcon->SetColorTableOffset( offset );

		int seconds = TimerTotalTime - ( int )( TimerTotalTime * frac );
		if ( TimerValue != seconds )
		{
            TimerValue = seconds;
            TimerIcon->SetText(VString::number(seconds));
		}
		TimerIcon->SetVisible( true );
		CenterPoster->SetColor( Vector4f( 0.55f, 0.55f, 0.55f, 1.0f ) );
	}
	else
	{
		TimerIcon->SetVisible( false );
	}
}

void MovieSelectionView::SetError( const char *text, bool showSDCard, bool showErrorIcon )
{
	ClearError();

	LOG( "SetError: %s", text );
	if ( showSDCard )
	{
		SDCardMessage->SetVisible( true );
        SDCardMessage->SetTextWordWrapped( text, Cinema.app->defaultFont(), 1.0f );
	}
	else if ( showErrorIcon )
	{
		ErrorMessage->SetVisible( true );
        ErrorMessage->SetTextWordWrapped( text, Cinema.app->defaultFont(), 1.0f );
	}
	else
	{
		PlainErrorMessage->SetVisible( true );
        PlainErrorMessage->SetTextWordWrapped( text, Cinema.app->defaultFont(), 1.0f );
	}
	TitleRoot->SetVisible( false );
	MovieRoot->SetVisible( false );

    SwipeHintComponent::ShowSwipeHints = false;
}

void MovieSelectionView::ClearError()
{
	LOG( "ClearError" );
	ErrorMessageClicked = false;
	ErrorMessage->SetVisible( false );
	SDCardMessage->SetVisible( false );
	PlainErrorMessage->SetVisible( false );
	TitleRoot->SetVisible( true );
	MovieRoot->SetVisible( true );

    SwipeHintComponent::ShowSwipeHints = true;
}

bool MovieSelectionView::ErrorShown() const
{
	return ErrorMessage->GetVisible() || SDCardMessage->GetVisible() || PlainErrorMessage->GetVisible();
}

Matrix4f MovieSelectionView::DrawEyeView( const int eye, const float fovDegrees )
{
	return Cinema.sceneMgr.DrawEyeView( eye, fovDegrees );
}

Matrix4f MovieSelectionView::Frame( const VrFrame & vrFrame )
{
	// We want 4x MSAA in the lobby
    EyeParms eyeParms = Cinema.app->eyeParms();
	eyeParms.multisamples = 4;
    Cinema.app->setEyeParms( eyeParms );

#if 0
	if ( !Cinema.InLobby && Cinema.SceneMgr.ChangeSeats( vrFrame ) )
	{
		UpdateMenuPosition();
	}
#endif

	if ( vrFrame.Input.buttonPressed & BUTTON_B )
	{
		if ( Cinema.inLobby )
        {
            //Cinema.app->StartSystemActivity( PUI_CONFIRM_QUIT );
		}
		else
		{
            Cinema.app->guiSys().closeMenu( Cinema.app, Menu->GetVRMenu(), false );
		}
	}

	// check if they closed the menu with the back button
    if ( !Cinema.inLobby && Menu->GetVRMenu()->isClosedOrClosing() && !Menu->GetVRMenu()->isOpenOrOpening() )
	{
		// if we finished the movie or have an error, don't resume it, go back to the lobby
		if ( ErrorShown() )
		{
			LOG( "Error closed.  Return to lobby." );
			ClearError();
			Cinema.setMovieSelection( true );
		}
		else if ( Cinema.isMovieFinished() )
		{
			LOG( "Movie finished.  Return to lobby." );
			Cinema.setMovieSelection( true );
		}
		else
		{
			LOG( "Resume movie." );
			Cinema.resumeMovieFromSavedLocation();
		}
	}

	if ( !Cinema.inLobby && ErrorShown() )
	{
		SwipeHintComponent::ShowSwipeHints = false;
		if ( vrFrame.Input.buttonPressed & ( BUTTON_TOUCH | BUTTON_A ) )
		{
            Cinema.app->playSound( "touch_down" );
		}
		else if ( vrFrame.Input.buttonReleased & ( BUTTON_TOUCH | BUTTON_A ) )
		{
            Cinema.app->playSound( "touch_up" );
			ErrorMessageClicked = true;
		}
		else if ( ErrorMessageClicked && ( ( vrFrame.Input.buttonState & ( BUTTON_TOUCH | BUTTON_A ) ) == 0 ) )
		{
			Menu->Close();
		}
	}

	if ( Cinema.sceneMgr.FreeScreenActive && !ErrorShown() )
	{
        if ( !RepositionScreen && !SelectionFrame->GetMenuObject()->isHilighted() )
		{
			// outside of screen, so show reposition message
			const double now = ovr_GetTimeInSeconds();
			float alpha = MoveScreenAlpha.Value( now );
			if ( alpha > 0.0f )
			{
				MoveScreenLabel->SetVisible( true );
				MoveScreenLabel->SetTextColor( Vector4f( alpha ) );
			}

			if ( vrFrame.Input.buttonPressed & ( BUTTON_A | BUTTON_TOUCH ) )
			{
				// disable hit detection on selection frame
                SelectionFrame->GetMenuObject()->addFlags( VRMENUOBJECT_DONT_HIT_ALL );
				RepositionScreen = true;
			}
		}
		else
		{
			// onscreen, so hide message
			const double now = ovr_GetTimeInSeconds();
			MoveScreenAlpha.Set( now, -1.0f, now + 1.0f, 1.0f );
			MoveScreenLabel->SetVisible( false );
		}

		const Matrix4f invViewMatrix = Cinema.sceneMgr.Scene.CenterViewMatrix().Inverted();
		const Vector3f viewPos( GetViewMatrixPosition( Cinema.sceneMgr.Scene.CenterViewMatrix() ) );
		const Vector3f viewFwd( GetViewMatrixForward( Cinema.sceneMgr.Scene.CenterViewMatrix() ) );

		// spawn directly in front
		Quatf rotation( -viewFwd, 0.0f );
		Quatf viewRot( invViewMatrix );
		Quatf fullRotation = rotation * viewRot;

		const float menuDistance = 1.45f;
		Vector3f position( viewPos + viewFwd * menuDistance );

		MoveScreenLabel->SetLocalPose( fullRotation, position );
	}

	// while we're holding down the button or touchpad, reposition screen
	if ( RepositionScreen )
	{
		if ( vrFrame.Input.buttonState & ( BUTTON_A | BUTTON_TOUCH ) )
		{
			Cinema.sceneMgr.PutScreenInFront();
			Quatf orientation = Quatf( Cinema.sceneMgr.FreeScreenOrientation );
            CenterRoot->GetMenuObject()->setLocalRotation( orientation );
            CenterRoot->GetMenuObject()->setLocalPosition( Cinema.sceneMgr.FreeScreenOrientation.Transform( Vector3f( 0.0f, -1.76f * 0.55f, 0.0f ) ) );

		}
		else
		{
			RepositionScreen = false;
		}
	}
	else
	{
		// reenable hit detection on selection frame.
		// note: we do this on the frame following the frame we disabled RepositionScreen on
		// so that the selection object doesn't get the touch up.
        SelectionFrame->GetMenuObject()->removeFlags( VRMENUOBJECT_DONT_HIT_ALL );
	}

	UpdateMovieTitle();
	UpdateSelectionFrame( vrFrame );

	return Cinema.sceneMgr.Frame( vrFrame );
}

} // namespace OculusCinema
