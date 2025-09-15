#include "cbase.h"
#include "tier0/vprof.h"
#include "gifhelper.h"
#include "gif_lib.h"

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
static int GifReadData( GifFileType *pFile, GifByteType *pubBuffer, int cubBuffer )
{
    auto &gif = *( CUtlBuffer * )pFile->UserData;

    int nBytesToRead = Min( cubBuffer, gif.GetBytesRemaining() );
    if ( nBytesToRead > 0 )
        gif.Get( pubBuffer, nBytesToRead );

    return nBytesToRead;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CGIFHelper::OpenImage( CUtlBuffer &gif )
{
    if ( m_pImage )
    {
        CloseImage();
    }

    int nError;
    m_pImage = DGifOpen( &gif, GifReadData, &nError );
    if ( !m_pImage )
    {
        DevWarning( "[CGIFHelper] Failed to open GIF image: %s\n", GifErrorString( nError ) );
        return false;
    }

    if ( DGifSlurp( m_pImage ) != GIF_OK )
    {
        DevWarning( "[CGIFHelper] Failed to slurp GIF image: %s\n", GifErrorString( m_pImage->Error ) );
        CloseImage();
        return false;
    }

    int nWide, nTall;
    m_pubPrevFrameBuffer = new uint8[ FrameSize( IMAGE_FORMAT_RGBA8888, nWide, nTall ) ];
    FrameData( IMAGE_FORMAT_RGBA8888, m_pubPrevFrameBuffer );

    return true;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CGIFHelper::CloseImage( bool bKeepMetadata /*= false*/ )
{
    if ( !m_pImage )
        return;

    // delete stuff that we always want removed..
    if ( m_pubPrevFrameBuffer )
    {
        delete[] m_pubPrevFrameBuffer;
        m_pubPrevFrameBuffer = NULL;
    }

    if ( m_pImage->Image.ColorMap )
    {
        GifFreeMapObject( m_pImage->Image.ColorMap );
        m_pImage->Image.ColorMap = NULL;
    }

    if ( m_pImage->SColorMap )
    {
        GifFreeMapObject( m_pImage->SColorMap );
        m_pImage->SColorMap = NULL;
    }

    if ( m_pImage->SavedImages )
    {
        // delete everything except extension blocks
        for ( SavedImage *pFrame = m_pImage->SavedImages; pFrame < m_pImage->SavedImages + m_pImage->ImageCount; pFrame++ )
        {
            if ( pFrame->ImageDesc.ColorMap )
            {
                GifFreeMapObject( pFrame->ImageDesc.ColorMap );
                pFrame->ImageDesc.ColorMap = NULL;
            }

            if ( pFrame->RasterBits )
            {
                free( pFrame->RasterBits );
                pFrame->RasterBits = NULL;
            }
        }
    }

    // delete everything else if we don't want to keep metadata
    if ( !bKeepMetadata )
    {
        int nError;
        if ( DGifCloseFile( m_pImage, &nError ) != GIF_OK )
        {
            DevWarning( "[CGIFHelper] Failed to close GIF image: %s\n", GifErrorString( nError ) );
        }
        m_pImage = NULL;
        m_iSelectedFrame = 0;
        m_dIterateTime = 0.0;
    }
}

//-----------------------------------------------------------------------------
// Purpose: Iterates the current frame index
// Output : true - looped back to frame 0
//-----------------------------------------------------------------------------
bool CGIFHelper::NextFrame( void )
{
    if ( !m_pImage )
        return false;

    m_iSelectedFrame++;

    if ( m_iSelectedFrame >= m_pImage->ImageCount )
    {
        // Loop
        m_iSelectedFrame = 0;
    }

    GraphicsControlBlock gcb;
    if ( DGifSavedExtensionToGCB( m_pImage, m_iSelectedFrame, &gcb ) == GIF_OK )
    {
        // simulates web browsers "throttling" short time delays so
        // gif animation speed is similar to Steam's
        static const double k_dMinTime = .02, k_dDefaultTime = .1; // Chrome defaults

        double dDelayTime = gcb.DelayTime * .01;
        m_dIterateTime = ( dDelayTime < k_dMinTime ? k_dDefaultTime : dDelayTime ) + Plat_FloatTime();
    }

    return m_iSelectedFrame == 0;
}

//-----------------------------------------------------------------------------
// Purpose: Gets the total number of frames in the open image
// Output : int
//-----------------------------------------------------------------------------
int CGIFHelper::GetFrameCount( void ) const
{
    return m_pImage ? m_pImage->ImageCount : 0;
}

//-----------------------------------------------------------------------------
// Purpose: Copies the current frame data to a buffer
// Input  :	eFormat - format of the output buffer
//			pubOutFrameBuffer - output buffer, size needs to be the return value
//			of FrameSize( eFormat )
//-----------------------------------------------------------------------------
void CGIFHelper::FrameData( ImageFormat eFormat, uint8 *pubOutFrameBuffer )
{
    VPROF( "CGIFHelper::GetRGBA" );

    if ( !m_pImage || !m_pImage->SavedImages->RasterBits )
        return;

    const int cBytesPerPixel = ImageLoader::SizeInBytes( IMAGE_FORMAT_RGBA8888 );

    int nTargetWide, nTargetTall;
    if ( FrameSize( eFormat, nTargetWide, nTargetTall ) == -1 )
    {
        AssertMsg( NULL, "Unsupported format \"%s\" supplied to CGIFHelper::FrameData", ImageLoader::GetName( eFormat ) );
        return;
    }
    const int nTargetStride = nTargetWide * cBytesPerPixel;

    const GifImageDesc &imageDesc = m_pImage->SavedImages[ m_iSelectedFrame ].ImageDesc;
    const ColorMapObject *pColorMap = imageDesc.ColorMap ? imageDesc.ColorMap : m_pImage->SColorMap;

    const int nScreenWide = m_pImage->SWidth;
    const int nScreenTall = m_pImage->SHeight;
    const int nScreenStride = nScreenWide * cBytesPerPixel;

    int nTransparentIndex = NO_TRANSPARENT_COLOR;
    int nDisposalMethod = DISPOSAL_UNSPECIFIED;

    GraphicsControlBlock gcb;
    if ( DGifSavedExtensionToGCB( m_pImage, m_iSelectedFrame, &gcb ) == GIF_OK )
    {
        nTransparentIndex = gcb.TransparentColor;
        nDisposalMethod = gcb.DisposalMode;
    }

    const int cubScreenSize = nScreenStride * nScreenTall;
    const int cubTargetSize = nTargetStride * nTargetTall;

    uint8 *pubCompositionFrameBuffer = ( uint8 * )stackalloc( cubScreenSize );
    Q_memcpy( pubCompositionFrameBuffer, m_pubPrevFrameBuffer, cubScreenSize );

    uint8 *pubTargetFrameBuffer = NULL;
    if ( nTargetWide == nScreenWide && nTargetTall == nScreenTall )
    {
        pubTargetFrameBuffer = pubCompositionFrameBuffer;
    }
    else
    {
        pubTargetFrameBuffer = ( uint8 * )stackalloc( cubTargetSize );
        Q_memset( pubTargetFrameBuffer, 0, cubTargetSize );
    }

    auto lambdaComputeFrame = [ & ]( int nRowOffset = 0, int nRowIncrement = 1 )
    {
        int iPixel = nRowOffset * imageDesc.Width;
        for ( int y = nRowOffset; y < imageDesc.Height; y += nRowIncrement )
        {
            const int nScreenY = y + imageDesc.Top;
            if ( nScreenY >= nScreenTall )
            {
                iPixel += imageDesc.Width;
                continue;
            }

            uint8 *pubDest = pubCompositionFrameBuffer +
                ( nScreenY * nScreenStride ) +
                ( imageDesc.Left * cBytesPerPixel );

            for ( int x = 0; x < imageDesc.Width; x++, iPixel++ )
            {
                const int nScreenX = x + imageDesc.Left;
                if ( nScreenX >= nScreenWide )
                {
                    pubDest += cBytesPerPixel;
                    continue;
                }

                GifByteType idx = m_pImage->SavedImages[ m_iSelectedFrame ].RasterBits[ iPixel ];
                if ( idx < pColorMap->ColorCount && idx != nTransparentIndex )
                {
                    const GifColorType &color = pColorMap->Colors[ idx ];
                    pubDest[ 0 ] = color.Red;
                    pubDest[ 1 ] = color.Green;
                    pubDest[ 2 ] = color.Blue;
                    pubDest[ 3 ] = 255;
                }
                pubDest += cBytesPerPixel;
            }
        }

        if ( pubTargetFrameBuffer != pubCompositionFrameBuffer )
        {
            for ( int y = 0; y < nTargetTall; y++ )
            {
                int nSrcY = ( y * nScreenTall ) / nTargetTall;
                uint8 *pubDestRow = pubTargetFrameBuffer + y * nTargetStride;
                uint8 *pubSrcRow = pubCompositionFrameBuffer + nSrcY * nScreenStride;

                for ( int x = 0; x < nTargetWide; x++ )
                {
                    int nSrcX = ( x * nScreenWide ) / nTargetWide;

                    uint8 *pubDestPixel = pubDestRow + x * cBytesPerPixel;
                    uint8 *pubSrcPixel = pubSrcRow + nSrcX * cBytesPerPixel;

                    pubDestPixel[ 0 ] = pubSrcPixel[ 0 ];
                    pubDestPixel[ 1 ] = pubSrcPixel[ 1 ];
                    pubDestPixel[ 2 ] = pubSrcPixel[ 2 ];
                    pubDestPixel[ 3 ] = pubSrcPixel[ 3 ];
                }
            }
        }
    };

    if ( imageDesc.Interlace )
    {
        static const int k_rowOffsets[] = { 0, 4, 2, 1 };
        static const int k_rowIncrements[] = { 8, 8, 4, 2 };
        for ( int nPass = 0; nPass < 4; nPass++ )
        {
            lambdaComputeFrame( k_rowOffsets[ nPass ], k_rowIncrements[ nPass ] );
        }
    }
    else
    {
        lambdaComputeFrame();
    }

    // update prev frame buffer depending on disposal method
    switch ( nDisposalMethod )
    {
    case DISPOSE_BACKGROUND:
        if ( m_pImage->SBackGroundColor < m_pImage->SColorMap->ColorCount )
        {
            const GifColorType &color =
                m_pImage->SColorMap->Colors[ m_pImage->SBackGroundColor ];
            const int nFillTall = Min( imageDesc.Height, nScreenTall - imageDesc.Top );
            const int nFillWide = Min( imageDesc.Width, nScreenWide - imageDesc.Left );
            uint32 unFillColor = ( color.Red ) | ( color.Green << 8 ) | ( color.Blue << 16 ) | ( 0xFF << 24 );
            for ( int y = 0; y < nFillTall; y++ )
            {
                uint32 *punRow = reinterpret_cast< uint32 * >(
                    m_pubPrevFrameBuffer +
                    ( ( y + imageDesc.Top ) * nScreenStride ) +
                    imageDesc.Left * cBytesPerPixel );
                for ( int x = 0; x < nFillWide; x++ )
                {
                    punRow[ x ] = unFillColor;
                }
            }
        }
        break;
    case DISPOSE_PREVIOUS:
        break;
    case DISPOSAL_UNSPECIFIED:
    case DISPOSE_DO_NOT:
    default:
        Q_memcpy( m_pubPrevFrameBuffer, pubCompositionFrameBuffer, cubScreenSize );
        break;
    }

    // convert to the desired format
    ImageLoader::ConvertImageFormat(
        pubTargetFrameBuffer,
        IMAGE_FORMAT_RGBA8888,
        pubOutFrameBuffer,
        eFormat,
        nTargetWide,
        nTargetTall
    );
}


//-----------------------------------------------------------------------------
// Purpose: Gets the count of bytes and screen resolution required to get
//			the current frame data with the specified image format
// Input  :	eFormat - format of the output buffer
//-----------------------------------------------------------------------------
int CGIFHelper::FrameSize( ImageFormat eFormat, int &nWide, int &nTall )
{
    nWide = nTall = 0;
    if ( !m_pImage )
    {
        return -1;
    }

    switch ( eFormat )
    {
    case IMAGE_FORMAT_RGBA8888:
    case IMAGE_FORMAT_BGRA8888:
    case IMAGE_FORMAT_ARGB8888:
    case IMAGE_FORMAT_ABGR8888:
    case IMAGE_FORMAT_RGB888:
    case IMAGE_FORMAT_BGR888:
        nWide = m_pImage->SWidth;
        nTall = m_pImage->SHeight;
        return nWide * nTall * ImageLoader::SizeInBytes( eFormat );
    case IMAGE_FORMAT_DXT1_RUNTIME:
    {
        // DXT1RT requires the resolution to be a power of two
        nWide = 1;
        while ( nWide < m_pImage->SWidth )
        {
            nWide <<= 1;
        }
        nTall = 1;
        while ( nTall < m_pImage->SHeight )
        {
            nTall <<= 1;
        }
        return ImageLoader::GetMemRequired( nWide, nTall, 1, eFormat, false );
    }
    }
    return -1;
}