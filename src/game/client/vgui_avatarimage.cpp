//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
//=============================================================================

#include "cbase.h"
#include <vgui_controls/Controls.h>
#include <vgui_controls/Panel.h>
#include <vgui/ISurface.h>
#include "vgui_avatarimage.h"
#include "VGuiMatSurface/IMatSystemSurface.h"
#if defined( _X360 )
#include "xbox/xbox_win32stubs.h"
#endif
#include "steam/steam_api.h"
#include "gif_lib.h"

DECLARE_BUILD_FACTORY( CAvatarImagePanel );

CUtlMap< AvatarImagePair_t, int > CAvatarImage::s_staticAvatarCache; // cache of steam id's to textureids to use for static avatars
CUtlMap< CUtlString, AnimatedAvatarImagePair_t > CAvatarImage::s_animatedAvatarCache; // cache of avatar URLs to textureids to use for animated avatars
bool CAvatarImage::m_sbInitializedAvatarCache = false;

ConVar cl_animated_avatars( "cl_animated_avatars", "1", FCVAR_ARCHIVE, "Enable animated avatars" );

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CAvatarImage::CAvatarImage( void )
: m_sPersonaStateChangedCallback( this, &CAvatarImage::OnPersonaStateChanged )
{
	ClearAvatarSteamID();
	m_pFriendIcon = NULL;
	m_nX = 0;
	m_nY = 0;
	m_wide = m_tall = 0;
	m_avatarWide = m_avatarTall = 0;
	m_Color = Color( 255, 255, 255, 255 );
	m_bLoadPending = false;
	m_bSetDesiredSize = false;
	m_fNextLoadTime = 0.0f;
	m_AvatarSize = k_EAvatarSize32x32;
	
	//=============================================================================
	// HPE_BEGIN:
	//=============================================================================
	// [tj] Default to drawing the friend icon for avatars
	m_bDrawFriend = true;

	// [menglish] Default icon for avatar icons if there is no avatar icon for the player
	m_textureIDs.FillWithValue( -1 );

	// set up friend icon
	m_pFriendIcon = gHUD.GetIcon( "ico_friend_indicator_avatar" );

	m_pDefaultImage = NULL;

	SetAvatarSize(DEFAULT_AVATAR_SIZE, DEFAULT_AVATAR_SIZE);

	//=============================================================================
	// HPE_END
	//=============================================================================

	if ( !m_sbInitializedAvatarCache) 
	{
		m_sbInitializedAvatarCache = true;
		SetDefLessFunc( s_staticAvatarCache );
		s_animatedAvatarCache.SetLessFunc( UtlStringLessFunc );
	}
}

//-----------------------------------------------------------------------------
// Purpose: reset the image to a default state (will render with the default image)
//-----------------------------------------------------------------------------
void CAvatarImage::ClearAvatarSteamID( void )
{
	m_bValid = false;
	m_bFriend = false;
	m_bLoadPending = false;
	m_SteamID.Set( 0, k_EUniverseInvalid, k_EAccountTypeInvalid );
	m_sPersonaStateChangedCallback.Unregister();
}


//-----------------------------------------------------------------------------
// Purpose: Set the CSteamID for this image; this will cause a deferred load
//-----------------------------------------------------------------------------
bool CAvatarImage::SetAvatarSteamID( CSteamID steamIDUser, EAvatarSize avatarSize /*= k_EAvatarSize32x32 */ )
{
	ClearAvatarSteamID();

	m_SteamID = steamIDUser;
	// misyl: We determine this in UpdateAvatarImageSize.
	//m_AvatarSize = avatarSize;
	m_bLoadPending = true;

	m_sPersonaStateChangedCallback.Register( this, &CAvatarImage::OnPersonaStateChanged );

	if ( m_bSetDesiredSize )
	{
		LoadAvatarImage();
	}
	UpdateFriendStatus();

	return m_bValid;
}

//-----------------------------------------------------------------------------
// Purpose: Called when somebody changes their avatar image
//-----------------------------------------------------------------------------
void CAvatarImage::OnPersonaStateChanged( PersonaStateChange_t *info )
{
	if ( ( info->m_ulSteamID == m_SteamID.ConvertToUint64() ) && ( info->m_nChangeFlags & k_EPersonaChangeAvatar ) )
	{
		// Mark us as invalid.
		m_bValid = false;
		m_bLoadPending = true;

		// Poll
		UpdateAvatarImageSize();
		LoadAvatarImage();
	}
}

