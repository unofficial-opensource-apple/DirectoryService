/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/*!
 * @header DirServices
 * API implementations.
 */

#include "DirServices.h"
#include "DirServicesConst.h"
#include "DirServicesUtils.h"
#include "DirServicesTypes.h"
#include "DirServicesPriv.h"
#include "PrivateTypes.h"
#include "CDSRefMap.h"

#include "DSLogException.h"
#include "CMessaging.h"
#include "DSMutexSemaphore.h"

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>				// for stat()
#include <sys/sysctl.h>				// for sysctl()
#include <unistd.h>

#include <mach/mach.h>				// mach ipc approach to IsDirServiceRunning
									// versus searching entire process space
#include <mach/mach_error.h>
#include <servers/bootstrap.h>

#define	kDSFWDefaultRemotePort 625 //TODO need final port number
#define kDSFWMaxRemoteConnections 8

// Globals  ----------------------------------------------------------------------

static CMessaging		   *gMessageTable[kDSFWMaxRemoteConnections+1]
													= {nil,nil,nil,nil,nil,nil,nil,nil,nil};
static UInt32				gMaxEndpoints			= kDSFWMaxRemoteConnections + 1;
													//maximum number of distinct endpoints for client
static UInt32				gDSConnections  		= 0;	//keep track of open mach DS sessions
static DSMutexSemaphore		*gLock					= NULL;	//lock on modifying these globals
static dsBool				gNormalDaemonInUse		= false; //client allowed to make normal connections via mach to DS daemon
static dsBool				gLocalDaemonInUse		= false; //client allowed to make normal connections via mach to DS local daemon
static UInt32				gTranslateBit			= 0;	//bit set so server knows if FW is running on Intel translated to big endian
static dsBool				gProcessForked   		= true; // we start as true so we can lazily clean up the first session
static pthread_once_t		_gGlobalsInitialized	= PTHREAD_ONCE_INIT;

pid_t						gProcessPID				= 0;	//process PID of the client
CDSRefMap					*gFWRefMap				= NULL;
CDSRefTable					*gFWRefTable			= NULL;

void CheckToCleanUpLostTCPConnection ( tDirStatus *inStatus, UInt32 inMessageIndex, UInt32 lineNumber );

static void __ForkPrepare( void )
{
	// let's grab our global lock so we can do changes in the child
	gLock->WaitLock();
}

static void __ForkParent( void )
{
	// all we do is unlock, really nothing to do
	gLock->SignalLock();
}

static void __ForkChild( void )
{
	gProcessPID = getpid();

	gProcessForked = true;
	gNormalDaemonInUse = false;
	gLocalDaemonInUse = false;
	
	gLock->SignalLock();
}

static void __InitGlobals( void )
{
	gLock = new DSMutexSemaphore( "API Global::gLock" );
	gProcessPID = getpid();
	
	gFWRefMap = new CDSRefMap;
	gFWRefTable = new CDSRefTable;
	
#if __BIG_ENDIAN__
	// Check to see if we are running translated, only needs to be done once
	int mib[] = { CTL_KERN, KERN_CLASSIC, gProcessPID };
	size_t len = sizeof(int);
	int ret = 0;
	if (sysctl (mib, 3, &ret, &len, NULL, 0) == 0 && ret == 1)
	{
		// Running on Intel under translation
		gTranslateBit = 1;
	}
#endif
	
	pthread_atfork( __ForkPrepare, __ForkParent, __ForkChild );
}

static void __ResetAllSessions( void )
{
	gFWRefMap->ClearAllMaps();
	gFWRefTable->ClearAllTables();
	
	for (UInt32 tableIndex = 1; tableIndex < gMaxEndpoints; tableIndex++)
	{
		if ( gMessageTable[tableIndex] != nil )
		{
			gMessageTable[tableIndex]->Lock();
			gMessageTable[tableIndex]->CloseTCPEndpoint();
			gMessageTable[tableIndex]->Unlock();
			delete(gMessageTable[tableIndex]);
			gMessageTable[tableIndex] = nil;
		}
	}	
	
	if ( gMessageTable[0] != nil )
	{
		gMessageTable[0]->Lock();
		gMessageTable[0]->CloseCommPort(); // don't check status
		gMessageTable[0]->Unlock();
	}
	
	gLocalDaemonInUse = false;
	gNormalDaemonInUse = false;
	gDSConnections	= 0;
}

