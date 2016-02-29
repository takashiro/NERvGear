#include <VRMenuMgr.h>

#include "UI/UIMenu.h"
#include "CinemaApp.h"

namespace OculusCinema {

UIMenu::UIMenu( CinemaApp &cinema ) :
	Cinema( cinema ),
	MenuName(),
	Menu( NULL ),
	MenuOpen( false ),
	IdPool( 1 )

{
	// This is called at library load time, so the system is not initialized
	// properly yet.
}

UIMenu::~UIMenu()
{
	//DeletePointerArray( MovieBrowserItems );
}

VRMenuId_t UIMenu::AllocId()
{
	VRMenuId_t id = IdPool;
	IdPool = VRMenuId_t( IdPool.Get() + 1 );

	return id;
}

void UIMenu::Open()
{
	LOG( "Open" );
    Cinema.app->GetGuiSys().openMenu( Cinema.app, Cinema.app->GetGazeCursor(), MenuName.toCString() );
	MenuOpen = true;
}

void UIMenu::Close()
{
	LOG( "Close" );
    Cinema.app->GetGuiSys().closeMenu( Cinema.app, Menu, false );
	MenuOpen = false;
}

//=======================================================================================

void UIMenu::Create( const char *menuName )
{
	MenuName = menuName;
	Menu = VRMenu::Create( menuName );
    Menu->init( Cinema.app->GetVRMenuMgr(), Cinema.app->GetDefaultFont(), 0.0f, VRMenuFlags_t() );
    Cinema.app->GetGuiSys().addMenu( Menu );
}

VRMenuFlags_t const & UIMenu::GetFlags() const
{
    return Menu->flags();
}

void UIMenu::SetFlags( VRMenuFlags_t const & flags )
{
    Menu->setFlags( flags );
}

} // namespace OculusCinema