//-----------------------------------------------------------------------------
// Purpose: EquippedProfileItems_t callresult
//-----------------------------------------------------------------------------
void CAvatarImage::OnEquippedProfileItemsRequested( EquippedProfileItems_t* pInfo, bool bIOFailure )
{
	LoadAnimatedAvatar();
}

//-----------------------------------------------------------------------------
// Purpose: HTTPRequestCompleted_t callresult
//-----------------------------------------------------------------------------
void CAvatarImage::OnHTTPRequestCompleted( HTTPRequestCompleted_t* pInfo, bool bIOFailure )
{
	CUtlBuffer* buf = new CUtlBuffer;
	buf->EnsureCapacity( pInfo->m_unBodySize );
	buf->SeekPut( CUtlBuffer::SEEK_HEAD, pInfo->m_unBodySize );
	Verify( SteamHTTP()->GetHTTPResponseBodyData( pInfo->m_hRequest, ( uint8* )buf->Base(), pInfo->m_unBodySize ) );

	if( !m_animatedImage.OpenImage( buf ) )
	{
		SteamHTTP()->ReleaseHTTPRequest( pInfo->m_hRequest );
		return;
	}

	// Construct textures from the gif data
	do
	{
		if( m_animatedImage.GetSelectedFrame() >= ANIMATED_AVATAR_MAX_FRAME_COUNT )
		{
			// too many frames, stop processing
			break;
		}

		m_textureIDs[ m_animatedImage.GetSelectedFrame() ] = vgui::surface()->CreateNewTextureID( true );

		int iWide, iTall;
		m_animatedImage.GetScreenSize( iWide, iTall );
		uint8* pDest = new uint8[ iWide * iTall * 4 ];
		m_animatedImage.GetRGBA( &pDest );

		// bind RGBA data to the texture
		g_pMatSystemSurface->DrawSetTextureRGBAEx2( m_textureIDs[ m_animatedImage.GetSelectedFrame() ], pDest, iWide, iTall, IMAGE_FORMAT_RGBA8888, true );
		delete[] pDest;
	} while( !m_animatedImage.NextFrame() );

	// cache our texture IDs
	s_animatedAvatarCache.Insert( m_strAvatarUrl, AnimatedAvatarImagePair_t( buf, m_textureIDs ) );

	// if we are over cache size limit deallocate unused avatars
	if( s_animatedAvatarCache.Count() > ANIMATED_AVATAR_CACHE_MAX_COUNT )
	{
		FOR_EACH_MAP_BACK( s_animatedAvatarCache, i )
		{
			AnimatedAvatarImagePair_t& pair = s_animatedAvatarCache[ i ];
			if( pair.IsUnused() )
			{
				delete pair.m_pBuffer;
				FOR_EACH_ARRAY( pair.m_textureIDs, j )
				{
					if( pair.m_textureIDs[ j ] != -1 )
						vgui::surface()->DestroyTextureID( pair.m_textureIDs[ j ] );
				}
				s_animatedAvatarCache.RemoveAt( i );
			}
		}
	}

	m_bAnimating = true;

	SteamHTTP()->ReleaseHTTPRequest( pInfo->m_hRequest );
}