//--------------------------------------------------------------------------------------------------
//
//	Name:	dsOpenDirService
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsOpenDirService ( tDirReference *outDirRef )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;

    pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfNilMacro( outDirRef, eDSNullParameter );
		gLock->WaitLock();
		try
		{
			//a client process uses a SINGLE CMesssaging for mach and therefore a single EndPt
			//a client can hold several dir refs at once all handled through the same mach port
			do
			{
				if (gLocalDaemonInUse)
				{
					siStatus = eDSLocalDSDaemonInUse;
					break;
				}
				
				// if the daemon is not registered, nothing to do, just return the error
				if ( (siStatus = dsIsDirServiceRunning()) != eDSNoErr )
					break;				
				
				if ( gProcessForked ) // we have forked the process, need to reset all
				{
					__ResetAllSessions();
					gProcessForked = false;
				}
				
				if ( gMessageTable[0] == nil )
				{
					gMessageTable[0] = new CMessaging(true, gTranslateBit);
					
					siStatus = gMessageTable[0]->OpenCommPort(false);
				}
				else
				{
					gMessageTable[0]->ChangeLocalDaemonUse( false ); // change the state
				}
				
				gDSConnections++; //increment the number of DS connections open
				gNormalDaemonInUse = true;
				break;
			} while(1);
		}
		catch ( SInt32 err )
		{
			gLock->SignalLock();
			throw err;
		}
		catch ( ... )
		{
			gLock->SignalLock();
			throw (SInt32)eDSCannotAccessSession;
		}
		gLock->SignalLock();
		
		LogThenThrowIfDSErrorMacro( siStatus );
		LogThenThrowIfNilMacro( gMessageTable[0], eMemoryAllocError );

		gMessageTable[0]->Lock();
		try
		{
			gMessageTable[0]->ClearMessageBlock();
			
			// **************** Send the message ****************
			siStatus = gMessageTable[0]->SendInlineMessage( kOpenDirService );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = (tDirStatus)gMessageTable[0]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[0]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outDirRef != nil )
			{
				// Get the directory reference
				siStatus = gMessageTable[0]->Get_Value_FromMsg( outDirRef, ktDirRef );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDirRef );
			}
		}
		catch( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[0]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	return( outResult );

} // dsOpenDirService


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsOpenDirServiceProxy
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsOpenDirServiceProxy (	tDirReference	   *outDirRef,
									const char		   *inIPAddress,
									UInt32				inIPPort,
									tDataNodePtr		inAuthMethod,				//KW let's use default
									tDataBufferPtr		inAuthStepData,
									tDataBufferPtr		outAuthStepDataResponse,	//KW no need
									tContextData	   *ioContinueData )			//KW no need
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;
	UInt32			tableIndex	= 0;
	tDataBufferPtr	versBuff	= nil;
	UInt32			serverVersion = 0;

    pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfNilMacro( outDirRef, eDSNullParameter );
		
		gLock->WaitLock();
		try
		{
			//a client process uses a separate CMesssaging for each TCP endpoint
			//which in turn is tied to a single dir ref
			//for now we have up to kDSFWMaxRemoteConnections = "gMaxEndpoints - 1" available TCP endpoints
			
			if ( gProcessForked ) // we have forked the process, need to reset all
			{
				__ResetAllSessions();
				gProcessForked = false;
			}
						
			//search for the next available gMessageTable slot
			for (tableIndex = 1; tableIndex < gMaxEndpoints; tableIndex++)
			{
				if ( gMessageTable[tableIndex] == nil )
				{
					messageIndex = tableIndex;
					break;
				}
			}
			
			//don't allow more than max sessions to be opened
			LogThenThrowIfTrueMacro( (messageIndex == 0), eDSMaxSessionsOpen );
			
			//TODO what happens when there is a network transition ie. server daemon on this machine
			//will recycle BUT this client will not so how do we deal with the remote connections?
			//ref cleanup on the FW side will be an issue as well
			if ( gMessageTable[messageIndex] == nil )
			{
				gMessageTable[messageIndex] = new CMessaging(false, gTranslateBit);
				if (gMessageTable[messageIndex] != nil)
				{
					if (inIPPort != 0)
					{
						siStatus = gMessageTable[messageIndex]->ConfigTCP(inIPAddress, inIPPort);
					}
					else
					{
						siStatus = gMessageTable[messageIndex]->ConfigTCP(inIPAddress, kDSFWDefaultRemotePort);
					}
					LogThenThrowIfDSErrorMacro( siStatus );
					
					siStatus = gMessageTable[messageIndex]->OpenTCPEndpoint();
					LOG2( kStdErr, "DirServices::dsOpenDirServiceProxy: Correlate the messageIndex: %d with the actual CMessaging class ptr %d.", messageIndex, (UInt32)gMessageTable[messageIndex] );
				}
			}
		}
		catch( SInt32 err )
		{
			gLock->SignalLock();
			throw err;
		}
		catch( ... )
		{
			gLock->SignalLock();
			throw (SInt32)eDSCannotAccessSession;
		}
		gLock->SignalLock();
		
		LogThenThrowIfDSErrorMacro( siStatus );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eMemoryAllocError );

		//go ahead and pack the message to send
		
		gMessageTable[messageIndex]->Lock();
		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAuthMethod, eDSNullAutMethod, eDSEmptyAuthMethod );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAuthStepData, eDSNullAuthStepData, eDSEmptyAuthStepData );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( outAuthStepDataResponse, eDSNullAuthStepDataResp, eDSEmptyAuthStepDataResp );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the version info DSProxy1.3
			versBuff = dsDataBufferAllocate( 0, 16 ); //dir ref not needed
			LogThenThrowIfNilMacro( versBuff, eMemoryAllocError );
			memcpy( &(versBuff->fBufferData), "DSProxy1.3", 10 );
			versBuff->fBufferLength = 10;
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( versBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			siStatus = dsDataBufferDeAllocate( 0, versBuff ); //dir ref not needed
			LogThenThrowThisIfDSErrorMacro( siStatus, eMemoryError );
			
			// Add the auth method
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAuthMethod, kAuthMethod );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the auth step data
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAuthStepData, kAuthStepBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the auth step response
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( outAuthStepDataResponse, kAuthResponseBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			if ( ioContinueData != nil )
			{
				// Add the context data
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (UInt32)*ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterReceiveError - 4 );
			}
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kOpenDirServiceProxy );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = (tDirStatus)gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Get the server DSProxy version if it exists
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&serverVersion, kNodeCount );
			siStatus = eDSNoErr;
			gMessageTable[messageIndex]->SetServerVersion(serverVersion);
			
			if ( outDirRef != nil )
			{
				tDirNodeReference	aRef = 0;
				// Get the directory reference
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( &aRef, ktDirRef );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDirRef );
				gFWRefMap->NewDirRefMap( outDirRef, gProcessPID, aRef, messageIndex );
			}
		}
		catch ( SInt32 err )
		{
			if ( (messageIndex != 0) && ( gMessageTable[messageIndex] != nil ) )
			{
				gLock->WaitLock();
				if (gMessageTable[messageIndex] != nil )
				{
					gMessageTable[messageIndex]->CloseTCPEndpoint();
					gMessageTable[messageIndex]->Unlock();
					delete(gMessageTable[messageIndex]);
					gMessageTable[messageIndex] = nil;
				}
				gLock->SignalLock();
			}
			throw err;
		}
		catch ( ... )
		{
			if ( (messageIndex != 0) && ( gMessageTable[messageIndex] != nil ) )
			{
				gLock->WaitLock();
				if (gMessageTable[messageIndex] != nil )
				{
					gMessageTable[messageIndex]->CloseTCPEndpoint();
					gMessageTable[messageIndex]->Unlock();
					delete(gMessageTable[messageIndex]);
					gMessageTable[messageIndex] = nil;
				}
				gLock->SignalLock();
			}
			throw (SInt32) eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	return( outResult );

} // dsOpenDirServiceProxy


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsOpenDirServiceLocal
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsOpenDirServiceLocal ( tDirReference *outDirRef, const char *inFilePath )
{
	tDirStatus		outResult			= eDSNoErr;
	SInt32			siStatus			= eDSNoErr;
	tDataBufferPtr	fpBuff				= nil;
	char			newPath[PATH_MAX+1]	= { 0 };
	struct stat		statResult;

    pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfNilMacro( outDirRef, eDSNullParameter );
		
		// see if we need to set a default path
		if ( DSIsStringEmpty(inFilePath) || strcmp(inFilePath, "Default") == 0 )
			strlcpy( newPath, "/var/db/dslocal/nodes/Default/", sizeof(newPath) );
		else
			strlcpy( newPath, inFilePath, sizeof(newPath) );
		
		unsigned length = strlen( newPath );
		if ( length >= PATH_MAX )
			LogThenThrowThisIfDSErrorMacro( siStatus, eDSInvalidFilePath );
		
		// ensure we have a slash
		if ( newPath[length-1] != '/' )
			newPath[length++] = '/';
		
		// see if we need with just "/dslocal/nodes", if so add /Default
		char *dirPath = strstr( newPath, "/dslocal/nodes/" );
		if ( dirPath != NULL && strcmp(dirPath, "/dslocal/nodes/") == 0 ) {
			strlcat( newPath, "Default/", sizeof(newPath) );
		}
		
		gLock->WaitLock();
		try
		{
			//a client process uses a SINGLE CMesssaging for mach and therefore a single EndPt
			//a client can hold several dir refs at once all handled through the same mach port
			
			do
			{
				if ( gProcessForked )  // we have forked the process, need to reset all
				{
					__ResetAllSessions();
					gProcessForked	= false;
				}
				
				//check that this is a valid file path
				LogThenThrowIfTrueMacro( lstat(newPath, &statResult) != 0, eDSInvalidFilePath );
				
				// ok let's see if we have a real daemon and if someone is trying to modify the local DB, if so return error
				if ( dsIsDirServiceRunning() == eDSNoErr )
				{
					struct stat localDirStat;
					
					// let's see if we are trying to get to the same place
					if ( lstat("/var/db/dslocal/nodes/Default", &localDirStat) == 0 )
					{
						// if these are the same files
						if ( statResult.st_ino == localDirStat.st_ino && statResult.st_dev == localDirStat.st_dev ) {
							gLock->SignalLock();
							return dsOpenDirService( outDirRef );
						}
					}
				}
				
				if (gNormalDaemonInUse)
				{
					siStatus = eDSNormalDSDaemonInUse;
					break;
				}
				
				// if the daemon is not registered, nothing to do, just return the error
				if ( (siStatus = dsIsDirServiceLocalRunning()) != eDSNoErr )
					break;
				
				if ( gMessageTable[0] == nil )
				{
					gMessageTable[0] = new CMessaging(true, gTranslateBit);
					
					siStatus = gMessageTable[0]->OpenCommPort(true);
				}
				else
				{
					gMessageTable[0]->ChangeLocalDaemonUse( true ); // change the state
				}
				
				gDSConnections++; //increment the number of DS connections open
				gLocalDaemonInUse = true;
				break;
			} while(1);
		}
		catch( SInt32 err )
		{
			gLock->SignalLock();
			throw err;
		}
		catch( ... )
		{
			gLock->SignalLock();
			throw (SInt32) eDSCannotAccessSession;
		}
		gLock->SignalLock();
		
		LogThenThrowIfDSErrorMacro( siStatus );
		LogThenThrowIfNilMacro( gMessageTable[0], eMemoryAllocError );

		gMessageTable[0]->Lock();
		
		try
		{
			gMessageTable[0]->ClearMessageBlock();
			
			// Add the file path to the ds local node DB
			fpBuff = dsDataNodeAllocateString( 0, newPath );
			LogThenThrowIfNilMacro( fpBuff, eMemoryAllocError );

			siStatus = gMessageTable[0]->Add_tDataBuff_ToMsg( fpBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			siStatus = dsDataBufferDeAllocate( 0, fpBuff ); //dir ref not needed
			LogThenThrowThisIfDSErrorMacro( siStatus, eMemoryError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[0]->SendInlineMessage( kOpenDirServiceLocal );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = (tDirStatus)gMessageTable[0]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[0]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outDirRef != nil )
			{
				// Get the directory reference
				siStatus = gMessageTable[0]->Get_Value_FromMsg( outDirRef, ktDirRef );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDirRef );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus) err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[0]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}
	
	return( outResult );

} // dsOpenDirServiceLocal


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsCloseDirService
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsCloseDirService ( tDirReference inDirRef )
{
	tDirStatus		outResult	= eDSNoErr;
	UInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inDirRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirRef,eDirectoryRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the directory reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inDirRef, eDirectoryRefType, gProcessPID), ktDirRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the connections count
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gDSConnections, kNodeCount );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kCloseDirService );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = (tDirStatus)gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			//now remove the dir reference here if it exists
			gFWRefMap->RemoveDirRef( inDirRef, gProcessPID );
		}
		catch( SInt32 err )
		{
			gMessageTable[messageIndex]->Unlock();
			throw err;
		}
		catch( ... )
		{
			gMessageTable[messageIndex]->Unlock();
			throw (SInt32) eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
		
		gLock->WaitLock();
		
		try
		{
			//a client calls this and the mach EndPt will be closed ONLY
			//if there are NO other dir refs outstanding
			//ref count the cleanup using gDSConnections below
			if ((messageIndex == 0) && (gDSConnections > 0))
			{
				gDSConnections--; //decrement the number of DS mach connections open
			}
			
			if (gDSConnections == 0)
			{
				// just close the mach port since we have no sessions
				if ( gMessageTable[0] != nil )
				{
					gMessageTable[0]->Lock();
					gMessageTable[0]->CloseCommPort(); // don't check status
					gMessageTable[0]->Unlock();
				}
				
				gLocalDaemonInUse = false;
				gNormalDaemonInUse = false;
			}
			
			//always clean up the TCP Endpt upon the close
			if ((messageIndex != 0) && ( gMessageTable[messageIndex] != nil ))
			{
				gMessageTable[messageIndex]->Lock();
				gMessageTable[messageIndex]->CloseTCPEndpoint();
				gMessageTable[messageIndex]->Unlock();
				delete(gMessageTable[messageIndex]);
				gMessageTable[messageIndex] = nil;
			}
		}
		catch( SInt32 err )
		{
			outResult = (tDirStatus) err;
		}
		catch( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gLock->SignalLock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsCloseDirService


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsAddChildPIDToReference
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsAddChildPIDToReference ( tDirReference inDirRef, SInt32 inValidChildPID, UInt32 inValidAPIReferenceToGrantChild )
//accept only NODE references
{
	tDirStatus		outResult	= eDSNoErr;
	UInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inDirRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirRef, eDirectoryRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();
		
		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the directory reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inDirRef, eDirectoryRefType, gProcessPID), ktDirRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the child process PID
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (SInt32)inValidChildPID, ktPidRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the node reference to which access to the child is to be granted
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inValidAPIReferenceToGrantChild, eNodeRefType, gProcessPID), ktGenericRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kAddChildPIDToReference );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = (tDirStatus)gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsAddChildPIDToReference


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsIsDirServiceRunning
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsIsDirServiceRunning ( void )
{
	tDirStatus		outResult	= eServerNotRunning;
	mach_port_t		bPort		= 0;

	// If we can lookup the port with the DirectoryService port name, then DirectoryService is already running
	if ( bootstrap_look_up(bootstrap_port, kDSStdMachPortName, &bPort) == 0 )
	{
		mach_port_mod_refs( mach_task_self(), bPort, MACH_PORT_RIGHT_SEND, -1 );
		outResult = eDSNoErr;
	}

	return( outResult );

} // dsIsDirServiceRunning


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsIsDirServiceLocalRunning //can't reuse code here since other routine was exported already
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsIsDirServiceLocalRunning ( void )
{
	tDirStatus		outResult	= eServerNotRunning;
	mach_port_t		bPort		= 0;

	// If we can lookup the port with the DirectoryServiceLocal port name, then DirectoryService is already running
	if ( bootstrap_look_up(bootstrap_port, kDSStdMachLocalPortName, &bPort) == 0 )
	{
		// deallocate the send right
		mach_port_mod_refs( mach_task_self(), bPort, MACH_PORT_RIGHT_SEND, -1 );
		outResult = eDSNoErr;
	}

	return( outResult );

} // dsIsDirServiceLocalRunning


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetDirNodeCount
//
//	Params:	inDirReference			->	directory Reference Established with dsOpenDirService
//			outDirectoryNodeCount	->	function outResult eDSNoErr, contains count of the total number
//										 of nodes in the directory...
//
//	Notes:	Get the count of the total number of DirNodes in the Directory System
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetDirNodeCount ( tDirReference inDirRef, UInt32 *outNodeCount )
{
	tDirStatus		outResult	= eDSNoErr;
	UInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfNilMacro( outNodeCount, eDSNullParameter );
		LogThenThrowIfTrueMacro(inDirRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirRef, eDirectoryRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the directory reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inDirRef, eDirectoryRefType, gProcessPID), ktDirRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetDirNodeCount );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = (tDirStatus)gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outNodeCount != nil )
			{
				// Get the node count
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( outNodeCount, kNodeCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoNodeCount );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetDirNodeCount


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetDirNodeCountWithInfo
//
//	Params:	inDirReference			->	directory Reference Established with dsOpenDirService
//			outDirectoryNodeCount	->	function outResult eDSNoErr, contains count of the total number
//										 of nodes in the directory...
//			outDirectoryNodeChangeToken
//									->	token that changes every time any registered node changes
//
//	Notes:	Get the count of the total number of DirNodes in the Directory System and
//			determine whether the registered nodes have changed or not
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetDirNodeCountWithInfo ( tDirReference inDirRef, UInt32 *outNodeCount, UInt32 *outDirectoryNodeChangeToken )
{
	tDirStatus		outResult	= eDSNoErr;
	UInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inDirRef == 0, eDSInvalidReference);
		LogThenThrowIfTrueMacro(( outNodeCount == nil ) && ( outDirectoryNodeChangeToken == nil), eDSNullParameter);
		//need at least one container to make the call worth it
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirRef, eDirectoryRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the directory reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inDirRef, eDirectoryRefType, gProcessPID), ktDirRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetDirNodeChangeToken );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = (tDirStatus)gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outNodeCount != nil )
			{
				// Get the node count
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( outNodeCount, kNodeCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoNodeCount );
			}
			
			if ( outDirectoryNodeChangeToken != nil )
			{
				// Get the node change token
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( outDirectoryNodeChangeToken, kNodeChangeToken );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoNodeChangeToken );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetDirNodeCountWithInfo


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetDirNodeList
//
//	Params:	inDirReference			->	directory Reference Established with dsOpenDirService
//			inOutDataBufferPtr		->	contains a client allocated buffer to store results..
//										 data is extracted with dsGetDirNodeName
//			outDirNodeCount			->	number of directory node names contained in dataBuffer
//			inOutContinueData		->	pointer to a tContextData variable,
//										 if (*inOutCountinueData == NULL) there is no more data
//										 otherwise can be used in a 2nd call to the same routine
//										 to get the remainder of the directory node list.
//										 if client does not use  if (*inOutCountinueData != NULL)
//										 and the client doesn't wish to continue
//										 then dsReleaseContinueData should be called to clean up..
//
//	Notes:	Fill a buffer with the names of all the directory nodes...
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetDirNodeList (	tDirReference		inDirRef,
								tDataBufferPtr		outDataBuff,
								UInt32			   *outNodeCount,
								tContextData	   *ioContinueData )
{
	tDirStatus		outResult		= eDSNoErr;
	SInt32			siStatus		= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inDirRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outNodeCount, eDSNullParameter );
		//ability to accept continue data not enforced
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirRef, eDirectoryRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null output buffer
			outResult = VerifyTDataBuff( outDataBuff, eDSNullDataBuff, eDSEmptyBuffer );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the directory reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inDirRef, eDirectoryRefType, gProcessPID), ktDirRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the return buffer length
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( outDataBuff->fBufferSize, kOutBuffLen );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			if ( ioContinueData != nil )
			{
				// Add the context data
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (UInt32)*ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterReceiveError - 2 );
			}
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetDirNodeList );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Get the data buffer 
			siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &outDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDataBuff );
			
			if ( outNodeCount != nil )
			{
				// Get the node count
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( outNodeCount, kNodeCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoNodeCount );
			}
			
			if ( ioContinueData != nil )
			{
				// Get the context data
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoContinueData );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetDirNodeList



//--------------------------------------------------------------------------------------------------
//
//	Name:	dsReleaseContinueData
//
//	Params:	inDirReference		->	directory Reference Established with dsOpenDirService
//			inContinueData		->	pointer to a tContextData variable, value passed in will
//									 be cleaned up by the API libarary, and set to NULL on exit.
//
//
//	Notes:	If continue Data from dsGetDirNodeList is non-NULL, then call this routine to
//			 release the continuation data if the client chooses not to continue the DirNodeListing
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsReleaseContinueData (	tDirReference	inDirReference,
									tContextData	inContinueData )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inDirReference == 0, eDSInvalidReference);
#ifdef __LP64__
		LogThenThrowIfZeroMacro( inContinueData, eDSInvalidContext );
#else
		LogThenThrowIfNilMacro( inContinueData, eDSInvalidContext );
#endif
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirReference, eDirectoryRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the dir or node reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inDirReference, ktDirRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the continue data
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (UInt32)inContinueData, kContextData );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kReleaseContinueData );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus) err;
		}
		catch ( ... )
		{
			outResult = eUnknownServerError;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsReleaseContinueData



//--------------------------------------------------------------------------------------------------
//
//	Name:	dsFindDirNodes
//
//	Params:	inDirReference			->	directory Reference Established with dsOpenDirService
//			inOutDataBufferPtr		->	contains a client allocated buffer to store results..
//										 data is extracted with dsGetDirNodeName
//			inNodeNamePattern		->	a tDataList pointer, which contains the pattern to be matched...
//			inPatternMatchType		->	what type of match to perform on the NodeName pattern...
//										 valid values for this are:
//										 	eDSExact, eDSStartsWith, eDSEndsWith, eDSContains
//										 other match types will return an error.
//			outDirNodeCount			->	number of items in the client buffer.
//			inOutContinueData		->	pointer to a tContextData variable,
//										 if (*inOutCountinueData == NULL) there is no more data
//											 otherwise can be used in a 2nd call to the same routine
//										 to get the remainder of the directory node list.
//										 if client does not use  if (*inOutCountinueData != NULL)
//										 and the client doesn't wish to continue then
//										 dsReleaseContinueData should be called to clean up..
//
//	Notes:	Find directory nodes matching a certain pattern
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsFindDirNodes (	tDirReference		inDirRef,
							tDataBufferPtr		outDataBuff,
							tDataListPtr		inNodeNamePattern,
							tDirPatternMatch	inPatternMatchType,
							UInt32			   *outDirNodeCount,
							tContextData	   *ioContinueData )
{
	tDirStatus		outResult	= eDSNoErr;
	dsBool			bSendList	= true;
	SInt32			siStatus	= eDSNoErr;
	SInt32			siDataLen	= 0;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inDirRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outDirNodeCount, eDSNullParameter );
		//ability to accept continue data not enforced
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirRef, eDirectoryRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();
		
		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();

			if (	(inPatternMatchType == eDSLocalNodeNames) ||
					(inPatternMatchType == eDSConfigNodeName) ||
					(inPatternMatchType == eDSAuthenticationSearchNodeName) ||
					(inPatternMatchType == eDSContactsSearchNodeName) ||
					(inPatternMatchType == eDSNetworkSearchNodeName) ||
					(inPatternMatchType == eDSLocalHostedNodes) ||
					(inPatternMatchType == eDSDefaultNetworkNodes) )
			{
				bSendList = false;
			}
			else
			{
				outResult = VerifyTNodeList( inNodeNamePattern, eDSNullNodeNamePattern, eDSEmptyNodeNamePattern );
				LogThenThrowIfDSErrorMacro( outResult );

				siDataLen = dsGetDataLength( inNodeNamePattern );
			}

			// Make sure we have a non-null output buffer
			outResult = VerifyTDataBuff( outDataBuff, eDSNullDataBuff, eDSEmptyBuffer );
			LogThenThrowIfDSErrorMacro( outResult );

			// Add the directory reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inDirRef, eDirectoryRefType, gProcessPID), ktDirRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );

			// Add the return buffer length
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( outDataBuff->fBufferSize, kOutBuffLen );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );

			if ( bSendList == true )
			{
				// Add the node name pattern
				siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inNodeNamePattern, kNodeNamePatt );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			}

			// Add the pattern match type
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inPatternMatchType, ktDirPattMatch );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );

			if ( ioContinueData != nil )
			{
				// Add the context data
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (UInt32)*ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterReceiveError - 4 );
			}

			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kFindDirNodes );
			LogThenThrowIfDSErrorMacro( siStatus );

			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );

			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			if ( outDataBuff != nil )
			{
				// Get the data buffer 
				siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &outDataBuff, ktDataBuff );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDataBuff );

			}

			if ( outDirNodeCount != nil )
			{
				// Get the Node count
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( outDirNodeCount, kNodeCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoNodeCount );
			}

			if ( ioContinueData != nil )
			{
				// Get the context data
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoContinueData );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsFindDirNodes



//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetDirNodeName
//
//	Params:	inDirReference			->	directory Reference Established with dsOpenDirService
//			inOutDataBuffer			->	a buffer containing all the directory Node names from
//										 dsGetDirNodeList or dsFindDirNodes
//			inDirNodeIndex			->	index of DirNode name to fetch/build, zero-based...
//			inOutDataList			->	a DataList structure that is built by this API call,
//										 client is responsible for disposing of it with a
//										 dsDataListDeAllocate
//
//	Notes:	Parse the return Buffer from dsFindDirNodes or dsGetDirNodeList
//			 build a tDataList representing the directory Node's name...
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetDirNodeName (	tDirReference		inDirRef,
								tDataBufferPtr		inDataBuff,
								UInt32				inDirNodeIndex,
								tDataList		  **outDataList )
{
	tDirStatus		outResult		= eDSInvalidBuffFormat;
	UInt32			siStatus		= eDSNoErr;

	//check to determine whether the buffer is of a standard type for this call
	siStatus = IsNodePathStrBuffer( inDataBuff );
	if (siStatus == eDSNoErr)
	{
		outResult = ExtractDirNodeName(inDataBuff, inDirNodeIndex, outDataList);
	}

	return( outResult );

} // dsGetDirNodeName


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsOpenDirNode
//
//	Params:	inDirReference			->	directory Reference Established with dsOpenDirService
//			inDirNodeName			->	diretoryNode name to open.
//			outDirNodeReference		->	valid call with eDSNoErr, results in a DirectoryNode session
//										 reference this reference represents the clients session
//										 context for the contents of the given directory node.
//
//	Notes:	Establish a session for a particular directory Node
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsOpenDirNode (	tDirReference		inDirRef,
							tDataListPtr		inDirNodeName,
							tDirNodeReference	*outDirNodeRef )
{
	tDirStatus				outResult		= eDSNoErr;
	tDirNodeReference		nodeRef			= 0;
	SInt32					siStatus		= eDSNoErr;
	UInt32					messageIndex	= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inDirRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outDirNodeRef, eDSNullParameter );
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirRef, eDirectoryRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			outResult = VerifyTNodeList( inDirNodeName, eDSNullNodeName, eDSEmptyNodeName );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the directory reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inDirRef, eDirectoryRefType, gProcessPID), ktDirRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the node name
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inDirNodeName, kDirNodeName );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Node reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( nodeRef, ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kOpenDirNode );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outDirNodeRef != nil )
			{
				tDirNodeReference	aRef = 0;
				tDataNodePtr		nodePtr	= nil;
				// Get the Node reference
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( &aRef, ktNodeRef );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDirRef );
				if (messageIndex != 0)
				{
					//we will assume that the plugin name is the first segment of the inDirNodeName
					//need to save this in the FW RefMap for use in endian swapping for CustomCalls to the node
					dsDataListGetNodeAlloc( 0, inDirNodeName, 1, &nodePtr );
					char* pluginNameValue = nil;
					if ( (nodePtr != nil) && (nodePtr->fBufferLength > 0) )
					{
						UInt32 pluginNameLength = nodePtr->fBufferLength;
						pluginNameValue = (char *) calloc(1, 1 + pluginNameLength);
						memcpy(pluginNameValue, nodePtr->fBufferData, pluginNameLength);
						dsDataBufferDeAllocate( 0, nodePtr ); //dir ref not needed and don't check return
					}
					gFWRefMap->NewNodeRefMap( outDirNodeRef, inDirRef, gProcessPID, aRef, messageIndex, pluginNameValue );
				}
				else
				{
					*outDirNodeRef = aRef;
				}
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsOpenDirNode


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsCloseDirNode
//
//	Params:	inDirNodeReference		->	directoryNode reference obtained from dsOpenDirNode
//
//	Notes:	Tear down a DirNode session
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsCloseDirNode ( tDirNodeReference inNodeRef )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the node reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kCloseDirNode );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			//now remove the node reference here if it exists
			gFWRefMap->RemoveNodeRef( inNodeRef, gProcessPID );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsOpenDirNode


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetDirNodeInfo
//
//	Params:	inDirNodeReference		->	directoryNode reference obtained from dsOpenDirNode
//			inDirNodeInfoTypeList	->	tDataList containing the types of requested data
//			inOutDataBuffer			->	a Client allocated buffer to hold the data results.
//			inAttributeInfoOnly		->	this flag is set to true if the client wants Attribute
//										 Info only, no attribute values..
//			outAttributeInfoCount	->	a count of the number of data types present in the DataBuffer
//			outAttributeListRef
//			inOutContinueData		->	pointer to a tContextData variable, if
//										 (*inOutCountinueData == NULL) there is no more data otherwise
//										 can be used in the next call to the same routine to get the
//										 remainder of the information if client does not use
//										 if (*inOutCountinueData != NULL) and the client doesn't
//										 wish to continue then dsReleaseContinueData should be
//										 called to clean up..
//
//	Notes:	
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetDirNodeInfo (	tDirNodeReference	inNodeRef,				// Node ref
								tDataListPtr		inDirNodeInfoTypeList,	// Requested info list
								tDataBufferPtr		outDataBuff,			// Out buffer from client
								dsBool				inAttrInfoOnly,			// Attribute only boolean
								UInt32			   *outAttrInfoCount,		// Attribute count
								tAttributeListRef  *outAttrListRef,		// Attribute ref
								tContextData	   *ioContinueData )		// to be continued...
{
	tDirStatus			outResult		= eDSNoErr;
	SInt32				siStatus		= eDSNoErr;
	UInt32				messageIndex	= 0;
	dsBool				closeServerRef	= false;
	tAttributeListRef	aRef			= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outAttrInfoCount, eDSNullParameter );
		LogThenThrowIfNilMacro( outAttrListRef, eDSNullParameter );
		//ability to accept continue data not enforced
		messageIndex = gFWRefMap->GetMessageTableIndex(inNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( outDataBuff, eDSNullDataBuff, eDSEmptyBuffer );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inDirNodeInfoTypeList, eDSNullNodeInfoTypeList, eDSEmptyNodeInfoTypeList );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the node reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the return buffer length
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( outDataBuff->fBufferSize, kOutBuffLen );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Requested info list
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inDirNodeInfoTypeList, kNodeInfoTypeList );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the Attribute only boolean
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inAttrInfoOnly, kAttrInfoOnly );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			if ( ioContinueData != nil )
			{
				// Add the context data
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (UInt32)*ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterReceiveError - 4 );
			}
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetDirNodeInfo );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Get the data buffer 
			siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &outDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDataBuff );
			
			if ( outAttrInfoCount != nil )
			{
				// Get the attribute info count
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( outAttrInfoCount, kAttrInfoCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoAttrCount );
			}
			
			if ( outAttrListRef != nil )
			{
				// Get the attribute list ref
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( &aRef, ktAttrListRef );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoAttrListRef );
				
				siStatus = IsStdBuffer( outDataBuff );
				if (siStatus == eDSNoErr)
				{
					siStatus = MakeGDNIFWRef(outDataBuff, outAttrListRef);
					if (siStatus == eDSNoErr)
					{
						closeServerRef = true;
					}
				}
				else
				{
					if (messageIndex != 0)
					{
						gFWRefMap->NewAttrListRefMap( outAttrListRef, inNodeRef, gProcessPID, aRef, messageIndex );
					}
					else
					{
						*outAttrListRef = aRef;
					}
				}
			}
			
			if ( ioContinueData != nil )
			{
				// Get the context data
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoContinueData );
			}			
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
		
		if (closeServerRef)
		{
			dsCloseAttributeList(aRef);
		}
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetDirNodeInfo



//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetRecordList
//
//	Params:	inNodeRef			->	directoryNode reference obtained from dsOpenDirNode
//			inDataBuff			->	a Client allocated buffer to hold the data results.
//			inRecNameList		->	a tDataList of Record names to be matched... 
//			inPatternMatch		->	how is the pattern matched for the inRecordName list
//			inRecordTypeList	->	what record types do we want returned?
//			inAttributeTypeList	->	what type of attributes do we want for each record...
//			inAttributeInfoOnly	->	do we want Attribute Information only, or do we also want
//										 Attribute Vales...
//			inOutRecordEntryCount	->	how many record entries are there in the client buffer upon return
//									however, also a limit of the maximum records returned as provided by the client
//									if zero or less then assuming no limit on number of records to be returned
//			inOutContinueData	->	pointer to a tContextData variable, if
//										 (*inOutCountinueData == NULL) there is no more data otherwise
//										 can be used in the next call to the same routine to get the
//										 remainder of the information if client does not use
//										 if (*inOutCountinueData != NULL) and the client doesn't
//										 wish to continue then dsReleaseContinueData should be
//										 called to clean up..
//
//	Notes:	Get a Record Entry from a buffer...
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetRecordList (	tDirNodeReference	inNodeRef,				// Node ref
								tDataBufferPtr		inOutDataBuff,			// in buffer from client
								tDataListPtr		inRecNameList,			// List of record names
								tDirPatternMatch	inPatternMatch,			// Pattern match
								tDataListPtr		inRecTypeList,			// Record types
								tDataListPtr		inAttribTypeList,		// Attribute types
								dsBool				inAttrInfoOnly,			// Attribute only boolean
								UInt32			   *inOutRecEntryCount,		// Record count
								tContextData	   *ioContinueData )		// To be continued...
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;
	UInt32			serverVersion = 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		//ability to not request record count is allowed
		//ability to accept continue data not enforced
		messageIndex = gFWRefMap->GetMessageTableIndex(inNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inOutDataBuff, eDSNullDataBuff, eDSEmptyBuffer );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inRecNameList, eDSNullRecNameList, eDSEmptyRecordNameList );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inRecTypeList, eDSNullRecTypeList, eDSEmptyRecordTypeList );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inAttribTypeList, eDSNullAttributeTypeList, eDSEmptyAttributeTypeList );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the node reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the data buffer
			//don't need to send this empty buffer to the server at all ie. do just like dsDoAttributeValueSearch for version 1 or above
			serverVersion = gMessageTable[messageIndex]->GetServerVersion();
			if (serverVersion > 0)
			{
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inOutDataBuff->fBufferSize, kOutBuffLen );
			}
			else
			{
				siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inOutDataBuff, ktDataBuff );
			}
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Record Name list
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inRecNameList, kRecNameList );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the Pattern Match type
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inPatternMatch, ktDirPattMatch );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			// Add the Record Type list
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inRecTypeList, kRecTypeList );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 4 );
			
			// Add the Attribute Type list
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inAttribTypeList, kAttrTypeList );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 5 );
			
			// Add the Attribute Info Only boolean
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inAttrInfoOnly, kAttrInfoOnly );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 6 );
			
			if ( inOutRecEntryCount != nil )
			{
				if (*inOutRecEntryCount < 0 )
				{
					*inOutRecEntryCount = 0;
				}
				// Add the record count
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg ( *inOutRecEntryCount, kAttrRecEntryCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 7 );
			}
			
			if ( ioContinueData != nil )
			{
				// Add the context data
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (UInt32)*ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterReceiveError - 8 );
			}
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetRecordList );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			if ( outResult != eDSBufferTooSmall )
			{
				LogThenThrowIfDSErrorMacro( outResult );
			}
			
			// Get the data buffer 
			siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &inOutDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDataBuff );
			
			if ( inOutRecEntryCount != nil )
			{
				// Get the record count
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( inOutRecEntryCount, kAttrRecEntryCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoRecEntryCount );
			}
			
			if ( ioContinueData != nil )
			{
				// Get the context data
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoContinueData );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	// This belongs in the "catch", but the compiler is broken - is the compiler fixed yet?
	if ( ( outResult != eDSNoErr ) && ( outResult != eDSBufferTooSmall ) )
	{
		if ( ioContinueData != nil )
		{
			*ioContinueData = 0;
		}
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetRecordList



//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetRecordEntry
//
//	Params:	inDirNodeReference		->	directoryNode reference obtained from dsOpenDirNode
//			inOutDataBuffer			->	a Client allocated buffer to hold the data results.
//			inRecordEntryIndex
//			outAttributeListRef
//			outRecordEntryPtr
//
//	Notes:	Get a Record Entry from a buffer...
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetRecordEntry	(	tDirNodeReference	inNodeRef,
								tDataBufferPtr		inOutDataBuff,
								UInt32				inRecordEntryIndex,
								tAttributeListRef	*outAttrListRef,
								tRecordEntryPtr		*outRecEntryPtr )
{
	tDirStatus			outResult	= eDSNoErr;
	SInt32				siStatus	= eDSNoErr;
	UInt32				messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );
	
	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outAttrListRef, eDSNullParameter );
		LogThenThrowIfNilMacro( outRecEntryPtr, eDSNullParameter );
		LogThenThrowIfZeroMacro( inRecordEntryIndex, eDSInvalidIndex );
        //check to determine whether we employ client side buffer parsing
        siStatus = IsStdBuffer( inOutDataBuff );
        if (siStatus == eDSNoErr)
        {
            outResult = ExtractRecordEntry(inOutDataBuff, inRecordEntryIndex, outAttrListRef, outRecEntryPtr);
            return( outResult );
        }
        
		messageIndex = gFWRefMap->GetMessageTableIndex(inNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inOutDataBuff, eDSNullDataBuff, eDSEmptyBuffer );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Node Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the data buffer
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inOutDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the record index
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inRecordEntryIndex, kRecEntryIndex );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetRecordEntry );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Get the data buffer 
			siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &inOutDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDataBuff );
			
			if ( outAttrListRef != nil )
			{
				tAttributeListRef	aRef = 0;
				// Get the attribute list ref
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( &aRef, ktAttrListRef );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoAttrListRef );
				if (messageIndex != 0)
				{
					gFWRefMap->NewAttrListRefMap( outAttrListRef, inNodeRef, gProcessPID, aRef, messageIndex );
				}
				else
				{
					*outAttrListRef = aRef;
				}
			}
			
			if ( outRecEntryPtr != nil )
			{
				// Get the context data
				siStatus = gMessageTable[messageIndex]->Get_tRecordEntry_FromMsg( outRecEntryPtr, ktRecordEntry );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoRecEntry );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetRecordEntry



//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetAttributeEntry
//
//	Params:	
//
//	Notes:	Get an Attribute Entry from a buffer...
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetAttributeEntry (	tDirNodeReference		inNodeRef,					// Node ref <- why ???
									tDataBufferPtr			inOutDataBuff,				// Client buffer
									tAttributeListRef		inAttrListRef,				// Attirbute list ref
									UInt32					inAttrInfoIndex,			// Attribute Index
									tAttributeValueListRef	*outAttrValueListRef,		// Attribute value ref
									tAttributeEntryPtr		*outAttrInfoPtr )			// Data ptr
{
	tDirStatus			outResult		= eDSNoErr;
	SInt32				siStatus		= eDSNoErr;
	UInt32				messageIndex	= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outAttrValueListRef, eDSNullParameter );
		LogThenThrowIfNilMacro( outAttrInfoPtr, eDSNullParameter );
		LogThenThrowIfZeroMacro( inAttrInfoIndex, eDSInvalidIndex );
        //check to determine whether we employ client side buffer parsing via FW reference
        siStatus = IsFWReference( inAttrListRef );
        if (siStatus == eDSNoErr)
        {
            outResult = ExtractAttributeEntry(inOutDataBuff, inAttrListRef, inAttrInfoIndex, outAttrValueListRef, outAttrInfoPtr);
            return( outResult );
        }
        
		messageIndex = gFWRefMap->GetMessageTableIndex(inNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();
		
		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inOutDataBuff, eDSNullDataBuff, eDSEmptyBuffer );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Node Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the data buffer
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inOutDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the Attribute List Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inAttrListRef, eAttrListRefType, gProcessPID), ktAttrListRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			// Add the Attribute Index
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inAttrInfoIndex, kAttrInfoIndex );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 4 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetAttributeEntry );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Get the data buffer 
			siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &inOutDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDataBuff );
			
			if ( outAttrValueListRef != nil )
			{
				tAttributeValueListRef	aRef = 0;
				// Get the attribute value list ref
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( &aRef, ktAttrValueListRef );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoAttrValueListRef );
				if (messageIndex != 0)
				{
					gFWRefMap->NewAttrValueRefMap( outAttrValueListRef, inAttrListRef, gProcessPID, aRef, messageIndex );
				}
				else
				{
					*outAttrValueListRef = aRef;
				}
			}
			
			if ( outAttrInfoPtr != nil )
			{
				// Get the attribute entry
				siStatus = gMessageTable[messageIndex]->Get_tAttrEntry_FromMsg( outAttrInfoPtr, ktAttrEntry );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoAttrEntry );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetAttributeEntry



//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetNextAttributeEntry
//
//	Notes:	Get Next Attribute Entry from a buffer...
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetNextAttributeEntry (	tDirNodeReference		inNodeRef,					// Node ref <- why ???
										tDataBufferPtr			inOutDataBuff,				// Client buffer
										tAttributeListRef		inAttrListRef,				// Attirbute list ref
										UInt32					inAttrInfoIndex,			// Attribute Index
										SInt32				   *inOutAttributeOffset,
										tAttributeValueListRef	*outAttrValueListRef,		// Attribute value ref
										tAttributeEntryPtr		*outAttrInfoPtr )			// Data ptr
{
	tDirStatus			outResult		= eDSNoErr;
	SInt32				siStatus		= eDSNoErr;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outAttrValueListRef, eDSNullParameter );
		LogThenThrowIfNilMacro( outAttrInfoPtr, eDSNullParameter );
		LogThenThrowIfZeroMacro( inAttrInfoIndex, eDSInvalidIndex );
        //check to determine whether we employ client side buffer parsing via FW reference
        siStatus = IsFWReference( inAttrListRef );
        if (siStatus == eDSNoErr)
        {
            outResult = ExtractNextAttributeEntry(inOutDataBuff, inAttrListRef, inAttrInfoIndex, inOutAttributeOffset, outAttrValueListRef, outAttrInfoPtr);
            return( outResult );
        }
        
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	//otherwise fall through to old method
	outResult = dsGetAttributeEntry(inNodeRef, inOutDataBuff, inAttrListRef, inAttrInfoIndex, outAttrValueListRef, outAttrInfoPtr);

	return( outResult );

} // dsGetNextAttributeEntry


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetAttributeValue
//
//	Params:	
//
//	Notes:	Get an Attribute Value from a buffer...
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetAttributeValue (	tDirNodeReference		 inNodeRef,
									tDataBufferPtr			 inOutDataBuff,
									UInt32					 inAttrValueIndex,
									tAttributeValueListRef	 inAttrValueListRef,
									tAttributeValueEntryPtr	*outAttrValue )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outAttrValue, eDSNullParameter );
		LogThenThrowIfZeroMacro( inAttrValueIndex, eDSInvalidIndex );
        //check to determine whether we employ client side buffer parsing via FW reference
        siStatus = IsFWReference( inAttrValueListRef );
        if (siStatus == eDSNoErr)
        {
            outResult = ExtractAttributeValue(inOutDataBuff, inAttrValueListRef, inAttrValueIndex, outAttrValue);
            return( outResult );
        }
        
		messageIndex = gFWRefMap->GetMessageTableIndex(inNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();
		
		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inOutDataBuff, eDSNullDataBuff, eDSEmptyBuffer );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Node Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the data buffer
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inOutDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Attribute Value Index
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inAttrValueIndex, kAttrValueIndex );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the Attribute Value List Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inAttrValueListRef, eAttrValueListRefType, gProcessPID), ktAttrValueListRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetAttributeValue );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Get the data buffer 
			siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &inOutDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDataBuff );
			
			if ( outAttrValue != nil )
			{
				// Get the attribute value entry
				siStatus = gMessageTable[messageIndex]->Get_tAttrValueEntry_FromMsg( outAttrValue, ktAttrValueEntry );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoAttrValueEntry );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetAttributeValue


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetNextAttributeValue
//
//	Notes:	Get Next Attribute Value from a buffer...
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetNextAttributeValue (	tDirNodeReference			inNodeRef,
										tDataBufferPtr				inOutDataBuff,
										UInt32						inAttrValueIndex,
										SInt32					   *inOutAttributeValueOffset,
										tAttributeValueListRef		inAttrValueListRef,
										tAttributeValueEntryPtr	   *outAttrValue )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outAttrValue, eDSNullParameter );
		LogThenThrowIfZeroMacro( inAttrValueIndex, eDSInvalidIndex );
        //check to determine whether we employ client side buffer parsing via FW reference
        siStatus = IsFWReference( inAttrValueListRef );
        if (siStatus == eDSNoErr)
        {
            outResult = ExtractNextAttributeValue(inOutDataBuff, inAttrValueListRef, inAttrValueIndex, inOutAttributeValueOffset, outAttrValue);
            return( outResult );
        }
        
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}
	
	//otherwise fall through to old method
	outResult = dsGetAttributeValue(inNodeRef, inOutDataBuff, inAttrValueIndex, inAttrValueListRef, outAttrValue);

	return( outResult );

} // dsGetNextAttributeValue


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsCloseAttributeList
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------


tDirStatus dsCloseAttributeList ( tAttributeListRef inAttributeListRef )
{

	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inAttributeListRef == 0, eDSInvalidReference);
        //check to determine whether we employ client side buffer parsing via FW reference
        siStatus = IsFWReference( inAttributeListRef );
        if (siStatus == eDSNoErr)
        {
			outResult = gFWRefTable->RemoveAttrListRef( inAttributeListRef, gProcessPID );
            return( outResult );
        }
        
		messageIndex = gFWRefMap->GetMessageTableIndex(inAttributeListRef,eAttrListRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();
		
		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the Attr List Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inAttributeListRef,eAttrListRefType, gProcessPID), ktAttrListRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kCloseAttributeList );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			//now remove the attr list reference here if it exists
			gFWRefMap->RemoveAttrListRef( inAttributeListRef, gProcessPID );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsCloseAttributeList


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsCloseAttributeValueList
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------


tDirStatus dsCloseAttributeValueList ( tAttributeValueListRef inAttributeValueListRef )
{

	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inAttributeValueListRef == 0, eDSInvalidReference);
        //check to determine whether we employ client side buffer parsing via FW reference
        siStatus = IsFWReference( inAttributeValueListRef );
        if (siStatus == eDSNoErr)
        {
			outResult = gFWRefTable->RemoveAttrValueRef( inAttributeValueListRef, gProcessPID );
            return( outResult );
        }
        
		messageIndex = gFWRefMap->GetMessageTableIndex(inAttributeValueListRef,eAttrValueListRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the Attr Value List Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inAttributeValueListRef, eAttrValueListRefType, gProcessPID), ktAttrValueListRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kCloseAttributeValueList );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			//now remove the attr value list reference here if it exists
			gFWRefMap->RemoveAttrListRef( inAttributeValueListRef, gProcessPID );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsCloseAttributeValueList


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsOpenRecord
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsOpenRecord (	tDirNodeReference	inNodeRef,
							tDataNodePtr		inRecType,
							tDataNodePtr		inRecName,
							tRecordReference	*outRecRef )
{
	tDirStatus			outResult		= eDSNoErr;
	SInt32				siStatus		= eDSNoErr;
	UInt32				messageIndex	= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outRecRef, eDSNullParameter );
		messageIndex = gFWRefMap->GetMessageTableIndex(inNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();
		
		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inRecType, eDSNullRecType, eDSEmptyRecordType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inRecName, eDSNullRecName, eDSEmptyRecordName );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Node Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the Record Type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inRecType, kRecTypeBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Record Name
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inRecName, kRecNameBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kOpenRecord );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outRecRef != nil )
			{
				tRecordReference	aRef = 0;
				// Get the record ref
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( &aRef, ktRecRef );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoRecRef );
				if (messageIndex != 0)
				{
					gFWRefMap->NewRecordRefMap( outRecRef, inNodeRef, gProcessPID, aRef, messageIndex );
				}
				else
				{
					*outRecRef = aRef;
				}
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );


} // dsOpenRecord



//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetRecordReferenceInfo
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetRecordReferenceInfo (	tRecordReference	inRecRef,
										tRecordEntryPtr		*outRecInfo )
{
	tDirStatus			outResult	= eDSNoErr;
	SInt32				siStatus	= eDSNoErr;
	UInt32				messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outRecInfo, eDSNullParameter );
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetRecordReferenceInfo );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outRecInfo != nil )
			{
				// Get the record entry
				siStatus = gMessageTable[messageIndex]->Get_tRecordEntry_FromMsg( outRecInfo, ktRecordEntry );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoRecEntry );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetRecordReferenceInfo



//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetRecordAttributeInfo
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetRecordAttributeInfo (	tRecordReference	inRecRef,
										tDataNodePtr		inAttributeType,
										tAttributeEntryPtr	*outAttrInfoPtr )
{
	tDirStatus		outResult		= eDSNoErr;
	SInt32			siStatus		= eDSNoErr;
	UInt32			messageIndex	= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outAttrInfoPtr, eDSNullParameter );
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAttributeType, eDSNullAttributeType, eDSEmptyAttributeType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the Attribute Type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttributeType, kAttrTypeBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetRecordAttributeInfo );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Get the attribute entry
			if ( outAttrInfoPtr != nil )
			{
				siStatus = gMessageTable[messageIndex]->Get_tAttrEntry_FromMsg( outAttrInfoPtr, ktAttrEntry );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoAttrEntry );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetRecordAttributeInfo



//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetRecordAttributeValueByID
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetRecordAttributeValueByID (	tRecordReference		inRecRef,
											tDataNodePtr			inAttributeType,											
											UInt32					inValueID,
											tAttributeValueEntryPtr	*outEntryPtr )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outEntryPtr, eDSNullParameter );
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAttributeType, eDSNullAttributeType, eDSEmptyAttributeType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the Attribute type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttributeType, kAttrTypeBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Value Index
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inValueID, kAttrValueID );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetRecordAttributeValueByID );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outEntryPtr != nil )
			{
				// Get the attribute value entry
				siStatus = gMessageTable[messageIndex]->Get_tAttrValueEntry_FromMsg( outEntryPtr, ktAttrValueEntry );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoAttrValueEntry );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetRecordAttributeValueByID


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetRecordAttributeValueByIndex
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetRecordAttributeValueByIndex (	tRecordReference		inRecRef,
												tDataNodePtr			inAttributeType,
												UInt32					inAttrValueIndex,
												tAttributeValueEntryPtr	*outEntryPtr )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outEntryPtr, eDSNullParameter );
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );
		LogThenThrowIfZeroMacro( inAttrValueIndex, eDSInvalidIndex );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAttributeType, eDSNullAttributeType, eDSEmptyAttributeType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the Attribute type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttributeType, kAttrTypeBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Value Index
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inAttrValueIndex, kAttrValueIndex );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetRecordAttributeValueByIndex );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outEntryPtr != nil )
			{
				// Get the attribute value entry
				siStatus = gMessageTable[messageIndex]->Get_tAttrValueEntry_FromMsg( outEntryPtr, ktAttrValueEntry );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoAttrValueEntry );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetRecordAttributeValueByIndex


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsGetRecordAttributeValueByValue
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsGetRecordAttributeValueByValue (	tRecordReference		inRecRef,
												tDataNodePtr			inAttributeType,
												tDataNodePtr			inAttributeValue,
												tAttributeValueEntryPtr	*outEntryPtr )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		LogThenThrowIfNilMacro( outEntryPtr, eDSNullParameter );
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAttributeType, eDSNullAttributeType, eDSEmptyAttributeType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Make sure we have a non-null data buffer
			LogThenThrowIfNilMacro( inAttributeValue, eDSNullAttributeValue );
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the Attribute type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttributeType, kAttrTypeBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Attribute value
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttributeValue, kAttrValueBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kGetRecordAttributeValueByValue );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outEntryPtr != nil )
			{
				// Get the attribute value entry
				siStatus = gMessageTable[messageIndex]->Get_tAttrValueEntry_FromMsg( outEntryPtr, ktAttrValueEntry );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoAttrValueEntry );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsGetRecordAttributeValueByValue


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsFlushRecord
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------


tDirStatus dsFlushRecord ( tRecordReference inRecRef )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kFlushRecord );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsFlushRecord



//--------------------------------------------------------------------------------------------------
//
//	Name:	dsCloseRecord
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------


tDirStatus dsCloseRecord ( tRecordReference inRecRef )
{

	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();
		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kCloseRecord );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			//now remove the record reference here if it exists
			gFWRefMap->RemoveRecordRef( inRecRef, gProcessPID );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsCloseRecord


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsSetRecordName
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsSetRecordName (	tRecordReference	inRecRef,
								tDataNodePtr		inNewRecordName )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inNewRecordName, eDSNullRecName, eDSEmptyRecordName );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the data buffer
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inNewRecordName, kRecNameBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kSetRecordName );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );			
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsSetRecordName


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsSetRecordType
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsSetRecordType ( tRecordReference inRecRef, tDataNodePtr inNewRecordType )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inNewRecordType, eDSNullRecType, eDSEmptyRecordType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the data buffer
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inNewRecordType, kRecTypeBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kSetRecordType );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsSetRecordType


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsDeleteRecord
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsDeleteRecord ( tRecordReference inRecRef )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kDeleteRecord );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			//now remove the record reference here if it exists
			gFWRefMap->RemoveRecordRef( inRecRef, gProcessPID );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsDeleteRecord


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsCreateRecord
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsCreateRecord (	tDirNodeReference	inNodeRef,
							tDataNodePtr		inRecType,
							tDataNodePtr		inRecName )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inRecType, eDSNullRecType, eDSEmptyRecordType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inRecName, eDSNullRecName, eDSEmptyRecordName );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Node Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the Record Type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inRecType, kRecTypeBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Record Name
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inRecName, kRecNameBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the Open boolean
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( false, kOpenRecBool );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kCreateRecord );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsCreateRecord


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsCreateRecordAndOpen
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsCreateRecordAndOpen (	tDirNodeReference	inNodeRef,
									tDataNodePtr		inRecType,
									tDataNodePtr		inRecName,
									tRecordReference	*outRecRef )
{
	tDirStatus			outResult		= eDSNoErr;
	SInt32				siStatus		= eDSNoErr;
	UInt32				messageIndex	= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inRecType, eDSNullRecType, eDSEmptyRecordType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inRecName, eDSNullRecName, eDSEmptyRecordName );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Node Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the Record Type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inRecType, kRecTypeBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Record Name
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inRecName, kRecNameBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the Open boolean
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (outRecRef != nil), kOpenRecBool );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kCreateRecordAndOpen );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outRecRef != nil )
			{
				tRecordReference	aRef = 0;
				// Get the record reference
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( &aRef, ktRecRef );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoRecRef );
				if (messageIndex != 0)
				{
					gFWRefMap->NewRecordRefMap( outRecRef, inNodeRef, gProcessPID, aRef, messageIndex );
				}
				else
				{
					*outRecRef = aRef;
				}
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsCreateRecordAndOpen


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsAddAttribute
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsAddAttribute (	tRecordReference		inRecRef,
							tDataNodePtr			inNewAttr,
							tAccessControlEntryPtr	inNewAttrAccess,
							tDataNodePtr			inFirstAttrValue )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

//tAccessControlEntryPtr	inNewAttrAccess NOT USED
	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inNewAttr, eDSNullAttribute, eDSEmptyAttribute );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Make sure we have a non-null data buffer
//			outResult = VerifyTDataBuff( inFirstAttrValue, eDSNullAttributeValue, eDSEmptyAttributeValue );
//			LogThenThrowIfDSErrorMacro( outResult );
			//allow no initial attribute value
			//LogThenThrowIfNilMacro( inFirstAttrValue, eDSNullAttributeValue );
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the New Attribute
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inNewAttr, kNewAttrBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the First Attribute Value
			if ( inFirstAttrValue != nil)
			{
				siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inFirstAttrValue, kFirstAttrBuff );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			}
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kAddAttribute );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsAddAttribute


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsRemoveAttribute
//
//	Params:	
//
//	Notes: Do it
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsRemoveAttribute (	tRecordReference	inRecRef,
								tDataNodePtr		inAttribute )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAttribute, eDSNullAttribute, eDSEmptyAttribute );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the Attribute
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttribute, kAttrBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kRemoveAttribute );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );			
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsRemoveAttribute


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsAddAttributeValue
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsAddAttributeValue (	tRecordReference	inRecRef,
									tDataNodePtr		inAttrType,
									tDataNodePtr		inAttrValue )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAttrType, eDSNullAttributeType, eDSEmptyAttributeType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			LogThenThrowIfNilMacro( inAttrValue, eDSNullAttributeValue );
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the Attribute
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttrType, kAttrTypeBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Attribute value
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttrValue, kAttrValueBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kAddAttributeValue );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsAddAttributeValue


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsRemoveAttributeValue
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus	dsRemoveAttributeValue	(	tRecordReference	inRecRef,
										tDataNodePtr		inAttrType,
										UInt32				inAttrValueID )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAttrType, eDSNullAttributeType, eDSEmptyAttributeType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the Attribute
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttrType, kAttrTypeBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Attribute value
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inAttrValueID, kAttrValueID );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kRemoveAttributeValue );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsRemoveAttributeValue


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsSetAttributeValue
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsSetAttributeValue (	tRecordReference		inRecRef,
									tDataNodePtr			inAttrType,
									tAttributeValueEntryPtr	inAttrValueEntry )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAttrType, eDSNullAttributeType, eDSEmptyAttributeType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			LogThenThrowIfNilMacro( inAttrValueEntry, eDSNullAttributeValue );
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the Attribute Type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttrType, kAttrTypeBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Attribute Value entry
			siStatus = gMessageTable[messageIndex]->Add_tAttrValueEntry_ToMsg( inAttrValueEntry );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kSetAttributeValue );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsSetAttributeValue


tDirStatus dsSetAttributeValues		(   tRecordReference		inRecRef,
										tDataNodePtr			inAttrType,
										tDataListPtr			inAttributeValuesPtr )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inRecRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inRecRef,eRecordRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAttrType, eDSNullAttributeType, eDSEmptyAttributeType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inAttributeValuesPtr, eDSNullDataList, eDSEmptyDataList );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Record Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inRecRef, eRecordRefType, gProcessPID), ktRecRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the Attribute Type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttrType, kAttrTypeBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Attribute Value list
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inAttributeValuesPtr, kAttrValueList );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kSetAttributeValues );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsSetAttributeValues

//--------------------------------------------------------------------------------------------------
//
//	Name:	dsDoDirNodeAuth
//
//	Params:	
//
//	Notes: 
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsDoDirNodeAuth (	tDirNodeReference	inNodeRef,
								tDataNodePtr		inAuthMethod,
								dsBool				inDirNodeAuthOnlyFlag,
								tDataBufferPtr		inAuthStepData,
								tDataBufferPtr		outAuthStepDataResponse, // <- should be a handle
								tContextData		*ioContinueData )
{
	tDirStatus		outResult	= eDSNoErr;

	outResult = dsDoDirNodeAuthOnRecordType(	inNodeRef,
												inAuthMethod,
												inDirNodeAuthOnlyFlag,
												inAuthStepData,
												outAuthStepDataResponse,
												ioContinueData,
												nil);
	//record type passed in as nil to be backward compatible
	return( outResult );

} // dsDoDirNodeAuth


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsDoDirNodeAuthOnRecordType
//
//	Params:	
//
//	Notes: 
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsDoDirNodeAuthOnRecordType (	tDirNodeReference	inNodeRef,
											tDataNodePtr		inAuthMethod,
											dsBool				inDirNodeAuthOnlyFlag,
											tDataBufferPtr		inAuthStepData,
											tDataBufferPtr		outAuthStepDataResponse,
											tContextData		*ioContinueData,
											tDataNodePtr		inRecordType )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAuthMethod, eDSNullAutMethod, eDSEmptyAuthMethod );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inAuthStepData, eDSNullAuthStepData, eDSEmptyAuthStepData );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( outAuthStepDataResponse, eDSNullAuthStepDataResp, eDSEmptyAuthStepDataResp );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the Node Reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the auth method
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAuthMethod, kAuthMethod );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the Auth Only bool
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inDirNodeAuthOnlyFlag, kAuthOnlyBool );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the auth step data
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAuthStepData, kAuthStepBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			// Add the auth step response
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( outAuthStepDataResponse, kAuthResponseBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 4 );
			
			if ( ioContinueData != nil )
			{
				// Add the context data
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (UInt32)*ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterReceiveError - 5 );
			}
			
			if (inRecordType != nil)
			{
				// Make sure we have a non-null data buffer - partially redundant check
				outResult = VerifyTDataBuff( inRecordType, eDSNullRecType, eDSEmptyRecordType );
				LogThenThrowIfDSErrorMacro( outResult );
				// Add the Record Type
				siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inRecordType, kRecTypeBuff );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 6 );
			}
			
			// **************** Send the message ****************
			if (inRecordType == nil)
			{
				//backward compatible with dsDoDirNodeAuth call
				siStatus = gMessageTable[messageIndex]->SendInlineMessage( kDoDirNodeAuth );
			}
			else
			{
				siStatus = gMessageTable[messageIndex]->SendInlineMessage( kDoDirNodeAuthOnRecordType );
			}
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outAuthStepDataResponse != nil )
			{
				// Get the auth step response 
				siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &outAuthStepDataResponse, kAuthStepDataResponse );
				// LogThenThrowThisIfDSErrorMacro( siStatus, eParameterReceiveError );
			}
			
			if ( ioContinueData != nil )
			{
				// Get the context data
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoContinueData );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}
	
	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsDoDirNodeAuthOnRecordType


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsDoAttributeValueSearch
//
//	Params:	inDirNodeReference
//			inOutDataBuffer			->	A client-allocated buffer to hold the data results.
//			inRecordTypeList		->	The list of record types to search over.
//			inAttributeType			->	Which attribute type we are to match on.
//			inPatternMatchType		->	The matching criteria used.
//			inPattern2Match			->	Value to match for the above attribute type.
//			inOutMatchRecordCount	->	How many records we found that met the match criteria.
//										However, also a limit of the maximum records returned as provided by the client.
//										If zero or less then assuming no limit on number of records to be returned.
//			ioContinueData			->  Set to non-NULL if there is potentially more result data available.
//
//	Notes:  All values of all attributes for the found records should be returned in this call.
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsDoAttributeValueSearch (	tDirNodeReference	inDirNodeRef,
										tDataBufferPtr		outDataBuff,
										tDataListPtr		inRecTypeList,
										tDataNodePtr		inAttrType,
										tDirPatternMatch	inPattMatchType,
										tDataNodePtr		inPatt2Match,
										UInt32			   *inOutMatchRecordCount,
										tContextData	   *ioContinueData )
{
	tDirStatus			outResult	= eDSNoErr;
	SInt32				siStatus	= eDSNoErr;
	UInt32				messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inDirNodeRef == 0, eDSInvalidReference);
		//ability to not request record count is allowed
		//ability to accept continue data not enforced
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( outDataBuff, eDSNullDataBuff, eDSEmptyBuffer );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inRecTypeList, eDSNullRecTypeList, eDSEmptyRecordTypeList );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTDataBuff( inAttrType, eDSNullAttributeType, eDSEmptyAttributeType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTDataBuff( inPatt2Match, eDSNullNodeNamePattern, eDSEmptyPatternMatch );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the node reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inDirNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the return buffer length
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( outDataBuff->fBufferSize, kOutBuffLen );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the record type list
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inRecTypeList, kRecTypeList );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the attribute type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttrType, kAttrType );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			// Add the pattern match value
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inPattMatchType, kAttrPattMatch );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 4 );
			
			// Add the pattern match
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inPatt2Match, kAttrMatch );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 5 );
			
			if ( inOutMatchRecordCount != nil )
			{
				if (*inOutMatchRecordCount < 0 )
				{
					*inOutMatchRecordCount = 0;
				}
				// Add the record count
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg ( *inOutMatchRecordCount, kMatchRecCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 6 );
			}
			
			if ( ioContinueData != nil )
			{
				// Add the context data
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (UInt32)*ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 7 );
			}
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kDoAttributeValueSearch );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			if ( outResult != eDSBufferTooSmall )
			{
				LogThenThrowIfDSErrorMacro( outResult );
			}
			
			// Get the data buffer 
			siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &outDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDataBuff );
			
			if ( inOutMatchRecordCount != nil )
			{
				// Get the record count
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( inOutMatchRecordCount, kMatchRecCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoRecMatchCount );
			}
			
			if ( ioContinueData != nil )
			{
				// Get the context data
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoContinueData );
			}			
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsDoAttributeValueSearch


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsDoMultipleAttributeValueSearch
//
//	Params:	inDirNodeReference
//			inOutDataBuffer			->	A client-allocated buffer to hold the data results.
//			inRecordTypeList		->	The list of record types to search over.
//			inAttributeType			->	Which attribute type we are to match on.
//			inPatternMatchType		->	The matching criteria used.
//			inPatterns2Match		->	List of values to match for the above attribute type.
//			inOutMatchRecordCount	->	How many records we found that met the match criteria.
//										However, also a limit of the maximum records returned as provided by the client.
//										If zero or less then assuming no limit on number of records to be returned.
//			ioContinueData			->  Set to non-NULL if there is potentially more result data available.
//
//	Notes:  All values of all attributes for the found records should be returned in this call.
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsDoMultipleAttributeValueSearch (	tDirNodeReference	inDirNodeRef,
												tDataBufferPtr		outDataBuff,
												tDataListPtr		inRecTypeList,
												tDataNodePtr		inAttrType,
												tDirPatternMatch	inPattMatchType,
												tDataListPtr		inPatterns2Match,
												UInt32			   *inOutMatchRecordCount,
												tContextData	   *ioContinueData )
{
	tDirStatus			outResult	= eDSNoErr;
	SInt32				siStatus	= eDSNoErr;
	UInt32				messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inDirNodeRef == 0, eDSInvalidReference);
		//ability to not request record count is allowed
		//ability to accept continue data not enforced
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( outDataBuff, eDSNullDataBuff, eDSEmptyBuffer );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inRecTypeList, eDSNullRecTypeList, eDSEmptyRecordTypeList );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTDataBuff( inAttrType, eDSNullAttributeType, eDSEmptyAttributeType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inPatterns2Match, eDSNullAttributeValue, eDSEmptyPatternMatch );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the node reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inDirNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the return buffer length
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( outDataBuff->fBufferSize, kOutBuffLen );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the record type list
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inRecTypeList, kRecTypeList );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the attribute type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttrType, kAttrType );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			// Add the pattern match value
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inPattMatchType, kAttrPattMatch );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 4 );
			
			// Add the pattern match
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inPatterns2Match, kAttrMatches );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 5 );
			
			if ( inOutMatchRecordCount != nil )
			{
				if (*inOutMatchRecordCount < 0 )
				{
					*inOutMatchRecordCount = 0;
				}
				// Add the record count
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg ( *inOutMatchRecordCount, kMatchRecCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 6 );
			}
			
			if ( ioContinueData != nil )
			{
				// Add the context data
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (UInt32)*ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 7 );
			}
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kDoMultipleAttributeValueSearch );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			if ( outResult != eDSBufferTooSmall )
			{
				LogThenThrowIfDSErrorMacro( outResult );
			}
			
			// Get the data buffer 
			siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &outDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDataBuff );
			
			if ( inOutMatchRecordCount != nil )
			{
				// Get the record count
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( inOutMatchRecordCount, kMatchRecCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoRecMatchCount );
			}
			
			if ( ioContinueData != nil )
			{
				// Get the context data
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoContinueData );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsDoMultipleAttributeValueSearch


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsDoAttributeValueSearchWithData
//
//	Params:	inDirNodeRef
//			inOutDataBuff			->	A client-allocated buffer to hold the data results.
//			inRecTypeList			->	The list of record types to search over.
//			inAttrType				->	Which attribute type we are to match on.
//			inPattMatchType			->	The matching criteria used.
//			inPatt2Match			->	Value to match for the above attribute type.
//			inAttrTypeRequestList   ->  List of attribute types that chould be returned.
//			inAttrInfoOnly			->  If set to true then the actual values for the above
//										list of attribute types need not be returned.
//			inOutMatchRecordCount	->	How many records we found that met the match criteria.
//										However, also a limit of the maximum records returned as provided by the client.
//										If zero or less then assuming no limit on number of records to be returned.
//			ioContinueData			->  Set to non-NULL if there is potentially more result data available.
//
//	Notes:  Only the requested list of attributes for the found records should be returned in this call.
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsDoAttributeValueSearchWithData (	tDirNodeReference	inDirNodeRef,
												tDataBufferPtr		inOutDataBuff,
												tDataListPtr		inRecTypeList,
												tDataNodePtr		inAttrType,
												tDirPatternMatch	inPattMatchType,
												tDataNodePtr		inPatt2Match,
												tDataListPtr		inAttrTypeRequestList,
												dsBool				inAttrInfoOnly,
												UInt32			   *inOutMatchRecordCount,
												tContextData	   *ioContinueData )
{
	tDirStatus			outResult	= eDSNoErr;
	SInt32				siStatus	= eDSNoErr;
	UInt32				messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inDirNodeRef == 0, eDSInvalidReference);
		//ability to not request record count is allowed
		//ability to accept continue data not enforced
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inOutDataBuff, eDSNullDataBuff, eDSEmptyBuffer );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inRecTypeList, eDSNullRecTypeList, eDSEmptyRecordTypeList );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTDataBuff( inAttrType, eDSNullAttributeType, eDSEmptyAttributeType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTDataBuff( inPatt2Match, eDSNullNodeNamePattern, eDSEmptyPatternMatch );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inAttrTypeRequestList, eDSNullAttributeRequestList, eDSEmptyAttributeRequestList );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the node reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inDirNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the return buffer length
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inOutDataBuff->fBufferSize, kOutBuffLen );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the record type list
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inRecTypeList, kRecTypeList );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the attribute type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttrType, kAttrType );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			// Add the pattern match value
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inPattMatchType, kAttrPattMatch );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 4 );
			
			// Add the pattern match
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inPatt2Match, kAttrMatch );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 5 );
			
			// Add the attribute request type list
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inAttrTypeRequestList, kAttrTypeRequestList );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 6 );
			
			// Add the Attribute Info Only boolean
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inAttrInfoOnly, kAttrInfoOnly );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 6 );
			
			if ( inOutMatchRecordCount != nil )
			{
				if (*inOutMatchRecordCount < 0 )
				{
					*inOutMatchRecordCount = 0;
				}
				// Add the record count
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg ( *inOutMatchRecordCount, kMatchRecCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 7 );
			}
			
			if ( ioContinueData != nil )
			{
				// Add the context data
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (UInt32)*ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 8 );
			}
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kDoAttributeValueSearchWithData );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			if ( outResult != eDSBufferTooSmall )
			{
				LogThenThrowIfDSErrorMacro( outResult );
			}
			
			// Get the data buffer 
			siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &inOutDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDataBuff );
			
			if ( inOutMatchRecordCount != nil )
			{
				// Get the record count
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( inOutMatchRecordCount, kMatchRecCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoRecMatchCount );
			}
			
			if ( ioContinueData != nil )
			{
				// Get the context data
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoContinueData );
			}			
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsDoAttributeValueSearchWithData


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsDoMultipleAttributeValueSearchWithData
//
//	Params:	inDirNodeRef
//			inOutDataBuff			->	A client-allocated buffer to hold the data results.
//			inRecTypeList			->	The list of record types to search over.
//			inAttrType				->	Which attribute type we are to match on.
//			inPattMatchType			->	The matching criteria used.
//			inPatterns2Match		->	The list of values to match for the above attribute type.
//			inAttrTypeRequestList   ->  List of attribute types that chould be returned.
//			inAttrInfoOnly			->  If set to true then the actual values for the above
//										list of attribute types need not be returned.
//			inOutMatchRecordCount	->	How many records we found that met the match criteria.
//										However, also a limit of the maximum records returned as provided by the client.
//										If zero or less then assuming no limit on number of records to be returned.
//			ioContinueData			->  Set to non-NULL if there is potentially more result data available.
//
//	Notes:  Only the requested list of attributes for the found records should be returned in this call.
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsDoMultipleAttributeValueSearchWithData (	tDirNodeReference	inDirNodeRef,
														tDataBufferPtr		inOutDataBuff,
														tDataListPtr		inRecTypeList,
														tDataNodePtr		inAttrType,
														tDirPatternMatch	inPattMatchType,
														tDataListPtr		inPatterns2Match,
														tDataListPtr		inAttrTypeRequestList,
														dsBool				inAttrInfoOnly,
														UInt32			   *inOutMatchRecordCount,
														tContextData	   *ioContinueData )
{
	tDirStatus			outResult	= eDSNoErr;
	SInt32				siStatus	= eDSNoErr;
	UInt32				messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inDirNodeRef == 0, eDSInvalidReference);
		//ability to not request record count is allowed
		//ability to accept continue data not enforced
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			outResult = VerifyTDataBuff( inOutDataBuff, eDSNullDataBuff, eDSEmptyBuffer );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inRecTypeList, eDSNullRecTypeList, eDSEmptyRecordTypeList );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTDataBuff( inAttrType, eDSNullAttributeType, eDSEmptyAttributeType );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inPatterns2Match, eDSNullAttributeValue, eDSEmptyPatternMatch );
			LogThenThrowIfDSErrorMacro( outResult );
			
			outResult = VerifyTNodeList( inAttrTypeRequestList, eDSNullAttributeRequestList, eDSEmptyAttributeRequestList );
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Add the node reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inDirNodeRef, eNodeRefType, gProcessPID), ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// Add the return buffer length
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inOutDataBuff->fBufferSize, kOutBuffLen );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the record type list
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inRecTypeList, kRecTypeList );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the attribute type
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inAttrType, kAttrType );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			// Add the pattern match value
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inPattMatchType, kAttrPattMatch );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 4 );
			
			// Add the pattern match
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inPatterns2Match, kAttrMatches );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 5 );
			
			// Add the attribute request type list
			siStatus = gMessageTable[messageIndex]->Add_tDataList_ToMsg( inAttrTypeRequestList, kAttrTypeRequestList );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 6 );
			
			// Add the Attribute Info Only boolean
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inAttrInfoOnly, kAttrInfoOnly );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 6 );
			
			if ( inOutMatchRecordCount != nil )
			{
				if (*inOutMatchRecordCount < 0 )
				{
					*inOutMatchRecordCount = 0;
				}
				// Add the record count
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg ( *inOutMatchRecordCount, kMatchRecCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 7 );
			}
			
			if ( ioContinueData != nil )
			{
				// Add the context data
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( (UInt32)*ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 8 );
			}
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kDoMultipleAttributeValueSearchWithData );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			if ( outResult != eDSBufferTooSmall )
			{
				LogThenThrowIfDSErrorMacro( outResult );
			}
			
			// Get the data buffer 
			siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &inOutDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoDataBuff );
			
			if ( inOutMatchRecordCount != nil )
			{
				// Get the record count
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( inOutMatchRecordCount, kMatchRecCount );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoRecMatchCount );
			}
			
			if ( ioContinueData != nil )
			{
				// Get the context data
				siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)ioContinueData, kContextData );
				LogThenThrowThisIfDSErrorMacro( siStatus, eDataReceiveErr_NoContinueData );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsDoMultipleAttributeValueSearchWithData


//--------------------------------------------------------------------------------------------------
//
//	Name:	dsDoPlugInCustomCall
//
//	Params:	
//
//	Notes:
//
//--------------------------------------------------------------------------------------------------

tDirStatus	dsDoPlugInCustomCall (	tDirNodeReference	inNodeRef,
									UInt32				inRequestCode,
									tDataBuffer		   *inDataBuff,
									tDataBuffer		   *outDataBuff )
{
	tDirStatus		outResult	= eDSNoErr;
	SInt32			siStatus	= eDSNoErr;
	UInt32			blockLen	= 0;
	UInt32			messageIndex= 0;
	UInt32			serverNodeRef = 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	try
	{
		LogThenThrowIfTrueMacro(inNodeRef == 0, eDSInvalidReference);
		messageIndex = gFWRefMap->GetMessageTableIndex(inNodeRef, eNodeRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Make sure we have a non-null data buffer
			if ( inDataBuff == NULL )
				outResult = eDSNullDataBuff;
			LogThenThrowIfDSErrorMacro( outResult );
			
			// Calculate the send block length
			if ( outDataBuff == nil )
			{
				blockLen = inDataBuff->fBufferSize;
			}
			else
			{
				blockLen = inDataBuff->fBufferSize + outDataBuff->fBufferSize;
			}
			
			//set up the server ref to FW ref mapping
			serverNodeRef = gFWRefMap->GetRefNum(inNodeRef, eNodeRefType, gProcessPID);
#ifdef __LITTLE_ENDIAN__
			gFWRefMap->MapServerRefToLocalRef(serverNodeRef, inNodeRef);
#endif
			// Add the node reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( serverNodeRef, ktNodeRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			//Add the node map reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inNodeRef, ktNodeRefMap );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 1 );
			
			// Add the request code
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( inRequestCode, kCustomRequestCode );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 2 );
			
			// Add the incoming data buffer
			siStatus = gMessageTable[messageIndex]->Add_tDataBuff_ToMsg( inDataBuff, ktDataBuff );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 3 );
			
			if ( outDataBuff != nil )
			{
				// Add the return buffer length
				siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( outDataBuff->fBufferSize, kOutBuffLen );
				LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError - 4 );
			}
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kDoPlugInCustomCall );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
			
			if ( outDataBuff != nil )
			{
				// Get the data buffer 
				siStatus = gMessageTable[messageIndex]->Get_tDataBuff_FromMsg( &outDataBuff, ktDataBuff );
			}
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}
		
		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}

	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsDoPlugInCustomCall
											

//--------------------------------------------------------------------------------------------------
//
//	Name:	dsVerifyDirRefNum
//
//--------------------------------------------------------------------------------------------------

tDirStatus dsVerifyDirRefNum ( tDirReference inDirRef )
{
	tDirStatus		outResult	= eDSNoErr;
	UInt32			siStatus	= eDSNoErr;
	UInt32			messageIndex= 0;

	pthread_once( &_gGlobalsInitialized, __InitGlobals );

	if ( inDirRef == 0x00F0F0F0 )
	{
		return( outResult );
	}
	if ( inDirRef == 0x0 )
	{
		return( eDSInvalidReference );
	}

	try
	{
		messageIndex = gFWRefMap->GetMessageTableIndex(inDirRef, eDirectoryRefType, gProcessPID);
		LogThenThrowIfTrueMacro( messageIndex > gMaxEndpoints, eDSRefTableIndexOutOfBoundsError );
		LogThenThrowIfNilMacro( gMessageTable[messageIndex], eDSRefTableEntryNilError );

		gMessageTable[messageIndex]->Lock();

		try
		{
			gMessageTable[messageIndex]->ClearMessageBlock();
			
			// Add the directory reference
			siStatus = gMessageTable[messageIndex]->Add_Value_ToMsg( gFWRefMap->GetRefNum(inDirRef, eDirectoryRefType, gProcessPID), ktDirRef );
			LogThenThrowThisIfDSErrorMacro( siStatus, eParameterSendError );
			
			// **************** Send the message ****************
			siStatus = gMessageTable[messageIndex]->SendInlineMessage( kVerifyDirRefNum );
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// **************** Get the reply ****************
			siStatus = (tDirStatus)gMessageTable[messageIndex]->GetReplyMessage();
			LogThenThrowIfDSErrorMacro( siStatus );
			
			// Get the return result
			siStatus = gMessageTable[messageIndex]->Get_Value_FromMsg( (UInt32 *)&outResult, kResult );
			LogThenThrowIfDSErrorMacro( outResult );
		}
		catch ( SInt32 err )
		{
			outResult = (tDirStatus)err;
		}
		catch ( ... )
		{
			outResult = eDSCannotAccessSession;
		}

		gMessageTable[messageIndex]->Unlock();
	}

	catch( SInt32 err )
	{
		outResult = (tDirStatus)err;
	}
	catch ( ... )
	{
		outResult = eDSCannotAccessSession;
	}
		
	CheckToCleanUpLostTCPConnection(&outResult, messageIndex, __LINE__);
	return( outResult );

} // dsVerifyDirRefNum

//--------------------------------------------------------------------------------------------------
//
//	Name:	CheckToCleanUpLostTCPConnection
//
//--------------------------------------------------------------------------------------------------

void CheckToCleanUpLostTCPConnection ( tDirStatus *inStatus, UInt32 inMessageIndex, UInt32 lineNumber )
{
    pthread_once( &_gGlobalsInitialized, __InitGlobals );

	if (*inStatus != eDSNoErr)
	{
		if ( inMessageIndex != 0 ) //not mach endpoint
		{
			if (	( *inStatus == eDSTCPReceiveError ) ||
					( *inStatus == eDSTCPSendError ) ) //TCP related error
			{
				*inStatus = eDSCannotAccessSession;
				if (	( gMessageTable[inMessageIndex] != nil ) )
				{
					gLock->WaitLock();
					if (gMessageTable[inMessageIndex] != nil )
					{
						LOG1( kStdErr, "DirServices::CheckToCleanUpLostTCPConnection: TCP connection was lost - refer to line %d.", lineNumber );
						gMessageTable[inMessageIndex]->Lock();
						gMessageTable[inMessageIndex]->CloseTCPEndpoint();
						gMessageTable[inMessageIndex]->Unlock();
						delete(gMessageTable[inMessageIndex]);
						gMessageTable[inMessageIndex] = nil;
					}
					gLock->SignalLock();
				}
			}
		}
	}
} // CheckToCleanUpLostTCPConnection
