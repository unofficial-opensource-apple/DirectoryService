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
 * @header CPluginHandler
 */

#ifndef __CPluginHandler_h__
#define __CPluginHandler_h__ 1

#include "CInternalDispatchThread.h"
#include "DSEventSemaphore.h"
#include "PrivateTypes.h"
#include "SharedConsts.h"

class	CPluginHandler;
extern	CPluginHandler	*gPluginHandler;		

class CPluginHandler : public CInternalDispatchThread
{
public:
					CPluginHandler		( void );
	virtual		   ~CPluginHandler		( void );

	virtual	long	ThreadMain			( void );		// we manage out own thread top level
	virtual	void	StartThread			( void );
	virtual	void	StopThread			( void );

protected:

private:
	sInt32			LoadPlugins			( void );

};

#endif