void CAvatarImage::UpdateAvatarImageSize()
{
	int nTall = GetAvatarTall();

	EAvatarSize eNewSize = k_EAvatarSize32x32;
	if ( nTall > 32 )
		eNewSize = k_EAvatarSize64x64;
	if ( nTall > 64 )
		eNewSize = k_EAvatarSize184x184;

	if ( m_AvatarSize != eNewSize )
		m_bLoadPending = true;

	m_AvatarSize = eNewSize;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CAvatarImage::LoadAnimatedAvatar()
{
	if( SteamHTTP() && SteamFriends() && SteamFriends()->BHasEquippedProfileItem( m_SteamID, k_ECommunityProfileItemType_AnimatedAvatar ) )
	{
		m_strAvatarUrl = SteamFriends()->GetProfileItemPropertyString( m_SteamID, k_ECommunityProfileItemType_AnimatedAvatar, k_ECommunityProfileItemProperty_ImageSmall );

		// See if we have this avatar cached already...
		int iIndex = s_animatedAvatarCache.Find( m_strAvatarUrl );
		if( iIndex != s_animatedAvatarCache.InvalidIndex() )
		{
			AnimatedAvatarImagePair_t& pair = s_animatedAvatarCache[ iIndex ];

			// ensure the buffer's read ptr is at head
			pair.m_pBuffer->SeekGet( CUtlBuffer::SEEK_HEAD, 0 );
			m_animatedImage.OpenImage( pair.m_pBuffer );

			m_textureIDs = pair.m_textureIDs;
			m_bAnimating = true;
			return;
		}

		HTTPRequestHandle hRequest = SteamHTTP()->CreateHTTPRequest( k_EHTTPMethodGET, m_strAvatarUrl );
		if( hRequest == INVALID_HTTPREQUEST_HANDLE )
		{
			return;
		}

		SteamAPICall_t hSendCall;
		if( !SteamHTTP()->SendHTTPRequest( hRequest, &hSendCall ) )
		{
			SteamHTTP()->ReleaseHTTPRequest( hRequest );
			return;
		}
		m_sHTTPRequestCompletedCallback.Set( hSendCall, this, &CAvatarImage::OnHTTPRequestCompleted );
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CAvatarImage::LoadStaticAvatar()
{
	if( !steamapicontext->SteamFriends()->RequestUserInformation( m_SteamID, false ) )
	{
		int iAvatar = 0;
		switch( m_AvatarSize )
		{
		case k_EAvatarSize32x32:
			iAvatar = steamapicontext->SteamFriends()->GetSmallFriendAvatar( m_SteamID );
			break;
		case k_EAvatarSize64x64:
			iAvatar = steamapicontext->SteamFriends()->GetMediumFriendAvatar( m_SteamID );
			break;
		case k_EAvatarSize184x184:
			iAvatar = steamapicontext->SteamFriends()->GetLargeFriendAvatar( m_SteamID );
			break;
		}

		//Msg( "Got avatar %d for SteamID %llud (%s)\n", iAvatar, m_SteamID.ConvertToUint64(), steamapicontext->SteamFriends()->GetFriendPersonaName( m_SteamID ) );

		if( iAvatar > 0 ) // if its zero, user doesn't have an avatar.  If -1, Steam is telling us that it's fetching it
		{
			uint32 wide = 0, tall = 0;
			if( steamapicontext->SteamUtils()->GetImageSize( iAvatar, &wide, &tall ) && wide > 0 && tall > 0 )
			{
				int destBufferSize = wide * tall * 4;
				byte* rgbDest = ( byte* )stackalloc( destBufferSize );
				if( steamapicontext->SteamUtils()->GetImageRGBA( iAvatar, rgbDest, destBufferSize ) )
					InitFromRGBA( iAvatar, rgbDest, wide, tall );

				stackfree( rgbDest );
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: load the avatar image if we have a load pending
//-----------------------------------------------------------------------------
void CAvatarImage::LoadAvatarImage()
{
	UpdateAvatarImageSize();

#ifdef CSS_PERF_TEST
	return;
#endif
	// attempt to retrieve the avatar image from Steam
	if ( m_bLoadPending && steamapicontext->SteamFriends() && steamapicontext->SteamUtils() && gpGlobals->curtime >= m_fNextLoadTime )
	{
		LoadStaticAvatar();
		if( cl_animated_avatars.GetBool() )
		{
			SteamAPICall_t hRequestItemsCall = SteamFriends()->RequestEquippedProfileItems( m_SteamID );
			m_sEquippedProfileItemsRequestedCallback.Set( hRequestItemsCall, this, &CAvatarImage::OnEquippedProfileItemsRequested );
		}

		if ( m_bValid )
		{
			// if we have a valid image, don't attempt to load it again
			m_bLoadPending = false;
		}
		else
		{
			// otherwise schedule another attempt to retrieve the image
			m_fNextLoadTime = gpGlobals->curtime + 1.0f;
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: Query Steam to set the m_bFriend status flag
//-----------------------------------------------------------------------------
void CAvatarImage::UpdateFriendStatus( void )
{
	if ( !m_SteamID.IsValid() )
		return;

	if ( steamapicontext->SteamFriends() && steamapicontext->SteamUtils() )
		m_bFriend = steamapicontext->SteamFriends()->HasFriend( m_SteamID, k_EFriendFlagImmediate );
}

//-----------------------------------------------------------------------------
// Purpose: Initialize the surface with the supplied raw RGBA image data
//-----------------------------------------------------------------------------
void CAvatarImage::InitFromRGBA( int iAvatar, const byte *rgba, int width, int height )
{
	int iTexIndex = s_staticAvatarCache.Find( AvatarImagePair_t( m_SteamID, iAvatar ) );
	if ( iTexIndex == s_staticAvatarCache.InvalidIndex() )
	{
		m_textureIDs[ 0 ] = vgui::surface()->CreateNewTextureID(true);
		g_pMatSystemSurface->DrawSetTextureRGBAEx2( m_textureIDs[ 0 ], rgba, width, height, IMAGE_FORMAT_RGBA8888, true );
		iTexIndex = s_staticAvatarCache.Insert( AvatarImagePair_t( m_SteamID, iAvatar ) );
		s_staticAvatarCache[ iTexIndex ] = m_textureIDs[ 0 ];
	}
	else
		m_textureIDs[ 0 ] = s_staticAvatarCache[iTexIndex];
	
	m_bValid = true;
}

//-----------------------------------------------------------------------------
// Purpose: Draw the image and optional friend icon
//-----------------------------------------------------------------------------
void CAvatarImage::Paint( void )
{
	if ( m_bFriend && m_pFriendIcon && m_bDrawFriend)
	{
		m_pFriendIcon->DrawSelf( m_nX, m_nY, m_wide, m_tall, m_Color );
	}

	int posX = m_nX;
	int posY = m_nY;

	if (m_bDrawFriend)
	{
		posX += FRIEND_ICON_AVATAR_INDENT_X * m_avatarWide / DEFAULT_AVATAR_SIZE;
		posY += FRIEND_ICON_AVATAR_INDENT_Y * m_avatarTall / DEFAULT_AVATAR_SIZE;
	}

	UpdateAvatarImageSize();
	
	if ( m_bLoadPending )
	{
		LoadAvatarImage();
	}

	int iTextureToDraw = m_textureIDs[ 0 ];

	// if we are an animated image, update the frame if needed
	if ( m_bAnimating )
	{
		if( m_animatedImage.ShouldIterateFrame() )
			m_animatedImage.NextFrame();

		iTextureToDraw = m_textureIDs[ m_animatedImage.GetSelectedFrame() ];

		int iCacheIndex = s_animatedAvatarCache.Find( m_strAvatarUrl );
		if( iCacheIndex != s_animatedAvatarCache.InvalidIndex() )
		{
			// update last used timestamp
			s_animatedAvatarCache[ iCacheIndex ].m_dLastUsedTimestamp = Plat_FloatTime();
		}
	}

	if ( m_bValid )
	{
		vgui::surface()->DrawSetTexture( iTextureToDraw );
		vgui::surface()->DrawSetColor( m_Color );
		vgui::surface()->DrawTexturedRect(posX, posY, posX + m_avatarWide, posY + m_avatarTall);
	}
	else if (m_pDefaultImage)
	{
		// draw default
		m_pDefaultImage->SetSize(m_avatarWide, m_avatarTall);
		m_pDefaultImage->SetPos(posX, posY);
		m_pDefaultImage->SetColor(m_Color);
		m_pDefaultImage->Paint();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Set the avatar size; scale the total image and friend icon to fit
//-----------------------------------------------------------------------------
void CAvatarImage::SetAvatarSize(int wide, int tall)
{
	m_avatarWide = wide;
	m_avatarTall = tall;

	if (m_bDrawFriend)
	{
		// scale the size of the friend background frame icon
		m_wide = FRIEND_ICON_SIZE_X * m_avatarWide / DEFAULT_AVATAR_SIZE;
		m_tall = FRIEND_ICON_SIZE_Y * m_avatarTall / DEFAULT_AVATAR_SIZE;
	}
	else
	{
		m_wide = m_avatarWide;
		m_tall = m_avatarTall;
	}

	m_bSetDesiredSize = true;

	UpdateAvatarImageSize();
}


//-----------------------------------------------------------------------------
// Purpose: Set the total image size; scale the avatar portion to fit
//-----------------------------------------------------------------------------
void CAvatarImage::SetSize( int wide, int tall )
{
	m_wide = wide;
	m_tall = tall;

	if (m_bDrawFriend)
	{
		// scale the size of the avatar portion based on the total image size
		m_avatarWide = DEFAULT_AVATAR_SIZE * m_wide / FRIEND_ICON_SIZE_X;
		m_avatarTall = DEFAULT_AVATAR_SIZE * m_tall / FRIEND_ICON_SIZE_Y ;
	}
	else
	{
		m_avatarWide = m_wide;
		m_avatarTall = m_tall;
	}
}

bool CAvatarImage::Evict()
{
	return false;
}

int CAvatarImage::GetNumFrames()
{
	return 0;
}

void CAvatarImage::SetFrame( int nFrame )
{
}

vgui::HTexture CAvatarImage::GetID()
{
	return 0;
}


//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CAvatarImagePanel::CAvatarImagePanel( vgui::Panel *parent, const char *name ) : BaseClass( parent, name )
{
	m_bScaleImage = false;
	m_pImage = new CAvatarImage();
	m_bSizeDirty = true;
	m_bClickable = false;
}


//-----------------------------------------------------------------------------
// Purpose: Set the avatar by C_BasePlayer pointer
//-----------------------------------------------------------------------------
void CAvatarImagePanel::SetPlayer( C_BasePlayer *pPlayer, EAvatarSize avatarSize )
{
	if ( pPlayer )
	{
		int iIndex = pPlayer->entindex();
		SetPlayer(iIndex, avatarSize);
	}
	else
		m_pImage->ClearAvatarSteamID();

}


//-----------------------------------------------------------------------------
// Purpose: Set the avatar by entity number
//-----------------------------------------------------------------------------
void CAvatarImagePanel::SetPlayer( int entindex, EAvatarSize avatarSize )
{
	m_pImage->ClearAvatarSteamID();

	player_info_t pi;
	if ( engine->GetPlayerInfo(entindex, &pi) )
	{
		if ( pi.friendsID != 0 	&& steamapicontext->SteamUtils() )
		{		
			CSteamID steamIDForPlayer( pi.friendsID, 1, GetUniverse(), k_EAccountTypeIndividual );
			SetPlayer(steamIDForPlayer, avatarSize);
		}
		else
		{
			m_pImage->ClearAvatarSteamID();
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Set the avatar by SteamID
//-----------------------------------------------------------------------------
void CAvatarImagePanel::SetPlayer(CSteamID steamIDForPlayer, EAvatarSize avatarSize )
{
	m_pImage->ClearAvatarSteamID();

	if (steamIDForPlayer.GetAccountID() != 0 )
		m_pImage->SetAvatarSteamID( steamIDForPlayer, avatarSize );
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CAvatarImagePanel::PaintBackground( void )
{
	if ( m_bSizeDirty )
		UpdateSize();

	m_pImage->Paint();
}

void CAvatarImagePanel::ClearAvatar()
{
	m_pImage->ClearAvatarSteamID();
}

void CAvatarImagePanel::SetDefaultAvatar( vgui::IImage* pDefaultAvatar )
{
	m_pImage->SetDefaultImage(pDefaultAvatar);
}

void CAvatarImagePanel::SetAvatarSize( int width, int height )
{
	if ( m_bScaleImage )
	{
		// panel is charge of image size - setting avatar size this way not allowed
		Assert(false);
		return;
	}
	else
	{
		m_pImage->SetAvatarSize( width, height );
		m_bSizeDirty = true;
	}
}

void CAvatarImagePanel::OnSizeChanged( int newWide, int newTall )
{
	BaseClass::OnSizeChanged(newWide, newTall);
	m_bSizeDirty = true;
}

void CAvatarImagePanel::OnMousePressed(vgui::MouseCode code)
{
	if ( !m_bClickable || code != MOUSE_LEFT )
		return;

	PostActionSignal( new KeyValues("AvatarMousePressed") );

	// audible feedback
	const char *soundFilename = "ui/buttonclick.wav";

	vgui::surface()->PlaySound( soundFilename );
}

void CAvatarImagePanel::SetShouldScaleImage( bool bScaleImage )
{
	m_bScaleImage = bScaleImage;
	m_bSizeDirty = true;
}

void CAvatarImagePanel::SetShouldDrawFriendIcon( bool bDrawFriend )
{
	m_pImage->SetDrawFriend(bDrawFriend);
	m_bSizeDirty = true;
}

void CAvatarImagePanel::UpdateSize()
{
	if ( m_bScaleImage )
	{
		// the panel is in charge of the image size
		m_pImage->SetAvatarSize(GetWide(), GetTall());
	}
	else
	{
		// the image is in charge of the panel size
		SetSize(m_pImage->GetAvatarWide(), m_pImage->GetAvatarTall() );
	}

	m_bSizeDirty = false;
}

void CAvatarImagePanel::ApplySettings( KeyValues *inResourceData )
{
	m_bScaleImage = inResourceData->GetInt("scaleImage", 0);

	BaseClass::ApplySettings(inResourceData);
}

int CGIFHelper::ReadData( GifFileType* pFile, GifByteType* pBuffer, int cubBuffer )
{
	auto pBuf = ( CUtlBuffer* )pFile->UserData;

	int nBytesToRead = MIN( cubBuffer, pBuf->GetBytesRemaining() );
	if( nBytesToRead > 0 )
		pBuf->Get( pBuffer, nBytesToRead );

	return nBytesToRead;
}

bool CGIFHelper::OpenImage( CUtlBuffer* pBuf )
{
	if( m_pImage )
	{
		CloseImage();
	}

	int nError;
	m_pImage = DGifOpen( pBuf, ReadData, &nError );
	if( !m_pImage )
	{
		DevWarning( "[CGIFHelper] Failed to open GIF image: %s\n", GifErrorString( nError ) );
		return false;
	}

	if( DGifSlurp( m_pImage ) != GIF_OK )
	{
		DevWarning( "[CGIFHelper] Failed to slurp GIF image: %s\n", GifErrorString( m_pImage->Error ) );
		CloseImage();
		return false;
	}

	int iWide, iTall;
	GetScreenSize( iWide, iTall );
	m_pPrevFrameBuffer = new uint8[ iWide * iTall * 4 ];

	return true;
}

void CGIFHelper::CloseImage( void )
{
	if( !m_pImage )
		return;

	delete[] m_pPrevFrameBuffer;

	int nError;
	if( DGifCloseFile( m_pImage, &nError ) != GIF_OK )
	{
		DevWarning( "[CGIFHelper] Failed to close GIF image: %s\n", GifErrorString( nError ) );
	}
	m_pImage = NULL;
	m_pPrevFrameBuffer = NULL;
	m_iSelectedFrame = 0;
	m_dIterateTime = 0.0;
}

bool CGIFHelper::NextFrame( void )
{
	if( !m_pImage )
		return false;

	m_iSelectedFrame++;

	if( m_iSelectedFrame >= m_pImage->ImageCount )
	{
		// Loop
		m_iSelectedFrame = 0;
	}

	GraphicsControlBlock gcb;
	if( DGifSavedExtensionToGCB( m_pImage, m_iSelectedFrame, &gcb ) == GIF_OK )
	{
		m_dIterateTime = ( gcb.DelayTime * 0.01 ) + Plat_FloatTime();
	}

	return m_iSelectedFrame == 0;
}

void CGIFHelper::GetRGBA( uint8** ppOutFrameBuffer )
{
	if( !m_pImage )
		return;

	SavedImage* pFrame = &m_pImage->SavedImages[ m_iSelectedFrame ];

	ColorMapObject* pColorMap = pFrame->ImageDesc.ColorMap ? pFrame->ImageDesc.ColorMap : m_pImage->SColorMap;
	GifByteType* pRasterBits = pFrame->RasterBits;

	int iScreenWide, iScreenTall;
	GetScreenSize( iScreenWide, iScreenTall );
	int iFrameWide, iFrameTall;
	GetFrameSize( iFrameWide, iFrameTall );

	int iFrameLeft = pFrame->ImageDesc.Left;
	int iFrameTop = pFrame->ImageDesc.Top;

	int nTransparentIndex = NO_TRANSPARENT_COLOR;
	int nDisposalMethod = DISPOSAL_UNSPECIFIED;

	GraphicsControlBlock gcb;
	if( DGifSavedExtensionToGCB( m_pImage, m_iSelectedFrame, &gcb ) == GIF_OK )
	{
		nTransparentIndex = gcb.TransparentColor;
		nDisposalMethod = gcb.DisposalMode;
	}

	// temporary buffer for current frame
	uint8* pCurFrameBuffer = ( uint8* )stackalloc( iScreenWide * iScreenTall * 4 );
	Q_memcpy( pCurFrameBuffer, m_pPrevFrameBuffer, iScreenWide * iScreenTall * 4 );

	int iPixel = 0;
	if( pFrame->ImageDesc.Interlace )
	{
		const int k_rowOffsets[] = { 0, 4, 2, 1 }; // interlacing row offsets
		const int k_rowIncrements[] = { 8, 8, 4, 2 }; // interlacing row increments

		for( int nPass = 0; nPass < 4; nPass++ )
		{
			for( int y = k_rowOffsets[ nPass ]; y < iFrameTall; y += k_rowIncrements[ nPass ] )
			{
				if( y + iFrameTop >= iScreenTall ) continue;
				for( int x = 0; x < iFrameWide; x++ )
				{
					if( x + iFrameLeft >= iScreenWide ) continue;
					int iOut = ( ( y + iFrameTop ) * iScreenWide + ( x + iFrameLeft ) ) * 4;
					GifByteType colorIndex = pRasterBits[ iPixel ];
					if( colorIndex < pColorMap->ColorCount && colorIndex != nTransparentIndex )
					{
						GifColorType& color = pColorMap->Colors[ colorIndex ];
						pCurFrameBuffer[ iOut + 0 ] = color.Red;
						pCurFrameBuffer[ iOut + 1 ] = color.Green;
						pCurFrameBuffer[ iOut + 2 ] = color.Blue;
						pCurFrameBuffer[ iOut + 3 ] = 255;
					}
					// else retain prev frame buffer pixel data
					iPixel++;
				}
			}
		}
	}
	else
	{
		for( int y = 0; y < iFrameTall; y++ )
		{
			if( y + iFrameTop >= iScreenTall ) continue;
			for( int x = 0; x < iFrameWide; x++ )
			{
				if( x + iFrameLeft >= iScreenWide ) continue;
				int iOut = ( ( y + iFrameTop ) * iScreenWide + ( x + iFrameLeft ) ) * 4;
				GifByteType colorIndex = pRasterBits[ iPixel ];
				if( colorIndex < pColorMap->ColorCount && colorIndex != nTransparentIndex )
				{
					GifColorType& color = pColorMap->Colors[ colorIndex ];
					pCurFrameBuffer[ iOut + 0 ] = color.Red;
					pCurFrameBuffer[ iOut + 1 ] = color.Green;
					pCurFrameBuffer[ iOut + 2 ] = color.Blue;
					pCurFrameBuffer[ iOut + 3 ] = 255;
				}
				// else retain prev frame buffer pixel data
				iPixel++;
			}
		}
	}

	// copy to output
	Q_memcpy( *ppOutFrameBuffer, pCurFrameBuffer, iScreenWide * iScreenTall * 4 );

	// update prev frame buffer depending on disposal method
	switch( nDisposalMethod )
	{
	case DISPOSE_BACKGROUND:
	{
		for( int y = iFrameTop; y < iFrameTop + iFrameTall && y < iScreenTall; y++ )
		{
			for( int x = iFrameLeft; x < iFrameLeft + iFrameWide && x < iScreenWide; x++ )
			{
				int idx = ( y * iScreenWide + x ) * 4;
				m_pPrevFrameBuffer[ idx + 0 ] = m_pImage->SBackGroundColor;
				m_pPrevFrameBuffer[ idx + 1 ] = m_pImage->SBackGroundColor;
				m_pPrevFrameBuffer[ idx + 2 ] = m_pImage->SBackGroundColor;
				m_pPrevFrameBuffer[ idx + 3 ] = 255;
			}
		}
		break;
	}
	case DISPOSE_PREVIOUS:
		break;
	case DISPOSAL_UNSPECIFIED:
	case DISPOSE_DO_NOT:
	default:
		Q_memcpy( m_pPrevFrameBuffer, pCurFrameBuffer, iScreenWide * iScreenTall * 4 );
		break;
	}

	stackfree( pCurFrameBuffer );
}

void CGIFHelper::GetFrameSize( int& iWidth, int& iHeight ) const
{
	if( !m_pImage )
	{
		iWidth = iHeight = 0;
		return;
	}

	GifImageDesc& imageDesc = m_pImage->SavedImages[ m_iSelectedFrame ].ImageDesc;
	iWidth = imageDesc.Width;
	iHeight = imageDesc.Height;
}

void CGIFHelper::GetScreenSize( int& iWide, int& iTall ) const
{
	if( !m_pImage )
	{
		iWide = iTall = 0;
		return;
	}

	iWide = m_pImage->SWidth;
	iTall = m_pImage->SHeight;
}
