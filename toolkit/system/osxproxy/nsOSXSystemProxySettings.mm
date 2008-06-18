/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et: */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Diane Trout.
 *
 * Portions created by the Initial Developer are Copyright (C) 2006
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *    James Bunton (jamesbunton@fastmail.fm)
 *    Diane Trout (diane@ghic.org)
 *    Robert O'Callahan (rocallahan@novell.com)
 *    Håkan Waara (hwaara@gmail.com)
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#import <Cocoa/Cocoa.h>
#import <SystemConfiguration/SystemConfiguration.h>

#include "nsISystemProxySettings.h"
#include "nsIGenericFactory.h"
#include "nsIServiceManager.h"
#include "nsPrintfCString.h"
#include "nsNetUtil.h"
#include "nsISupportsPrimitives.h"
#include "nsIURI.h"
#include "nsObjCExceptions.h"


class nsOSXSystemProxySettings : public nsISystemProxySettings {
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSISYSTEMPROXYSETTINGS

  nsOSXSystemProxySettings();
  nsresult Init();

  // called by OSX when the proxy settings have changed
  void ProxyHasChanged();

  // is there a PAC url specified in the system configuration
  PRBool IsAutoconfigEnabled() const;
  // retrieve the pac url
  nsresult GetAutoconfigURL(nsCAutoString& aResult) const;

  // Find the SystemConfiguration proxy & port for a given URI
  nsresult FindSCProxyPort(nsIURI* aURI, nsACString& aResultHost, PRInt32& aResultPort);

  // is host:port on the proxy exception list?
  PRBool IsInExceptionList(const nsACString& aHost, PRInt32 aPort) const;

private:
  ~nsOSXSystemProxySettings();

  SCDynamicStoreContext mContext;
  SCDynamicStoreRef mSystemDynamicStore;
  NSDictionary* mProxyDict;


  // Mapping of URI schemes to SystemConfiguration keys
  struct SchemeMapping {
    const char* mScheme;
    CFStringRef mEnabled;
    CFStringRef mHost;
    CFStringRef mPort;
  };
  static const SchemeMapping gSchemeMappingList[];
};

NS_IMPL_ISUPPORTS1(nsOSXSystemProxySettings, nsISystemProxySettings)

// Mapping of URI schemes to SystemConfiguration keys
const nsOSXSystemProxySettings::SchemeMapping nsOSXSystemProxySettings::gSchemeMappingList[] = {
  {"http", kSCPropNetProxiesHTTPEnable, kSCPropNetProxiesHTTPProxy, kSCPropNetProxiesHTTPPort},
  {"https", kSCPropNetProxiesHTTPSEnable, kSCPropNetProxiesHTTPSProxy, kSCPropNetProxiesHTTPSPort},
  {"ftp", kSCPropNetProxiesFTPEnable, kSCPropNetProxiesFTPProxy, kSCPropNetProxiesFTPPort},
  {"socks", kSCPropNetProxiesSOCKSEnable, kSCPropNetProxiesSOCKSProxy, kSCPropNetProxiesSOCKSPort},
  {NULL, NULL, NULL, NULL},
};

static void
ProxyHasChangedWrapper(SCDynamicStoreRef aStore, CFArrayRef aChangedKeys, void* aInfo)
{
  static_cast<nsOSXSystemProxySettings*>(aInfo)->ProxyHasChanged();
}


nsOSXSystemProxySettings::nsOSXSystemProxySettings()
  : mSystemDynamicStore(NULL), mProxyDict(NULL)
{
  mContext = (SCDynamicStoreContext){0, this, NULL, NULL, NULL};
}

nsresult
nsOSXSystemProxySettings::Init()
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK_NSRESULT;

  // Register for notification of proxy setting changes
  // See: http://developer.apple.com/documentation/Networking/Conceptual/CFNetwork/CFStreamTasks/chapter_4_section_5.html
  mSystemDynamicStore = SCDynamicStoreCreate(NULL, CFSTR("Mozilla"), ProxyHasChangedWrapper, &mContext);
  if (!mSystemDynamicStore)
    return NS_ERROR_FAILURE;

  // Set up the store to monitor any changes to the proxies
  CFStringRef proxiesKey = SCDynamicStoreKeyCreateProxies(NULL);
  if (!proxiesKey)
    return NS_ERROR_FAILURE;

  CFArrayRef keyArray = CFArrayCreate(NULL, (const void**)(&proxiesKey), 1, &kCFTypeArrayCallBacks);
  CFRelease(proxiesKey);
  if (!keyArray)
    return NS_ERROR_FAILURE;

  SCDynamicStoreSetNotificationKeys(mSystemDynamicStore, keyArray, NULL);
  CFRelease(keyArray);

  // Add the dynamic store to the run loop
  CFRunLoopSourceRef storeRLSource = SCDynamicStoreCreateRunLoopSource(NULL, mSystemDynamicStore, 0);
  if (!storeRLSource)
    return NS_ERROR_FAILURE;
  CFRunLoopAddSource(CFRunLoopGetCurrent(), storeRLSource, kCFRunLoopCommonModes);
  CFRelease(storeRLSource);

  // Load the initial copy of proxy info
  mProxyDict = (NSDictionary*)SCDynamicStoreCopyProxies(mSystemDynamicStore);
  if (!mProxyDict)
    return NS_ERROR_FAILURE;

  return NS_OK;

  NS_OBJC_END_TRY_ABORT_BLOCK_NSRESULT;
}

