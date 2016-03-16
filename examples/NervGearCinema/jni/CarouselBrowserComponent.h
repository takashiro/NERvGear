#pragma once

#include "gui/VRMenu.h"
#include "gui/VRMenuComponent.h"

using namespace NervGear;

namespace OculusCinema {

class CarouselItem
{
public:
	VString		name;
	GLuint		texture;
	int			textureWidth;
	int			textureHeight;
	uint		userFlags;

				CarouselItem() : texture( 0 ), textureWidth( 0 ), textureHeight( 0 ), userFlags( 0 ) {}
};

class PanelPose
{
public:
	Quatf    	Orientation;
	Vector3f 	Position;
	Vector4f	Color;

				PanelPose() {};
				PanelPose( Quatf orientation, Vector3f position, Vector4f color ) :
					Orientation( orientation ), Position( position ), Color( color ) {}
};

class CarouselItemComponent : public VRMenuComponent
{
public:
	explicit						CarouselItemComponent( VRMenuEventFlags_t const & eventFlags ) :
										VRMenuComponent( eventFlags )
									{
									}

	virtual							~CarouselItemComponent() { }

	virtual void 					SetItem( VRMenuObject * self, const CarouselItem * item, const PanelPose &pose ) = 0;
};

class CarouselBrowserComponent : public VRMenuComponent
{
public:
									CarouselBrowserComponent( const VArray<CarouselItem *> &items, const VArray<PanelPose> &panelPoses );

	void							SetPanelPoses( OvrVRMenuMgr & menuMgr, VRMenuObject * self, const VArray<PanelPose> &panelPoses );
	void 							SetMenuObjects( const VArray<VRMenuObject *> &menuObjs, const VArray<CarouselItemComponent *> &menuComps );
	void							SetItems( const VArray<CarouselItem *> &items );
	void							SetSelectionIndex( const int selectedIndex );
    int 							GetSelection() const;
	bool							HasSelection() const;
	bool							IsSwiping() const { return Swiping; }
	bool							CanSwipeBack() const;
	bool							CanSwipeForward() const;

	void 							CheckGamepad( App * app, VrFrame const & vrFrame, OvrVRMenuMgr & menuMgr, VRMenuObject * self );

private:
    eMsgStatus onEventImpl( App * app, VrFrame const & vrFrame, OvrVRMenuMgr & menuMgr, VRMenuObject * self, VRMenuEvent const & event ) override;
    PanelPose 						GetPosition( const float t );
    void 							UpdatePanels( OvrVRMenuMgr & menuMgr, VRMenuObject * self );

    eMsgStatus 						Frame( App * app, VrFrame const & vrFrame, OvrVRMenuMgr & menuMgr, VRMenuObject * self, VRMenuEvent const & event );
    eMsgStatus 						SwipeForward( App * app, VrFrame const & vrFrame, OvrVRMenuMgr & menuMgr, VRMenuObject * self );
    eMsgStatus 						SwipeBack( App * app, VrFrame const & vrFrame, OvrVRMenuMgr & menuMgr, VRMenuObject * self );
	eMsgStatus 						TouchDown( App * app, VrFrame const & vrFrame, OvrVRMenuMgr & menuMgr, VRMenuObject * self, VRMenuEvent const & event );
	eMsgStatus 						TouchUp( App * app, VrFrame const & vrFrame, OvrVRMenuMgr & menuMgr, VRMenuObject * self, VRMenuEvent const & event );
	eMsgStatus 						Opened( App * app, VrFrame const & vrFrame, OvrVRMenuMgr & menuMgr, VRMenuObject * self, VRMenuEvent const & event );
	eMsgStatus 						Closed( App * app, VrFrame const & vrFrame, OvrVRMenuMgr & menuMgr, VRMenuObject * self, VRMenuEvent const & event );

public:
    bool							SelectPressed;

private:
	Vector3f						PositionScale;
    float							Position;
	double							TouchDownTime;			// the time in second when a down even was received, < 0 if touch is not down

	int 							ItemWidth;
    int 							ItemHeight;

    VArray<CarouselItem *> 			Items;
    VArray<VRMenuObject *> 			MenuObjs;
    VArray<CarouselItemComponent *> 	MenuComps;
	VArray<PanelPose>				PanelPoses;

	double 							StartTime;
	double 							EndTime;
	float							PrevPosition;
	float							NextPosition;

	bool							Swiping;
	bool							PanelsNeedUpdate;
};

} // namespace OculusCinema

