#ifndef GIFHELPER_H
#define GIFHELPER_H
#ifdef _WIN32
#pragma once
#endif

struct GifFileType;

//-----------------------------------------------------------------------------
// Purpose: Simple utility for decoding GIFs
//-----------------------------------------------------------------------------
class CGIFHelper
{
public:
	CGIFHelper( void ) : m_pImage( NULL ), m_pubPrevFrameBuffer( NULL ),
		m_iSelectedFrame( 0 ), m_dIterateTime( 0.0 )
	{}
	~CGIFHelper( void ) { CloseImage(); }

	bool OpenImage( CUtlBuffer &gif );
	// if you copied the frame data somewhere else, you can call this with bKeepMetadata set to true so
	// you can still call NextFrame, GetFrameCount, etc. functions without needing to have the image allocated twice
	void CloseImage( bool bKeepMetadata = false );

	bool NextFrame( void ); // iterates to the next frame, returns true if we have just looped
	int GetFrameCount( void ) const;
	int GetSelectedFrame( void ) const { return m_iSelectedFrame; }
	bool ShouldIterateFrame( void ) const { return m_dIterateTime < Plat_FloatTime(); }

	// Main methods for retrieving current frame data to a format that the engine understands.
	// currently supports:
	//  - IMAGE_FORMAT_DXT1_RUNTIME
	//    cheap on memory but WILL nearest-neighbor scale up the frame to the closest power of two
	//  - IMAGE_FORMAT_RGB(A)888(8) and friends
	//    raw format, very expensive on memory
	// size of pubOutFrameBuffer should be the return value of FrameSize( eFormat )
	// this function is somewhat expensive to call so try to spread usage across different frames!
	void FrameData( ImageFormat eFormat, uint8 *pubOutFrameBuffer );
	int FrameSize( ImageFormat eFormat, int &nWide, int &nTall );


private:
	GifFileType *m_pImage;
	uint8 *m_pubPrevFrameBuffer;
	int m_iSelectedFrame;
	double m_dIterateTime;
};

#endif //GIFHELPER_H