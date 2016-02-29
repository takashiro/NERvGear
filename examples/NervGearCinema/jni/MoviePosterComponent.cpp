#include "MoviePosterComponent.h"
#include "UI/UIContainer.h"
#include "UI/UIImage.h"
#include "UI/UILabel.h"

namespace OculusCinema {

bool MoviePosterComponent::ShowShadows = true;

//==============================
//  MoviePosterComponent::
MoviePosterComponent::MoviePosterComponent() :
	CarouselItemComponent( VRMenuEventFlags_t() ),
	CurrentItem( NULL ),
    Poster( NULL ),
	PosterImage( NULL ),
    Is3DIcon( NULL ),
    Shadow( NULL )
{
}

//==============================
//  MoviePosterComponent::OnEvent_Impl
eMsgStatus MoviePosterComponent::onEventImpl( App * app, VrFrame const & vrFrame, OvrVRMenuMgr & menuMgr,
        VRMenuObject * self, VRMenuEvent const & event )
{
	return MSG_STATUS_ALIVE;
}

//==============================
//  MoviePosterComponent::SetMenuObjects
void MoviePosterComponent::SetMenuObjects( const int width, const int height, UIContainer * poster, UIImage * posterImage, UIImage * is3DIcon, UIImage * shadow )
{
	Width = width;
	Height = height;
	Poster = poster;
	PosterImage = posterImage;
    Is3DIcon = is3DIcon;
    Shadow = shadow;

    CurrentItem = NULL;
    Is3DIcon->SetVisible( false );
	Shadow->SetVisible( false );
	PosterImage->SetVisible( false );
}

//==============================
//  MoviePosterComponent::SetItem
void MoviePosterComponent::SetItem( VRMenuObject * self, const CarouselItem * item, const PanelPose &pose )
{
	Poster->SetLocalPose( pose.Orientation, pose.Position );
	PosterImage->SetColor( pose.Color );
	Is3DIcon->SetColor( pose.Color );
	Shadow->SetColor( pose.Color );

	if ( item != CurrentItem )
	{
		if ( item != NULL )
		{
			PosterImage->SetImage( 0, SURFACE_TEXTURE_DIFFUSE, item->texture, Width, Height );

			Is3DIcon->SetVisible( ( item->userFlags & 1 ) != 0 );
			Shadow->SetVisible( ShowShadows );
			PosterImage->SetVisible( true );
		}
		else
		{
			Is3DIcon->SetVisible( false );
			Shadow->SetVisible( false );
			PosterImage->SetVisible( false );
		}
		CurrentItem = item;
	}
}

} // namespace OculusCinema