nsOSXSystemProxySettings::~nsOSXSystemProxySettings()
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK;

  [mProxyDict release];

  if (mSystemDynamicStore) {
    // Invalidate the dynamic store's run loop source
    // to get the store out of the run loop
    CFRunLoopSourceRef rls = SCDynamicStoreCreateRunLoopSource(NULL, mSystemDynamicStore, 0);
    if (rls) {
      CFRunLoopSourceInvalidate(rls);
      CFRelease(rls);
    }
    CFRelease(mSystemDynamicStore);
  }

  NS_OBJC_END_TRY_ABORT_BLOCK;
}


void
nsOSXSystemProxySettings::ProxyHasChanged()
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK;

  [mProxyDict release];
  mProxyDict = (NSDictionary*)SCDynamicStoreCopyProxies(mSystemDynamicStore);

  NS_OBJC_END_TRY_ABORT_BLOCK;
}

nsresult
nsOSXSystemProxySettings::FindSCProxyPort(nsIURI* aURI, nsACString& aResultHost, PRInt32& aResultPort)
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK_NSRESULT;

  NS_ENSURE_TRUE(mProxyDict != NULL, NS_ERROR_FAILURE);

  for (const SchemeMapping* keys = gSchemeMappingList; keys->mScheme != NULL; ++keys) {
    // Check for matching scheme
    PRBool res;
    if (NS_FAILED(aURI->SchemeIs(keys->mScheme, &res)) || !res) {
      continue;
    }

    // Check the proxy is enabled
    NSNumber* enabled = [mProxyDict objectForKey:(NSString*)keys->mEnabled];
    NS_ENSURE_TRUE(enabled == NULL || [enabled isKindOfClass:[NSNumber class]], NS_ERROR_FAILURE);
    if ([enabled intValue] == 0)
      break;

    // Get the proxy host
    NSString* host = [mProxyDict objectForKey:(NSString*)keys->mHost];
    if (host == NULL)
      break;
    NS_ENSURE_TRUE([host isKindOfClass:[NSString class]], NS_ERROR_FAILURE);
    aResultHost.Assign([host UTF8String]);

    // Get the proxy port
    NSNumber* port = [mProxyDict objectForKey:(NSString*)keys->mPort];
    NS_ENSURE_TRUE([port isKindOfClass:[NSNumber class]], NS_ERROR_FAILURE);
    aResultPort = [port intValue];

    return NS_OK;
  }

  return NS_ERROR_FAILURE;

  NS_OBJC_END_TRY_ABORT_BLOCK_NSRESULT;
}

PRBool
nsOSXSystemProxySettings::IsAutoconfigEnabled() const
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK_RETURN;

  NSNumber* value = [mProxyDict objectForKey:(NSString*)kSCPropNetProxiesProxyAutoConfigEnable];
  NS_ENSURE_TRUE(value == NULL || [value isKindOfClass:[NSNumber class]], PR_FALSE);
  return ([value intValue] != 0);

  NS_OBJC_END_TRY_ABORT_BLOCK_RETURN(PR_FALSE);
}

nsresult
nsOSXSystemProxySettings::GetAutoconfigURL(nsCAutoString& aResult) const
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK_NSRESULT;

  NSString* value = [mProxyDict objectForKey:(NSString*)kSCPropNetProxiesProxyAutoConfigURLString];
  if (value != NULL) {
    NS_ENSURE_TRUE([value isKindOfClass:[NSString class]], NS_ERROR_FAILURE);
    aResult.Assign([value UTF8String]);
    return NS_OK;
  }

  return NS_ERROR_FAILURE;

  NS_OBJC_END_TRY_ABORT_BLOCK_NSRESULT;
}

