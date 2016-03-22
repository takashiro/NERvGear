/************************************************************************************

Filename    :   FileLoader.cpp
Content     :
Created     :   August 13, 2014
Authors     :   John Carmack

Copyright   :   Copyright 2014 Oculus VR, LLC. All Rights reserved.

This source code is licensed under the BSD-style license found in the
LICENSE file in the Oculus360Photos/ directory. An additional grant
of patent rights can be found in the PATENTS file in the same directory.

************************************************************************************/

#include "VThread.h"
#include "FileLoader.h"
#include "Oculus360Photos.h"
#include "turbojpeg.h"
#include "OVR_TurboJpeg.h"

#include <VApkFile.h>
#include <VLog.h>

namespace NervGear {

VEventLoop		Queue1( 4000 );	// big enough for all the thumbnails that might be needed
VEventLoop		Queue3( 1 );

pthread_mutex_t QueueMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t	QueueWake = PTHREAD_COND_INITIALIZER;
bool			QueueHasCleared = true;

void * Queue1Thread( void * v )
{
	int result = pthread_setname_np( pthread_self(), "FileQueue1" );
	if ( result != 0 )
	{
		LOG( "InitFileQueue: pthread_setname_np failed %s", strerror( result ) );
	}

	// Process incoming messages until queue is empty
	for ( ; ; )
	{
		Queue1.wait();

		// If Queue3 hasn't finished our last output, sleep until it has.
		pthread_mutex_lock( &QueueMutex );
		while ( !QueueHasCleared )
		{
			// Atomically unlock the mutex and block until we get a message.
			pthread_cond_wait( &QueueWake, &QueueMutex );
		}
		pthread_mutex_unlock( &QueueMutex );

        VEvent event = Queue1.next();
        VString filename = event.data.toString();

        VEventLoop *queue = &Queue3;
        if (filename.endsWith("_nz.jpg")) {
			// cube map
			const char * const cubeSuffix[6] = { "_px.jpg", "_nx.jpg", "_py.jpg", "_ny.jpg", "_pz.jpg", "_nz.jpg" };

			MemBufferFile mbfs[6];

            VString filenameWithoutSuffix = filename.left(filename.size() - 7);
			int side = 0;
			for ( ; side < 6; side++ )
			{
                VString sideFilename = filenameWithoutSuffix + cubeSuffix[side];
                if ( !mbfs[side].loadFile( sideFilename.toLatin1().data() ) )
				{
                    const VApkFile &apk = VApkFile::CurrentApkFile();
                    if (!apk.read(sideFilename, mbfs[side])) {
						break;
					}
				}
                vInfo( "Queue1 loaded" << sideFilename);
			}
			if ( side >= 6 )
			{
				// if no error occured, post to next thread
                LOG( "%s.PostPrintf( \"%s %p %i %p %i %p %i %p %i %p %i %p %i\" )", "Queue3", event.name.toUtf8().data(),
						mbfs[0].buffer, mbfs[0].length,
						mbfs[1].buffer, mbfs[1].length,
						mbfs[2].buffer, mbfs[2].length,
						mbfs[3].buffer, mbfs[3].length,
						mbfs[4].buffer, mbfs[4].length,
						mbfs[5].buffer, mbfs[5].length );

                VVariantArray args;
                for (int i = 0; i < 6; i++) {
                    args << mbfs[i].buffer << mbfs[i].length;
                }
                queue->post(event.name, args);
				for ( int i = 0; i < 6; ++i )
				{
					// make sure we do not free the actual buffers because they're used in the next thread
					mbfs[i].buffer = NULL;
					mbfs[i].length = 0;
				}
			}
			else
			{
				// otherwise free the buffers we did manage to allocate
				for ( int i = 0; i < side; ++i )
				{
					mbfs[i].freeData();
				}
			}
		}
		else
		{
			// non-cube map
            MemBufferFile mbf( filename.toUtf8().data() );
			if ( mbf.length <= 0 || mbf.buffer == NULL )
			{
                const VApkFile &apk = VApkFile::CurrentApkFile();
                if (!apk.read(filename, mbf)) {
					continue;
				}
			}

            VVariantArray args;
            args << mbf.buffer << mbf.length;
            queue->post(event.name, args);
			mbf.buffer = NULL;
			mbf.length = 0;
		}
	}
	return NULL;
}

void * Queue3Thread( void * v )
{
	int result = pthread_setname_np( pthread_self(), "FileQueue3" );
	if ( result != 0 )
	{
		LOG( "InitFileQueue: pthread_setname_np failed %s", strerror( result ) );
	}

	// Process incoming messages until queue is empty
	for ( ; ; )
	{
		Queue3.wait();
        VEvent event = Queue3.next();

        vInfo("Queue3 msg =" << event.name);

		// Note that Queue3 has cleared the message
		pthread_mutex_lock( &QueueMutex );
		QueueHasCleared = true;
		pthread_cond_signal( &QueueWake );
		pthread_mutex_unlock( &QueueMutex );

        uint *b[6] = {};
		int blen[6];
        int numBuffers = 1;
        if (event.name != "cube") {
            b[0] = static_cast<uint *>(event.data.at(0).toPointer());
            blen[0] = event.data.at(1).toInt();
        } else {
            numBuffers = 6;
            int k = 0;
            for (int i = 0; i < 6; i++) {
                b[i] = static_cast<uint *>(event.data.at(k).toPointer());
                k++;
                blen[i] = event.data.at(k).toInt();
                k++;
            }
		}

#define USE_TURBO_JPEG
#if !defined( USE_TURBO_JPEG )
		stbi_uc * data[6];
#else
		unsigned char * data[6];
#endif

		int resolutionX = 0;
		int resolutionY = 0;
		int buffCount = 0;
		for ( ; buffCount < numBuffers; buffCount++ )
		{
			int	x, y;
			unsigned * b1 = b[buffCount];
			int b1len = blen[buffCount];

#if !defined( USE_TURBO_JPEG )
			int comp;
			data[buffCount] = stbi_load_from_memory( (const stbi_uc*)b1, b1len, &x, &y, &comp, 4 );
#else
			data[buffCount] = TurboJpegLoadFromMemory( (unsigned char*)b1, b1len, &x, &y );
#endif
			if ( buffCount == 0 )
			{
				resolutionX = x;
				resolutionY = y;
			}

			// done with the loading buffer now
			free( b1 );

			if ( data[buffCount] == NULL )
			{
				LOG( "LoadingThread: failed to load from buffer" );
				break;
			}
		}

		if ( buffCount != numBuffers )	// an image load failed, free everything and abort this load
		{
			for ( int i = 0; i < numBuffers; ++i )
			{
				free( data[i] );
				data[i] = NULL;
			}
		}
		else
		{
			if ( numBuffers == 1 )
			{
                OVR_ASSERT( data[0] != NULL );
                VVariantArray args;
                args << data[0] << resolutionX << resolutionY;
                ( ( Oculus360Photos * )v )->backgroundMessageQueue().post(event.name, std::move(args));
			}
			else
			{
                vAssert(numBuffers == 6);
                VVariantArray args;
                args << resolutionX << data[0] << data[1] << data[2] << data[3] << data[4] << data[5];
                ( ( Oculus360Photos * )v )->backgroundMessageQueue().post(event.name, std::move(args));
			}
		}
	}
	return NULL;
}

static const int NUM_QUEUE_THREADS = 2;

void InitFileQueue( App * app, Oculus360Photos * photos )
{
    // spawn the queue threads
	void * (*funcs[NUM_QUEUE_THREADS])( void *) = { Queue1Thread, Queue3Thread };

	for ( int i = 0; i < NUM_QUEUE_THREADS; i++ )
	{
		pthread_attr_t loadingThreadAttr;
		pthread_attr_init( &loadingThreadAttr );
		sched_param sparam;
        sparam.sched_priority = VThread::GetOSPriority(VThread::NormalPriority);
		pthread_attr_setschedparam( &loadingThreadAttr, &sparam );
		pthread_t	loadingThread;
		int createLoadingThreadErr = -1;
		if ( i == 1 )
		{
			createLoadingThreadErr = pthread_create( &loadingThread, &loadingThreadAttr, funcs[ i ], ( void * )photos );
		}
		else
		{
			createLoadingThreadErr = pthread_create( &loadingThread, &loadingThreadAttr, funcs[ i ], ( void * )app );
		}
		if ( createLoadingThreadErr != 0 )
		{
			LOG( "loadingThread: pthread_create returned %i", createLoadingThreadErr );
		}
	}
}

}	// namespace NervGear