static PRBool
IsHostProxyEntry(const nsACString& aHost, PRInt32 aPort, NSString* aStr)
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK_RETURN;

  nsCAutoString proxyEntry([aStr UTF8String]);

  nsReadingIterator<char> start;
  nsReadingIterator<char> colon;
  nsReadingIterator<char> end;

  proxyEntry.BeginReading(start);
  proxyEntry.EndReading(end);
  colon = start;
  PRInt32 port = -1;

  if (FindCharInReadable(':', colon, end)) {
    ++colon;
    nsDependentCSubstring portStr(colon, end);
    nsCAutoString portStr2(portStr);
    PRInt32 err;
    port = portStr2.ToInteger(&err);
    if (err != 0) {
      port = -2; // don't match any port, so we ignore this pattern
    }
    --colon;
  } else {
    colon = end;
  }

  if (port == -1 || port == aPort) {
    nsDependentCSubstring hostStr(start, colon);
    if (StringEndsWith(aHost, hostStr, nsCaseInsensitiveCStringComparator())) {
      return PR_TRUE;
    }
  }

  NS_OBJC_END_TRY_ABORT_BLOCK_RETURN(PR_FALSE);
}

PRBool
nsOSXSystemProxySettings::IsInExceptionList(const nsACString& aHost,
                                            PRInt32 aPort) const
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK_RETURN;

  NS_ENSURE_TRUE(mProxyDict != NULL, PR_FALSE);

  NSArray* exceptionList = [mProxyDict objectForKey:(NSString*)kSCPropNetProxiesExceptionsList];
  NS_ENSURE_TRUE(exceptionList == NULL || [exceptionList isKindOfClass:[NSArray class]], PR_FALSE);

  NSEnumerator* exceptionEnumerator = [exceptionList objectEnumerator];
  NSString* currentValue = NULL;
  while ((currentValue = [exceptionEnumerator nextObject])) {
    NS_ENSURE_TRUE([currentValue isKindOfClass:[NSString class]], PR_FALSE);
    if (IsHostProxyEntry(aHost, aPort, currentValue))
      return PR_TRUE;
  }

  NS_OBJC_END_TRY_ABORT_BLOCK_RETURN(PR_FALSE);
}


nsresult
nsOSXSystemProxySettings::GetPACURI(nsACString& aResult)
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK_NSRESULT;

  NS_ENSURE_TRUE(mProxyDict != NULL, NS_ERROR_FAILURE);

  nsCAutoString pacUrl;
  if (IsAutoconfigEnabled() && NS_SUCCEEDED(GetAutoconfigURL(pacUrl))) {
    aResult.Assign(pacUrl);
    return NS_OK;
  }

  return NS_ERROR_FAILURE;

  NS_OBJC_END_TRY_ABORT_BLOCK_NSRESULT;
}

nsresult
nsOSXSystemProxySettings::GetProxyForURI(nsIURI* aURI, nsACString& aResult)
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK_NSRESULT;

  nsCAutoString host;
  nsresult rv = aURI->GetHost(host);
  NS_ENSURE_SUCCESS(rv, rv);

  PRInt32 port;
  rv = aURI->GetPort(&port);
  NS_ENSURE_SUCCESS(rv, rv);

  PRInt32 proxyPort;
  nsCAutoString proxyHost;
  rv = FindSCProxyPort(aURI, proxyHost, proxyPort);

  if (NS_FAILED(rv) || IsInExceptionList(host, port)) {
    aResult.AssignLiteral("DIRECT");
  } else {
    aResult.Assign(NS_LITERAL_CSTRING("PROXY ") + proxyHost + nsPrintfCString(":%d", proxyPort));
  }

  return NS_OK;

  NS_OBJC_END_TRY_ABORT_BLOCK_NSRESULT;
}

#define NS_OSXSYSTEMPROXYSERVICE_CID  /* 9afcd4b8-2e0f-41f4-8f1f-3bf0d3cf67de */\
    { 0x9afcd4b8, 0x2e0f, 0x41f4, \
      { 0x8f, 0x1f, 0x3b, 0xf0, 0xd3, 0xcf, 0x67, 0xde } }

NS_GENERIC_FACTORY_CONSTRUCTOR_INIT(nsOSXSystemProxySettings, Init);

static const nsModuleComponentInfo components[] = {
  { "OSX System Proxy Settings Service",
    NS_OSXSYSTEMPROXYSERVICE_CID,
    NS_SYSTEMPROXYSETTINGS_CONTRACTID,
    nsOSXSystemProxySettingsConstructor }
};

NS_IMPL_NSGETMODULE(nsOSXProxyModule, components)
