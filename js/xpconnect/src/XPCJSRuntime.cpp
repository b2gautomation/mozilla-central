/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
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
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   John Bandhauer <jband@netscape.com> (original author)
 *   Nicholas Nethercote <nnethercote@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
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

/* Per JSRuntime object */

#include "mozilla/Util.h"

#include "xpcprivate.h"
#include "xpcpublic.h"
#include "WrapperFactory.h"
#include "dom_quickstubs.h"

#include "nsIMemoryReporter.h"
#include "nsPrintfCString.h"
#include "mozilla/FunctionTimer.h"
#include "prsystem.h"
#include "mozilla/Preferences.h"
#include "mozilla/Telemetry.h"

#include "nsContentUtils.h"
#include "nsCCUncollectableMarker.h"
#include "jsfriendapi.h"
#include "js/MemoryMetrics.h"

#ifdef MOZ_CRASHREPORTER
#include "nsExceptionHandler.h"
#endif

using namespace mozilla;
using namespace mozilla::xpconnect::memory;

/***************************************************************************/

const char* XPCJSRuntime::mStrings[] = {
    "constructor",          // IDX_CONSTRUCTOR
    "toString",             // IDX_TO_STRING
    "toSource",             // IDX_TO_SOURCE
    "lastResult",           // IDX_LAST_RESULT
    "returnCode",           // IDX_RETURN_CODE
    "value",                // IDX_VALUE
    "QueryInterface",       // IDX_QUERY_INTERFACE
    "Components",           // IDX_COMPONENTS
    "wrappedJSObject",      // IDX_WRAPPED_JSOBJECT
    "Object",               // IDX_OBJECT
    "Function",             // IDX_FUNCTION
    "prototype",            // IDX_PROTOTYPE
    "createInstance",       // IDX_CREATE_INSTANCE
    "item",                 // IDX_ITEM
    "__proto__",            // IDX_PROTO
    "__iterator__",         // IDX_ITERATOR
    "__exposedProps__",     // IDX_EXPOSEDPROPS
    "__scriptOnly__",       // IDX_SCRIPTONLY
    "baseURIObject",        // IDX_BASEURIOBJECT
    "nodePrincipal",        // IDX_NODEPRINCIPAL
    "documentURIObject"     // IDX_DOCUMENTURIOBJECT
};

/***************************************************************************/

static JSDHashOperator
WrappedJSDyingJSObjectFinder(JSDHashTable *table, JSDHashEntryHdr *hdr,
                             uint32_t number, void *arg)
{
    nsTArray<nsXPCWrappedJS*>* array = static_cast<nsTArray<nsXPCWrappedJS*>*>(arg);
    nsXPCWrappedJS* wrapper = ((JSObject2WrappedJSMap::Entry*)hdr)->value;
    NS_ASSERTION(wrapper, "found a null JS wrapper!");

    // walk the wrapper chain and find any whose JSObject is to be finalized
    while (wrapper) {
        if (wrapper->IsSubjectToFinalization()) {
            if (JS_IsAboutToBeFinalized(wrapper->GetJSObjectPreserveColor()))
                array->AppendElement(wrapper);
        }
        wrapper = wrapper->GetNextWrapper();
    }
    return JS_DHASH_NEXT;
}

struct CX_AND_XPCRT_Data
{
    JSContext* cx;
    XPCJSRuntime* rt;
};

static JSDHashOperator
NativeInterfaceSweeper(JSDHashTable *table, JSDHashEntryHdr *hdr,
                       uint32_t number, void *arg)
{
    XPCNativeInterface* iface = ((IID2NativeInterfaceMap::Entry*)hdr)->value;
    if (iface->IsMarked()) {
        iface->Unmark();
        return JS_DHASH_NEXT;
    }

#ifdef XPC_REPORT_NATIVE_INTERFACE_AND_SET_FLUSHING
    fputs("- Destroying XPCNativeInterface for ", stdout);
    JS_PutString(JSVAL_TO_STRING(iface->GetName()), stdout);
    putc('\n', stdout);
#endif

    XPCNativeInterface::DestroyInstance(iface);
    return JS_DHASH_REMOVE;
}

// *Some* NativeSets are referenced from mClassInfo2NativeSetMap.
// *All* NativeSets are referenced from mNativeSetMap.
// So, in mClassInfo2NativeSetMap we just clear references to the unmarked.
// In mNativeSetMap we clear the references to the unmarked *and* delete them.

static JSDHashOperator
NativeUnMarkedSetRemover(JSDHashTable *table, JSDHashEntryHdr *hdr,
                         uint32_t number, void *arg)
{
    XPCNativeSet* set = ((ClassInfo2NativeSetMap::Entry*)hdr)->value;
    if (set->IsMarked())
        return JS_DHASH_NEXT;
    return JS_DHASH_REMOVE;
}

static JSDHashOperator
NativeSetSweeper(JSDHashTable *table, JSDHashEntryHdr *hdr,
                 uint32_t number, void *arg)
{
    XPCNativeSet* set = ((NativeSetMap::Entry*)hdr)->key_value;
    if (set->IsMarked()) {
        set->Unmark();
        return JS_DHASH_NEXT;
    }

#ifdef XPC_REPORT_NATIVE_INTERFACE_AND_SET_FLUSHING
    printf("- Destroying XPCNativeSet for:\n");
    PRUint16 count = set->GetInterfaceCount();
    for (PRUint16 k = 0; k < count; k++) {
        XPCNativeInterface* iface = set->GetInterfaceAt(k);
        fputs("    ", stdout);
        JS_PutString(JSVAL_TO_STRING(iface->GetName()), stdout);
        putc('\n', stdout);
    }
#endif

    XPCNativeSet::DestroyInstance(set);
    return JS_DHASH_REMOVE;
}

static JSDHashOperator
JSClassSweeper(JSDHashTable *table, JSDHashEntryHdr *hdr,
               uint32_t number, void *arg)
{
    XPCNativeScriptableShared* shared =
        ((XPCNativeScriptableSharedMap::Entry*) hdr)->key;
    if (shared->IsMarked()) {
#ifdef off_XPC_REPORT_JSCLASS_FLUSHING
        printf("+ Marked XPCNativeScriptableShared for: %s @ %x\n",
               shared->GetJSClass()->name,
               shared->GetJSClass());
#endif
        shared->Unmark();
        return JS_DHASH_NEXT;
    }

#ifdef XPC_REPORT_JSCLASS_FLUSHING
    printf("- Destroying XPCNativeScriptableShared for: %s @ %x\n",
           shared->GetJSClass()->name,
           shared->GetJSClass());
#endif

    delete shared;
    return JS_DHASH_REMOVE;
}

static JSDHashOperator
DyingProtoKiller(JSDHashTable *table, JSDHashEntryHdr *hdr,
                 uint32_t number, void *arg)
{
    XPCWrappedNativeProto* proto =
        (XPCWrappedNativeProto*)((JSDHashEntryStub*)hdr)->key;
    delete proto;
    return JS_DHASH_REMOVE;
}

static JSDHashOperator
DetachedWrappedNativeProtoMarker(JSDHashTable *table, JSDHashEntryHdr *hdr,
                                 uint32_t number, void *arg)
{
    XPCWrappedNativeProto* proto =
        (XPCWrappedNativeProto*)((JSDHashEntryStub*)hdr)->key;

    proto->Mark();
    return JS_DHASH_NEXT;
}

// GCCallback calls are chained
static JSBool
ContextCallback(JSContext *cx, unsigned operation)
{
    XPCJSRuntime* self = nsXPConnect::GetRuntimeInstance();
    if (self) {
        if (operation == JSCONTEXT_NEW) {
            if (!self->OnJSContextNew(cx))
                return false;
        } else if (operation == JSCONTEXT_DESTROY) {
            delete XPCContext::GetXPCContext(cx);
        }
    }
    return true;
}

xpc::CompartmentPrivate::~CompartmentPrivate()
{
    MOZ_COUNT_DTOR(xpc::CompartmentPrivate);
}

static JSBool
CompartmentCallback(JSContext *cx, JSCompartment *compartment, unsigned op)
{
    JS_ASSERT(op == JSCOMPARTMENT_DESTROY);

    XPCJSRuntime* self = nsXPConnect::GetRuntimeInstance();
    if (!self)
        return true;

    nsAutoPtr<xpc::CompartmentPrivate>
        priv(static_cast<xpc::CompartmentPrivate*>(JS_GetCompartmentPrivate(compartment)));
    if (!priv)
        return true;

    JS_SetCompartmentPrivate(compartment, nsnull);

    xpc::PtrAndPrincipalHashKey *key = priv->key;
    XPCCompartmentMap &map = self->GetCompartmentMap();
#ifdef DEBUG
    JSCompartment *current = NULL;
    NS_ASSERTION(map.Get(key, &current), "no compartment?");
    NS_ASSERTION(current == compartment, "compartment mismatch");
#endif
    map.Remove(key);

    return true;
}

struct ObjectHolder : public JSDHashEntryHdr
{
    void *holder;
    nsScriptObjectTracer* tracer;
};

nsresult
XPCJSRuntime::AddJSHolder(void* aHolder, nsScriptObjectTracer* aTracer)
{
    if (!mJSHolders.ops)
        return NS_ERROR_OUT_OF_MEMORY;

    ObjectHolder *entry =
        reinterpret_cast<ObjectHolder*>(JS_DHashTableOperate(&mJSHolders,
                                                             aHolder,
                                                             JS_DHASH_ADD));
    if (!entry)
        return NS_ERROR_OUT_OF_MEMORY;

    entry->holder = aHolder;
    entry->tracer = aTracer;

    return NS_OK;
}

nsresult
XPCJSRuntime::RemoveJSHolder(void* aHolder)
{
    if (!mJSHolders.ops)
        return NS_ERROR_OUT_OF_MEMORY;

    JS_DHashTableOperate(&mJSHolders, aHolder, JS_DHASH_REMOVE);

    return NS_OK;
}

// static
void XPCJSRuntime::TraceBlackJS(JSTracer* trc, void* data)
{
    XPCJSRuntime* self = (XPCJSRuntime*)data;

    // Skip this part if XPConnect is shutting down. We get into
    // bad locking problems with the thread iteration otherwise.
    if (!self->GetXPConnect()->IsShuttingDown()) {
        Mutex* threadLock = XPCPerThreadData::GetLock();
        if (threadLock)
        { // scoped lock
            MutexAutoLock lock(*threadLock);

            XPCPerThreadData* iterp = nsnull;
            XPCPerThreadData* thread;

            while (nsnull != (thread =
                              XPCPerThreadData::IterateThreads(&iterp))) {
                // Trace those AutoMarkingPtr lists!
                thread->TraceJS(trc);
            }
        }
    }

    {
        XPCAutoLock lock(self->mMapLock);

        // XPCJSObjectHolders don't participate in cycle collection, so always
        // trace them here.
        XPCRootSetElem *e;
        for (e = self->mObjectHolderRoots; e; e = e->GetNextRoot())
            static_cast<XPCJSObjectHolder*>(e)->TraceJS(trc);
    }
}

// static
void XPCJSRuntime::TraceGrayJS(JSTracer* trc, void* data)
{
    XPCJSRuntime* self = (XPCJSRuntime*)data;

    // Mark these roots as gray so the CC can walk them later.
    self->TraceXPConnectRoots(trc);
}

static void
TraceJSObject(PRUint32 aLangID, void *aScriptThing, const char *name,
              void *aClosure)
{
    if (aLangID == nsIProgrammingLanguage::JAVASCRIPT) {
        JS_CALL_TRACER(static_cast<JSTracer*>(aClosure), aScriptThing,
                       js_GetGCThingTraceKind(aScriptThing), name);
    }
}

static JSDHashOperator
TraceJSHolder(JSDHashTable *table, JSDHashEntryHdr *hdr, uint32_t number,
              void *arg)
{
    ObjectHolder* entry = reinterpret_cast<ObjectHolder*>(hdr);

    entry->tracer->Trace(entry->holder, TraceJSObject, arg);

    return JS_DHASH_NEXT;
}

struct ClearedGlobalObject : public JSDHashEntryHdr
{
    JSContext* mContext;
    JSObject* mGlobalObject;
};

static PLDHashOperator
TraceExpandos(XPCWrappedNative *wn, JSObject *&expando, void *aClosure)
{
    if (wn->IsWrapperExpired())
        return PL_DHASH_REMOVE;
    JS_CALL_OBJECT_TRACER(static_cast<JSTracer *>(aClosure), expando, "expando object");
    return PL_DHASH_NEXT;
}

static PLDHashOperator
TraceDOMExpandos(nsPtrHashKey<JSObject> *expando, void *aClosure)
{
    JS_CALL_OBJECT_TRACER(static_cast<JSTracer *>(aClosure), expando->GetKey(),
                          "DOM expando object");
    return PL_DHASH_NEXT;
}

static PLDHashOperator
TraceCompartment(xpc::PtrAndPrincipalHashKey *aKey, JSCompartment *compartment, void *aClosure)
{
    xpc::CompartmentPrivate *priv =
        static_cast<xpc::CompartmentPrivate *>(JS_GetCompartmentPrivate(compartment));
    if (priv->expandoMap)
        priv->expandoMap->Enumerate(TraceExpandos, aClosure);
    if (priv->domExpandoMap)
        priv->domExpandoMap->EnumerateEntries(TraceDOMExpandos, aClosure);
    return PL_DHASH_NEXT;
}

void XPCJSRuntime::TraceXPConnectRoots(JSTracer *trc)
{
    JSContext *iter = nsnull;
    while (JSContext *acx = JS_ContextIterator(GetJSRuntime(), &iter)) {
        JS_ASSERT(js::HasUnrootedGlobal(acx));
        if (JSObject *global = JS_GetGlobalObject(acx))
            JS_CALL_OBJECT_TRACER(trc, global, "XPC global object");
    }

    XPCAutoLock lock(mMapLock);

    XPCWrappedNativeScope::TraceJS(trc, this);

    for (XPCRootSetElem *e = mVariantRoots; e ; e = e->GetNextRoot())
        static_cast<XPCTraceableVariant*>(e)->TraceJS(trc);

    for (XPCRootSetElem *e = mWrappedJSRoots; e ; e = e->GetNextRoot())
        static_cast<nsXPCWrappedJS*>(e)->TraceJS(trc);

    if (mJSHolders.ops)
        JS_DHashTableEnumerate(&mJSHolders, TraceJSHolder, trc);

    // Trace compartments.
    GetCompartmentMap().EnumerateRead(TraceCompartment, trc);
}

struct Closure
{
    bool cycleCollectionEnabled;
    nsCycleCollectionTraversalCallback *cb;
};

static void
CheckParticipatesInCycleCollection(PRUint32 aLangID, void *aThing,
                                   const char *name, void *aClosure)
{
    Closure *closure = static_cast<Closure*>(aClosure);

    closure->cycleCollectionEnabled =
        aLangID == nsIProgrammingLanguage::JAVASCRIPT &&
        AddToCCKind(js_GetGCThingTraceKind(aThing));
}

static JSDHashOperator
NoteJSHolder(JSDHashTable *table, JSDHashEntryHdr *hdr, uint32_t number,
             void *arg)
{
    ObjectHolder* entry = reinterpret_cast<ObjectHolder*>(hdr);
    Closure *closure = static_cast<Closure*>(arg);

    entry->tracer->Trace(entry->holder, CheckParticipatesInCycleCollection,
                         closure);
    if (!closure->cycleCollectionEnabled)
        return JS_DHASH_NEXT;

    closure->cb->NoteRoot(nsIProgrammingLanguage::CPLUSPLUS, entry->holder,
                          entry->tracer);

    return JS_DHASH_NEXT;
}

// static
void
XPCJSRuntime::SuspectWrappedNative(XPCWrappedNative *wrapper,
                                   nsCycleCollectionTraversalCallback &cb)
{
    if (!wrapper->IsValid() || wrapper->IsWrapperExpired())
        return;

    NS_ASSERTION(NS_IsMainThread() || NS_IsCycleCollectorThread(),
                 "Suspecting wrapped natives from non-CC thread");

    // Only record objects that might be part of a cycle as roots, unless
    // the callback wants all traces (a debug feature).
    JSObject* obj = wrapper->GetFlatJSObjectPreserveColor();
    if (xpc_IsGrayGCThing(obj) || cb.WantAllTraces())
        cb.NoteRoot(nsIProgrammingLanguage::JAVASCRIPT, obj,
                    nsXPConnect::GetXPConnect());
}

static PLDHashOperator
SuspectExpandos(XPCWrappedNative *wrapper, JSObject *expando, void *arg)
{
    Closure* closure = static_cast<Closure*>(arg);
    XPCJSRuntime::SuspectWrappedNative(wrapper, *closure->cb);

    return PL_DHASH_NEXT;
}

static PLDHashOperator
SuspectDOMExpandos(nsPtrHashKey<JSObject> *expando, void *arg)
{
    Closure *closure = static_cast<Closure*>(arg);
    closure->cb->NoteXPCOMRoot(static_cast<nsISupports*>(js::GetObjectPrivate(expando->GetKey())));
    return PL_DHASH_NEXT;
}

static PLDHashOperator
SuspectCompartment(xpc::PtrAndPrincipalHashKey *key, JSCompartment *compartment, void *arg)
{
    xpc::CompartmentPrivate *priv =
        static_cast<xpc::CompartmentPrivate *>(JS_GetCompartmentPrivate(compartment));
    if (priv->expandoMap)
        priv->expandoMap->EnumerateRead(SuspectExpandos, arg);
    if (priv->domExpandoMap)
        priv->domExpandoMap->EnumerateEntries(SuspectDOMExpandos, arg);
    return PL_DHASH_NEXT;
}

void
XPCJSRuntime::AddXPConnectRoots(nsCycleCollectionTraversalCallback &cb)
{
    // For all JS objects that are held by native objects but aren't held
    // through rooting or locking, we need to add all the native objects that
    // hold them so that the JS objects are colored correctly in the cycle
    // collector. This includes JSContexts that don't have outstanding requests,
    // because their global object wasn't marked by the JS GC. All other JS
    // roots were marked by the JS GC and will be colored correctly in the cycle
    // collector.

    JSContext *iter = nsnull, *acx;
    while ((acx = JS_ContextIterator(GetJSRuntime(), &iter))) {
        cb.NoteRoot(nsIProgrammingLanguage::CPLUSPLUS, acx,
                    nsXPConnect::JSContextParticipant());
    }

    XPCAutoLock lock(mMapLock);

    XPCWrappedNativeScope::SuspectAllWrappers(this, cb);

    for (XPCRootSetElem *e = mVariantRoots; e ; e = e->GetNextRoot()) {
        XPCTraceableVariant* v = static_cast<XPCTraceableVariant*>(e);
        if (nsCCUncollectableMarker::InGeneration(cb,
                                                  v->CCGeneration())) {
           jsval val = v->GetJSValPreserveColor();
           if (val.isObject() && !xpc_IsGrayGCThing(&val.toObject()))
               continue;
        }
        cb.NoteXPCOMRoot(v);
    }

    for (XPCRootSetElem *e = mWrappedJSRoots; e ; e = e->GetNextRoot()) {
        nsXPCWrappedJS *wrappedJS = static_cast<nsXPCWrappedJS*>(e);
        JSObject *obj = wrappedJS->GetJSObjectPreserveColor();
        // If traversing wrappedJS wouldn't release it, nor
        // cause any other objects to be added to the graph, no
        // need to add it to the graph at all.
        if (nsCCUncollectableMarker::sGeneration &&
            !cb.WantAllTraces() && (!obj || !xpc_IsGrayGCThing(obj)) &&
            !wrappedJS->IsSubjectToFinalization() &&
            wrappedJS->GetRootWrapper() == wrappedJS &&
            !wrappedJS->IsAggregatedToNative()) {
            continue;
        }

        cb.NoteXPCOMRoot(static_cast<nsIXPConnectWrappedJS *>(wrappedJS));
    }

    Closure closure = { true, &cb };
    if (mJSHolders.ops) {
        JS_DHashTableEnumerate(&mJSHolders, NoteJSHolder, &closure);
    }

    // Suspect wrapped natives with expando objects.
    GetCompartmentMap().EnumerateRead(SuspectCompartment, &closure);
}

static JSDHashOperator
UnmarkJSHolder(JSDHashTable *table, JSDHashEntryHdr *hdr, uint32_t number,
               void *arg)
{
    ObjectHolder* entry = reinterpret_cast<ObjectHolder*>(hdr);
    entry->tracer->CanSkip(entry->holder, true);
    return JS_DHASH_NEXT;
}

void
XPCJSRuntime::UnmarkSkippableJSHolders()
{
    XPCAutoLock lock(mMapLock);
    if (mJSHolders.ops) {             
        JS_DHashTableEnumerate(&mJSHolders, UnmarkJSHolder, nsnull);
    }
}

void
xpc_UnmarkSkippableJSHolders()
{
    if (nsXPConnect::GetXPConnect() &&
        nsXPConnect::GetXPConnect()->GetRuntime()) {
        nsXPConnect::GetXPConnect()->GetRuntime()->UnmarkSkippableJSHolders();
    }
}

template<class T> static void
DoDeferredRelease(nsTArray<T> &array)
{
    while (1) {
        PRUint32 count = array.Length();
        if (!count) {
            array.Compact();
            break;
        }
        T wrapper = array[count-1];
        array.RemoveElementAt(count-1);
        NS_RELEASE(wrapper);
    }
}

static JSDHashOperator
SweepWaiverWrappers(JSDHashTable *table, JSDHashEntryHdr *hdr,
                    uint32_t number, void *arg)
{
    JSObject *key = ((JSObject2JSObjectMap::Entry *)hdr)->key;
    JSObject *value = ((JSObject2JSObjectMap::Entry *)hdr)->value;

    if (JS_IsAboutToBeFinalized(key) || JS_IsAboutToBeFinalized(value))
        return JS_DHASH_REMOVE;
    return JS_DHASH_NEXT;
}

static PLDHashOperator
SweepExpandos(XPCWrappedNative *wn, JSObject *&expando, void *arg)
{
    return JS_IsAboutToBeFinalized(wn->GetFlatJSObjectPreserveColor())
           ? PL_DHASH_REMOVE
           : PL_DHASH_NEXT;
}

static PLDHashOperator
SweepCompartment(nsCStringHashKey& aKey, JSCompartment *compartment, void *aClosure)
{
    MOZ_ASSERT(!aClosure);
    xpc::CompartmentPrivate *priv =
        static_cast<xpc::CompartmentPrivate *>(JS_GetCompartmentPrivate(compartment));
    if (priv->waiverWrapperMap)
        priv->waiverWrapperMap->Enumerate(SweepWaiverWrappers, nsnull);
    if (priv->expandoMap)
        priv->expandoMap->Enumerate(SweepExpandos, nsnull);
    return PL_DHASH_NEXT;
}

// static
JSBool XPCJSRuntime::GCCallback(JSContext *cx, JSGCStatus status)
{
    XPCJSRuntime* self = nsXPConnect::GetRuntimeInstance();
    if (!self)
        return true;

    switch (status) {
        case JSGC_BEGIN:
        {
            if (!NS_IsMainThread()) {
                return false;
            }

            // We seem to sometime lose the unrooted global flag. Restore it
            // here. FIXME: bug 584495.
            JSContext *iter = nsnull;
            while (JSContext *acx = JS_ContextIterator(JS_GetRuntime(cx), &iter)) {
                if (!js::HasUnrootedGlobal(acx))
                    JS_ToggleOptions(acx, JSOPTION_UNROOTED_GLOBAL);
            }
            break;
        }
        case JSGC_MARK_END:
        {
            NS_ASSERTION(!self->mDoingFinalization, "bad state");

            // mThreadRunningGC indicates that GC is running
            { // scoped lock
                XPCAutoLock lock(self->GetMapLock());
                NS_ASSERTION(!self->mThreadRunningGC, "bad state");
                self->mThreadRunningGC = PR_GetCurrentThread();
            }

            nsTArray<nsXPCWrappedJS*>* dyingWrappedJSArray =
                &self->mWrappedJSToReleaseArray;

            // Add any wrappers whose JSObjects are to be finalized to
            // this array. Note that we do not want to be changing the
            // refcount of these wrappers.
            // We add them to the array now and Release the array members
            // later to avoid the posibility of doing any JS GCThing
            // allocations during the gc cycle.
            self->mWrappedJSMap->
                Enumerate(WrappedJSDyingJSObjectFinder, dyingWrappedJSArray);

            // Find dying scopes.
            XPCWrappedNativeScope::FinishedMarkPhaseOfGC(cx, self);

            // Sweep compartments.
            self->GetCompartmentMap().EnumerateRead((XPCCompartmentMap::EnumReadFunction)
                                                    SweepCompartment, nsnull);

            self->mDoingFinalization = true;
            break;
        }
        case JSGC_FINALIZE_END:
        {
            NS_ASSERTION(self->mDoingFinalization, "bad state");
            self->mDoingFinalization = false;

            // Release all the members whose JSObjects are now known
            // to be dead.
            DoDeferredRelease(self->mWrappedJSToReleaseArray);

#ifdef XPC_REPORT_NATIVE_INTERFACE_AND_SET_FLUSHING
            printf("--------------------------------------------------------------\n");
            int setsBefore = (int) self->mNativeSetMap->Count();
            int ifacesBefore = (int) self->mIID2NativeInterfaceMap->Count();
#endif

            // We use this occasion to mark and sweep NativeInterfaces,
            // NativeSets, and the WrappedNativeJSClasses...

            // Do the marking...
            XPCWrappedNativeScope::MarkAllWrappedNativesAndProtos();

            self->mDetachedWrappedNativeProtoMap->
                Enumerate(DetachedWrappedNativeProtoMarker, nsnull);

            DOM_MarkInterfaces();

            // Mark the sets used in the call contexts. There is a small
            // chance that a wrapper's set will change *while* a call is
            // happening which uses that wrapper's old interfface set. So,
            // we need to do this marking to avoid collecting those sets
            // that might no longer be otherwise reachable from the wrappers
            // or the wrapperprotos.

            // Skip this part if XPConnect is shutting down. We get into
            // bad locking problems with the thread iteration otherwise.
            if (!self->GetXPConnect()->IsShuttingDown()) {
                Mutex* threadLock = XPCPerThreadData::GetLock();
                if (threadLock)
                { // scoped lock
                    MutexAutoLock lock(*threadLock);

                    XPCPerThreadData* iterp = nsnull;
                    XPCPerThreadData* thread;

                    while (nsnull != (thread =
                                      XPCPerThreadData::IterateThreads(&iterp))) {
                        // Mark those AutoMarkingPtr lists!
                        thread->MarkAutoRootsAfterJSFinalize();

                        XPCCallContext* ccxp = thread->GetCallContext();
                        while (ccxp) {
                            // Deal with the strictness of callcontext that
                            // complains if you ask for a set when
                            // it is in a state where the set could not
                            // possibly be valid.
                            if (ccxp->CanGetSet()) {
                                XPCNativeSet* set = ccxp->GetSet();
                                if (set)
                                    set->Mark();
                            }
                            if (ccxp->CanGetInterface()) {
                                XPCNativeInterface* iface = ccxp->GetInterface();
                                if (iface)
                                    iface->Mark();
                            }
                            ccxp = ccxp->GetPrevCallContext();
                        }
                    }
                }
            }

            // Do the sweeping...

            // We don't want to sweep the JSClasses at shutdown time.
            // At this point there may be JSObjects using them that have
            // been removed from the other maps.
            if (!self->GetXPConnect()->IsShuttingDown()) {
                self->mNativeScriptableSharedMap->
                    Enumerate(JSClassSweeper, nsnull);
            }

            self->mClassInfo2NativeSetMap->
                Enumerate(NativeUnMarkedSetRemover, nsnull);

            self->mNativeSetMap->
                Enumerate(NativeSetSweeper, nsnull);

            self->mIID2NativeInterfaceMap->
                Enumerate(NativeInterfaceSweeper, nsnull);

#ifdef DEBUG
            XPCWrappedNativeScope::ASSERT_NoInterfaceSetsAreMarked();
#endif

#ifdef XPC_REPORT_NATIVE_INTERFACE_AND_SET_FLUSHING
            int setsAfter = (int) self->mNativeSetMap->Count();
            int ifacesAfter = (int) self->mIID2NativeInterfaceMap->Count();

            printf("\n");
            printf("XPCNativeSets:        before: %d  collected: %d  remaining: %d\n",
                   setsBefore, setsBefore - setsAfter, setsAfter);
            printf("XPCNativeInterfaces:  before: %d  collected: %d  remaining: %d\n",
                   ifacesBefore, ifacesBefore - ifacesAfter, ifacesAfter);
            printf("--------------------------------------------------------------\n");
#endif

            // Sweep scopes needing cleanup
            XPCWrappedNativeScope::FinishedFinalizationPhaseOfGC(cx);

            // Now we are going to recycle any unused WrappedNativeTearoffs.
            // We do this by iterating all the live callcontexts (on all
            // threads!) and marking the tearoffs in use. And then we
            // iterate over all the WrappedNative wrappers and sweep their
            // tearoffs.
            //
            // This allows us to perhaps minimize the growth of the
            // tearoffs. And also makes us not hold references to interfaces
            // on our wrapped natives that we are not actually using.
            //
            // XXX We may decide to not do this on *every* gc cycle.

            // Skip this part if XPConnect is shutting down. We get into
            // bad locking problems with the thread iteration otherwise.
            if (!self->GetXPConnect()->IsShuttingDown()) {
                Mutex* threadLock = XPCPerThreadData::GetLock();
                if (threadLock) {
                    // Do the marking...

                    { // scoped lock
                        MutexAutoLock lock(*threadLock);

                        XPCPerThreadData* iterp = nsnull;
                        XPCPerThreadData* thread;

                        while (nsnull != (thread =
                                          XPCPerThreadData::IterateThreads(&iterp))) {
                            XPCCallContext* ccxp = thread->GetCallContext();
                            while (ccxp) {
                                // Deal with the strictness of callcontext that
                                // complains if you ask for a tearoff when
                                // it is in a state where the tearoff could not
                                // possibly be valid.
                                if (ccxp->CanGetTearOff()) {
                                    XPCWrappedNativeTearOff* to =
                                        ccxp->GetTearOff();
                                    if (to)
                                        to->Mark();
                                }
                                ccxp = ccxp->GetPrevCallContext();
                            }
                        }
                    }

                    // Do the sweeping...
                    XPCWrappedNativeScope::SweepAllWrappedNativeTearOffs();
                }
            }

            // Now we need to kill the 'Dying' XPCWrappedNativeProtos.
            // We transfered these native objects to this table when their
            // JSObject's were finalized. We did not destroy them immediately
            // at that point because the ordering of JS finalization is not
            // deterministic and we did not yet know if any wrappers that
            // might still be referencing the protos where still yet to be
            // finalized and destroyed. We *do* know that the protos'
            // JSObjects would not have been finalized if there were any
            // wrappers that referenced the proto but where not themselves
            // slated for finalization in this gc cycle. So... at this point
            // we know that any and all wrappers that might have been
            // referencing the protos in the dying list are themselves dead.
            // So, we can safely delete all the protos in the list.

            self->mDyingWrappedNativeProtoMap->
                Enumerate(DyingProtoKiller, nsnull);


            // mThreadRunningGC indicates that GC is running.
            // Clear it and notify waiters.
            { // scoped lock
                XPCAutoLock lock(self->GetMapLock());
                NS_ASSERTION(self->mThreadRunningGC == PR_GetCurrentThread(), "bad state");
                self->mThreadRunningGC = nsnull;
                xpc_NotifyAll(self->GetMapLock());
            }

            break;
        }
        case JSGC_END:
        {
            // NOTE that this event happens outside of the gc lock in
            // the js engine. So this could be simultaneous with the
            // events above.

            // Do any deferred releases of native objects.
#ifdef XPC_TRACK_DEFERRED_RELEASES
            printf("XPC - Begin deferred Release of %d nsISupports pointers\n",
                   self->mNativesToReleaseArray.Length());
#endif
            DoDeferredRelease(self->mNativesToReleaseArray);
#ifdef XPC_TRACK_DEFERRED_RELEASES
            printf("XPC - End deferred Releases\n");
#endif

            self->GetXPConnect()->ClearGCBeforeCC();
            break;
        }
        default:
            break;
    }

    nsTArray<JSGCCallback> callbacks(self->extraGCCallbacks);
    for (PRUint32 i = 0; i < callbacks.Length(); ++i) {
        if (!callbacks[i](cx, status))
            return false;
    }

    return true;
}

//static
void
XPCJSRuntime::WatchdogMain(void *arg)
{
    XPCJSRuntime* self = static_cast<XPCJSRuntime*>(arg);

    // Lock lasts until we return
    js::AutoLockGC lock(self->mJSRuntime);

    PRIntervalTime sleepInterval;
    while (self->mWatchdogThread) {
        // Sleep only 1 second if recently (or currently) active; otherwise, hibernate
        if (self->mLastActiveTime == -1 || PR_Now() - self->mLastActiveTime <= PRTime(2*PR_USEC_PER_SEC))
            sleepInterval = PR_TicksPerSecond();
        else {
            sleepInterval = PR_INTERVAL_NO_TIMEOUT;
            self->mWatchdogHibernating = true;
        }
#ifdef DEBUG
        PRStatus status =
#endif
            PR_WaitCondVar(self->mWatchdogWakeup, sleepInterval);
        JS_ASSERT(status == PR_SUCCESS);
        js::TriggerOperationCallback(self->mJSRuntime);
    }

    /* Wake up the main thread waiting for the watchdog to terminate. */
    PR_NotifyCondVar(self->mWatchdogWakeup);
}

//static
void
XPCJSRuntime::ActivityCallback(void *arg, JSBool active)
{
    XPCJSRuntime* self = static_cast<XPCJSRuntime*>(arg);
    if (active) {
        self->mLastActiveTime = -1;
        if (self->mWatchdogHibernating) {
            self->mWatchdogHibernating = false;
            PR_NotifyCondVar(self->mWatchdogWakeup);
        }
    } else {
        self->mLastActiveTime = PR_Now();
    }
}

size_t
XPCJSRuntime::SizeOfIncludingThis(nsMallocSizeOfFun mallocSizeOf)
{
    size_t n = 0;
    n += mallocSizeOf(this);
    n += mWrappedJSMap->SizeOfIncludingThis(mallocSizeOf);
    n += mIID2NativeInterfaceMap->SizeOfIncludingThis(mallocSizeOf);
    n += mClassInfo2NativeSetMap->ShallowSizeOfIncludingThis(mallocSizeOf);
    n += mNativeSetMap->SizeOfIncludingThis(mallocSizeOf);

    // NULL for the second arg;  we're not measuring anything hanging off the
    // entries in mJSHolders.
    n += JS_DHashTableSizeOfExcludingThis(&mJSHolders, NULL, mallocSizeOf);

    // There are other XPCJSRuntime members that could be measured; the above
    // ones have been seen by DMD to be worth measuring.  More stuff may be
    // added later.

    return n;
}

/***************************************************************************/

#ifdef XPC_CHECK_WRAPPERS_AT_SHUTDOWN
static JSDHashOperator
DEBUG_WrapperChecker(JSDHashTable *table, JSDHashEntryHdr *hdr,
                     uint32_t number, void *arg)
{
    XPCWrappedNative* wrapper = (XPCWrappedNative*)((JSDHashEntryStub*)hdr)->key;
    NS_ASSERTION(!wrapper->IsValid(), "found a 'valid' wrapper!");
    ++ *((int*)arg);
    return JS_DHASH_NEXT;
}
#endif

static JSDHashOperator
WrappedJSShutdownMarker(JSDHashTable *table, JSDHashEntryHdr *hdr,
                        uint32_t number, void *arg)
{
    JSRuntime* rt = (JSRuntime*) arg;
    nsXPCWrappedJS* wrapper = ((JSObject2WrappedJSMap::Entry*)hdr)->value;
    NS_ASSERTION(wrapper, "found a null JS wrapper!");
    NS_ASSERTION(wrapper->IsValid(), "found an invalid JS wrapper!");
    wrapper->SystemIsBeingShutDown(rt);
    return JS_DHASH_NEXT;
}

static JSDHashOperator
DetachedWrappedNativeProtoShutdownMarker(JSDHashTable *table, JSDHashEntryHdr *hdr,
                                         uint32_t number, void *arg)
{
    XPCWrappedNativeProto* proto =
        (XPCWrappedNativeProto*)((JSDHashEntryStub*)hdr)->key;

    proto->SystemIsBeingShutDown();
    return JS_DHASH_NEXT;
}

void XPCJSRuntime::SystemIsBeingShutDown()
{
    DOM_ClearInterfaces();

    if (mDetachedWrappedNativeProtoMap)
        mDetachedWrappedNativeProtoMap->
            Enumerate(DetachedWrappedNativeProtoShutdownMarker, nsnull);
}

JSContext *
XPCJSRuntime::GetJSCycleCollectionContext()
{
    if (!mJSCycleCollectionContext) {
        mJSCycleCollectionContext = JS_NewContext(mJSRuntime, 0);
        if (!mJSCycleCollectionContext)
            return nsnull;
    }
    return mJSCycleCollectionContext;
}

XPCJSRuntime::~XPCJSRuntime()
{
    if (mWatchdogWakeup) {
        // If the watchdog thread is running, tell it to terminate waking it
        // up if necessary and wait until it signals that it finished. As we
        // must release the lock before calling PR_DestroyCondVar, we use an
        // extra block here.
        {
            js::AutoLockGC lock(mJSRuntime);
            if (mWatchdogThread) {
                mWatchdogThread = nsnull;
                PR_NotifyCondVar(mWatchdogWakeup);
                PR_WaitCondVar(mWatchdogWakeup, PR_INTERVAL_NO_TIMEOUT);
            }
        }
        PR_DestroyCondVar(mWatchdogWakeup);
        mWatchdogWakeup = nsnull;
    }

    if (mJSCycleCollectionContext)
        JS_DestroyContextNoGC(mJSCycleCollectionContext);

#ifdef XPC_DUMP_AT_SHUTDOWN
    {
    // count the total JSContexts in use
    JSContext* iter = nsnull;
    int count = 0;
    while (JS_ContextIterator(mJSRuntime, &iter))
        count ++;
    if (count)
        printf("deleting XPCJSRuntime with %d live JSContexts\n", count);
    }
#endif

    // clean up and destroy maps...
    if (mWrappedJSMap) {
#ifdef XPC_DUMP_AT_SHUTDOWN
        uint32_t count = mWrappedJSMap->Count();
        if (count)
            printf("deleting XPCJSRuntime with %d live wrapped JSObject\n", (int)count);
#endif
        mWrappedJSMap->Enumerate(WrappedJSShutdownMarker, mJSRuntime);
        delete mWrappedJSMap;
    }

    if (mWrappedJSClassMap) {
#ifdef XPC_DUMP_AT_SHUTDOWN
        uint32_t count = mWrappedJSClassMap->Count();
        if (count)
            printf("deleting XPCJSRuntime with %d live nsXPCWrappedJSClass\n", (int)count);
#endif
        delete mWrappedJSClassMap;
    }

    if (mIID2NativeInterfaceMap) {
#ifdef XPC_DUMP_AT_SHUTDOWN
        uint32_t count = mIID2NativeInterfaceMap->Count();
        if (count)
            printf("deleting XPCJSRuntime with %d live XPCNativeInterfaces\n", (int)count);
#endif
        delete mIID2NativeInterfaceMap;
    }

    if (mClassInfo2NativeSetMap) {
#ifdef XPC_DUMP_AT_SHUTDOWN
        uint32_t count = mClassInfo2NativeSetMap->Count();
        if (count)
            printf("deleting XPCJSRuntime with %d live XPCNativeSets\n", (int)count);
#endif
        delete mClassInfo2NativeSetMap;
    }

    if (mNativeSetMap) {
#ifdef XPC_DUMP_AT_SHUTDOWN
        uint32_t count = mNativeSetMap->Count();
        if (count)
            printf("deleting XPCJSRuntime with %d live XPCNativeSets\n", (int)count);
#endif
        delete mNativeSetMap;
    }

    if (mMapLock)
        XPCAutoLock::DestroyLock(mMapLock);

    if (mThisTranslatorMap) {
#ifdef XPC_DUMP_AT_SHUTDOWN
        uint32_t count = mThisTranslatorMap->Count();
        if (count)
            printf("deleting XPCJSRuntime with %d live ThisTranslator\n", (int)count);
#endif
        delete mThisTranslatorMap;
    }

#ifdef XPC_CHECK_WRAPPERS_AT_SHUTDOWN
    if (DEBUG_WrappedNativeHashtable) {
        int LiveWrapperCount = 0;
        JS_DHashTableEnumerate(DEBUG_WrappedNativeHashtable,
                               DEBUG_WrapperChecker, &LiveWrapperCount);
        if (LiveWrapperCount)
            printf("deleting XPCJSRuntime with %d live XPCWrappedNative (found in wrapper check)\n", (int)LiveWrapperCount);
        JS_DHashTableDestroy(DEBUG_WrappedNativeHashtable);
    }
#endif

    if (mNativeScriptableSharedMap) {
#ifdef XPC_DUMP_AT_SHUTDOWN
        uint32_t count = mNativeScriptableSharedMap->Count();
        if (count)
            printf("deleting XPCJSRuntime with %d live XPCNativeScriptableShared\n", (int)count);
#endif
        delete mNativeScriptableSharedMap;
    }

    if (mDyingWrappedNativeProtoMap) {
#ifdef XPC_DUMP_AT_SHUTDOWN
        uint32_t count = mDyingWrappedNativeProtoMap->Count();
        if (count)
            printf("deleting XPCJSRuntime with %d live but dying XPCWrappedNativeProto\n", (int)count);
#endif
        delete mDyingWrappedNativeProtoMap;
    }

    if (mDetachedWrappedNativeProtoMap) {
#ifdef XPC_DUMP_AT_SHUTDOWN
        uint32_t count = mDetachedWrappedNativeProtoMap->Count();
        if (count)
            printf("deleting XPCJSRuntime with %d live detached XPCWrappedNativeProto\n", (int)count);
#endif
        delete mDetachedWrappedNativeProtoMap;
    }

    if (mExplicitNativeWrapperMap) {
#ifdef XPC_DUMP_AT_SHUTDOWN
        uint32_t count = mExplicitNativeWrapperMap->Count();
        if (count)
            printf("deleting XPCJSRuntime with %d live explicit XPCNativeWrapper\n", (int)count);
#endif
        delete mExplicitNativeWrapperMap;
    }

    if (mJSHolders.ops) {
        JS_DHashTableFinish(&mJSHolders);
        mJSHolders.ops = nsnull;
    }

    if (mJSRuntime) {
        JS_DestroyRuntime(mJSRuntime);
        JS_ShutDown();
#ifdef DEBUG_shaver_off
        fprintf(stderr, "nJRSI: destroyed runtime %p\n", (void *)mJSRuntime);
#endif
    }

    XPCPerThreadData::ShutDown();
}

static void*
GetCompartmentNameHelper(JSCompartment *c, bool getAddress)
{
    nsCString* name = new nsCString();
    if (js::IsAtomsCompartment(c)) {
        name->AssignLiteral("atoms");
    } else if (JSPrincipals *principals = JS_GetCompartmentPrincipals(c)) {
        if (principals->codebase) {
            name->Assign(principals->codebase);

            // For system compartments we append the location, if there is one.
            // And we append the address if |getAddress| is true, so that
            // multiple system compartments (and there can be many) can be
            // distinguished.
            if (js::IsSystemCompartment(c)) {
                xpc::CompartmentPrivate *compartmentPrivate =
                    static_cast<xpc::CompartmentPrivate*>(JS_GetCompartmentPrivate(c));
                if (compartmentPrivate &&
                    !compartmentPrivate->location.IsEmpty()) {
                    name->AppendLiteral(", ");
                    name->Append(compartmentPrivate->location);
                }

                if (getAddress) {
                    // ample; 64-bit address max is 18 chars
                    static const int maxLength = 31;
                    nsPrintfCString address(maxLength, ", 0x%llx", PRUint64(c));
                    name->Append(address);
                }
            }

            // A hack: replace forward slashes with '\\' so they aren't
            // treated as path separators.  Users of the reporters
            // (such as about:memory) have to undo this change.
            name->ReplaceChar('/', '\\');
        } else {
            name->AssignLiteral("null-codebase");
        }
    } else {
        name->AssignLiteral("null-principal");
    }
    return name;
}

static void*
GetCompartmentNameAndAddress(JSRuntime *rt, JSCompartment *c)
{
    return GetCompartmentNameHelper(c, /* get address = */true);
}

namespace xpc {

void*
GetCompartmentName(JSRuntime *rt, JSCompartment *c)
{
    return GetCompartmentNameHelper(c, /* get address = */false);
}

void
DestroyCompartmentName(void *string)
{
    delete static_cast<nsCString*>(string);
}

} // namespace xpc

namespace {

template <int N>
inline void
ReportMemory(const nsACString &path, PRInt32 kind, PRInt32 units,
             PRInt64 amount, const char (&desc)[N],
             nsIMemoryMultiReporterCallback *callback, nsISupports *closure)
{
    callback->Callback(NS_LITERAL_CSTRING(""), path, kind, units, amount,
                       NS_LITERAL_CSTRING(desc), closure);
}

template <int N>
inline void
ReportMemoryBytes(const nsACString &path, PRInt32 kind, PRInt64 amount,
                  const char (&desc)[N],
                  nsIMemoryMultiReporterCallback *callback,
                  nsISupports *closure)
{
    ReportMemory(path, kind, nsIMemoryReporter::UNITS_BYTES, amount, desc,
                 callback, closure);
}

template <int N>
inline void
ReportMemoryBytes0(const nsCString &path, PRInt32 kind, PRInt64 amount,
                   const char (&desc)[N],
                   nsIMemoryMultiReporterCallback *callback,
                   nsISupports *closure)
{
    if (amount)
        ReportMemoryBytes(path, kind, amount, desc, callback, closure);
}

template <int N>
inline void
ReportGCHeapBytes(const nsACString &path, PRInt64 *total, PRInt64 amount,
                  const char (&desc)[N],
                  nsIMemoryMultiReporterCallback *callback,
                  nsISupports *closure)
{
    ReportMemory(path, nsIMemoryReporter::KIND_NONHEAP, nsIMemoryReporter::UNITS_BYTES, amount,
                 desc, callback, closure);
    *total += amount;
}

template <int N>
inline void
ReportGCHeapBytes0(const nsCString &path, PRInt64 *total, PRInt64 amount,
                   const char (&desc)[N],
                   nsIMemoryMultiReporterCallback *callback,
                   nsISupports *closure)
{
    if (amount) 
        return ReportGCHeapBytes(path, total, amount, desc, callback, closure);
}

template <int N>
inline void
ReportMemoryPercentage(const nsACString &path, PRInt32 kind, PRInt64 amount,
                       const char (&desc)[N],
                       nsIMemoryMultiReporterCallback *callback,
                       nsISupports *closure)
{
    ReportMemory(path, kind, nsIMemoryReporter::UNITS_PERCENTAGE, amount, desc,
                 callback, closure);
}

template <int N>
inline const nsCString
MakeMemoryReporterPath(const nsACString &pathPrefix,
                       const JS::CompartmentStats &cStats,
                       const char (&reporterName)[N])
{
  return pathPrefix + NS_LITERAL_CSTRING("compartment(") +
         *static_cast<nsCString*>(cStats.name) +
         NS_LITERAL_CSTRING(")/") + nsDependentCString(reporterName);
}

} // anonymous namespace

// We have per-compartment GC heap totals, so we can't put the total GC heap
// size in the explicit allocations tree.  But it's a useful figure, so put it
// in the "others" list.

static PRInt64
GetGCChunkTotalBytes()
{
    JSRuntime *rt = nsXPConnect::GetRuntimeInstance()->GetJSRuntime();
    return PRInt64(JS_GetGCParameter(rt, JSGC_TOTAL_CHUNKS)) * js::gc::ChunkSize;
}

NS_MEMORY_REPORTER_IMPLEMENT(XPConnectJSGCHeap,
                             "js-gc-heap",
                             KIND_OTHER,
                             nsIMemoryReporter::UNITS_BYTES,
                             GetGCChunkTotalBytes,
                             "Memory used by the garbage-collected JavaScript heap.")

static PRInt64
GetJSSystemCompartmentCount()
{
    return JS::SystemCompartmentCount(nsXPConnect::GetRuntimeInstance()->GetJSRuntime());
}

static PRInt64
GetJSUserCompartmentCount()
{
    return JS::UserCompartmentCount(nsXPConnect::GetRuntimeInstance()->GetJSRuntime());
}

// Nb: js-system-compartment-count + js-user-compartment-count could be
// different to the number of compartments reported by
// JSMemoryMultiReporter if a garbage collection occurred
// between them being consulted.  We could move these reporters into
// XPConnectJSCompartmentCount to avoid that problem, but then we couldn't
// easily report them via telemetry, so we live with the small risk of
// inconsistencies.
NS_MEMORY_REPORTER_IMPLEMENT(XPConnectJSSystemCompartmentCount,
                             "js-compartments-system",
                             KIND_OTHER,
                             nsIMemoryReporter::UNITS_COUNT,
                             GetJSSystemCompartmentCount,
                             "The number of JavaScript compartments for system code.  The sum of this "
                             "and 'js-compartments-user' might not match the number of "
                             "compartments listed under 'js' if a garbage collection occurs at an "
                             "inopportune time, but such cases should be rare.")

NS_MEMORY_REPORTER_IMPLEMENT(XPConnectJSUserCompartmentCount,
                             "js-compartments-user",
                             KIND_OTHER,
                             nsIMemoryReporter::UNITS_COUNT,
                             GetJSUserCompartmentCount,
                             "The number of JavaScript compartments for user code.  The sum of this "
                             "and 'js-compartments-system' might not match the number of "
                             "compartments listed under 'js' if a garbage collection occurs at an "
                             "inopportune time, but such cases should be rare.")

namespace mozilla {
namespace xpconnect {
namespace memory {

static PRInt64
ReportCompartmentStats(const JS::CompartmentStats &cStats,
                       const nsACString &pathPrefix,
                       nsIMemoryMultiReporterCallback *callback,
                       nsISupports *closure)
{
    PRInt64 gcTotal = 0;

    ReportGCHeapBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "gc-heap/arena/headers"),
                       &gcTotal, cStats.gcHeapArenaHeaders,
                       "Memory on the compartment's garbage-collected JavaScript heap, within "
                       "arenas, that is used to hold internal book-keeping information.",
                       callback, closure);

    ReportGCHeapBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "gc-heap/arena/padding"),
                       &gcTotal, cStats.gcHeapArenaPadding,
                       "Memory on the compartment's garbage-collected JavaScript heap, within "
                       "arenas, that is unused and present only so that other data is aligned. "
                       "This constitutes internal fragmentation.",
                       callback, closure);

    ReportGCHeapBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "gc-heap/arena/unused"),
                       &gcTotal, cStats.gcHeapArenaUnused,
                       "Memory on the compartment's garbage-collected JavaScript heap, within "
                       "arenas, that could be holding useful data but currently isn't.",
                       callback, closure);

    ReportGCHeapBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "gc-heap/objects/non-function"),
                       &gcTotal, cStats.gcHeapObjectsNonFunction,
                       "Memory on the compartment's garbage-collected JavaScript heap that holds "
                       "non-function objects.",
                       callback, closure);

    ReportGCHeapBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "gc-heap/objects/function"),
                       &gcTotal, cStats.gcHeapObjectsFunction,
                       "Memory on the compartment's garbage-collected JavaScript heap that holds "
                       "function objects.",
                       callback, closure);

    ReportGCHeapBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "gc-heap/strings"),
                       &gcTotal, cStats.gcHeapStrings,
                       "Memory on the compartment's garbage-collected JavaScript heap that holds "
                       "string headers.  String headers contain various pieces of information "
                       "about a string, but do not contain (except in the case of very short "
                       "strings) the string characters;  characters in longer strings are counted "
                       "under 'gc-heap/string-chars' instead.",
                       callback, closure);

    ReportGCHeapBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "gc-heap/scripts"),
                       &gcTotal, cStats.gcHeapScripts,
                       "Memory on the compartment's garbage-collected JavaScript heap that holds "
                       "JSScript instances. A JSScript is created for each user-defined function "
                       "in a script. One is also created for the top-level code in a script.",
                       callback, closure);

    ReportGCHeapBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "gc-heap/shapes/tree"),
                       &gcTotal, cStats.gcHeapShapesTree,
                       "Memory on the compartment's garbage-collected JavaScript heap that holds "
                       "shapes that are in a property tree.",
                       callback, closure);

    ReportGCHeapBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "gc-heap/shapes/dict"),
                       &gcTotal, cStats.gcHeapShapesDict,
                       "Memory on the compartment's garbage-collected JavaScript heap that holds "
                       "shapes that are in dictionary mode.",
                       callback, closure);

    ReportGCHeapBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "gc-heap/shapes/base"),
                       &gcTotal, cStats.gcHeapShapesBase,
                       "Memory on the compartment's garbage-collected JavaScript heap that collates "
                       "data common to many shapes.",
                       callback, closure);

    ReportGCHeapBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "gc-heap/type-objects"),
                       &gcTotal, cStats.gcHeapTypeObjects,
                       "Memory on the compartment's garbage-collected JavaScript heap that holds "
                       "type inference information.",
                       callback, closure);

    ReportGCHeapBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "gc-heap/xml"),
                       &gcTotal, cStats.gcHeapXML,
                       "Memory on the compartment's garbage-collected JavaScript heap that holds "
                       "E4X XML objects.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "objects/slots"),
                       nsIMemoryReporter::KIND_HEAP, cStats.objectSlots,
                       "Memory allocated for the compartment's non-fixed object slot arrays, "
                       "which are used to represent object properties.  Some objects also "
                       "contain a fixed number of slots which are stored on the compartment's "
                       "JavaScript heap; those slots are not counted here, but in "
                       "'gc-heap/objects' instead.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "objects/elements"),
                       nsIMemoryReporter::KIND_HEAP, cStats.objectElements,
                       "Memory allocated for the compartment's object element arrays, "
                       "which are used to represent indexed object properties.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "objects/misc"),
                       nsIMemoryReporter::KIND_HEAP, cStats.objectMisc,
                       "Memory allocated for various small, miscellaneous "
                       "structures that hang off certain kinds of objects.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "string-chars"),
                       nsIMemoryReporter::KIND_HEAP, cStats.stringChars,
                       "Memory allocated to hold the compartment's string characters.  Sometimes "
                       "more memory is allocated than necessary, to simplify string "
                       "concatenation.  Each string also includes a header which is stored on the "
                       "compartment's JavaScript heap;  that header is not counted here, but in "
                       "'gc-heap/strings' instead.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "shapes-extra/tree-tables"),
                       nsIMemoryReporter::KIND_HEAP, cStats.shapesExtraTreeTables,
                       "Memory allocated for the compartment's property tables that belong to "
                       "shapes that are in a property tree.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "shapes-extra/dict-tables"),
                       nsIMemoryReporter::KIND_HEAP, cStats.shapesExtraDictTables,
                       "Memory allocated for the compartment's property tables that belong to "
                       "shapes that are in dictionary mode.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "shapes-extra/tree-shape-kids"),
                       nsIMemoryReporter::KIND_HEAP, cStats.shapesExtraTreeShapeKids,
                       "Memory allocated for the compartment's kid hashes that belong to shapes "
                       "that are in a property tree.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "shapes-extra/compartment-tables"),
                       nsIMemoryReporter::KIND_HEAP, cStats.shapesCompartmentTables,
                       "Memory used by compartment wide tables storing shape information "
                       "for use during object construction.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "script-data"),
                       nsIMemoryReporter::KIND_HEAP, cStats.scriptData,
                       "Memory allocated for JSScript bytecode and various variable-length "
                       "tables.",
                       callback, closure);

#ifdef JS_METHODJIT
    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "mjit/code"),
                       nsIMemoryReporter::KIND_NONHEAP, cStats.mjitCode,
                       "Memory used by the method JIT to hold the compartment's generated code.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "mjit/data"),
                       nsIMemoryReporter::KIND_HEAP, cStats.mjitData,
                       "Memory used by the method JIT for the compartment's compilation data: "
                       "JITScripts, native maps, and inline cache structs.",
                       callback, closure);
#endif

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "type-inference/script-main"),
                       nsIMemoryReporter::KIND_HEAP,
                       cStats.typeInferenceSizes.scripts,
                       "Memory used during type inference to store type sets of variables "
                       "and dynamically observed types.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "type-inference/object-main"),
                       nsIMemoryReporter::KIND_HEAP,
                       cStats.typeInferenceSizes.objects,
                       "Memory used during type inference to store types and possible "
                       "property types of JS objects.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "type-inference/tables"),
                       nsIMemoryReporter::KIND_HEAP,
                       cStats.typeInferenceSizes.tables,
                       "Memory used during type inference for compartment-wide tables.",
                       callback, closure);

    ReportMemoryBytes0(MakeMemoryReporterPath(pathPrefix, cStats,
                                              "analysis-temporary"),
                       nsIMemoryReporter::KIND_HEAP,
                       cStats.typeInferenceSizes.temporary,
                       "Memory used during type inference and compilation to hold transient "
                       "analysis information.  Cleared on GC.",
                       callback, closure);

    return gcTotal;
}

void
ReportJSRuntimeExplicitTreeStats(const JS::RuntimeStats &rtStats, const nsACString &pathPrefix,
                                 nsIMemoryMultiReporterCallback *callback,
                                 nsISupports *closure)
{
    PRInt64 gcTotal = 0;
    for (size_t index = 0;
         index < rtStats.compartmentStatsVector.length();
         index++) {
        gcTotal += ReportCompartmentStats(rtStats.compartmentStatsVector[index], pathPrefix,
                                    callback, closure);
    }

    ReportMemoryBytes(pathPrefix + NS_LITERAL_CSTRING("runtime/runtime-object"),
                      nsIMemoryReporter::KIND_HEAP, rtStats.runtimeObject,
                      "Memory used by the JSRuntime object.",
                      callback, closure);

    ReportMemoryBytes(pathPrefix + NS_LITERAL_CSTRING("runtime/atoms-table"),
                      nsIMemoryReporter::KIND_HEAP, rtStats.runtimeAtomsTable,
                      "Memory used by the atoms table.",
                      callback, closure);

    ReportMemoryBytes(pathPrefix + NS_LITERAL_CSTRING("runtime/contexts"),
                      nsIMemoryReporter::KIND_HEAP, rtStats.runtimeContexts,
                      "Memory used by JSContext objects and certain structures "
                      "hanging off them." ,
                      callback, closure);

    ReportMemoryBytes(pathPrefix + NS_LITERAL_CSTRING("runtime/normal"),
                      nsIMemoryReporter::KIND_HEAP, rtStats.runtimeNormal,
                      "Memory used by a JSRuntime, "
                      "excluding memory that is reported by "
                      "other reporters under 'explicit/js/runtime/'.",
                      callback, closure);

    ReportMemoryBytes(pathPrefix + NS_LITERAL_CSTRING("runtime/temporary"),
                      nsIMemoryReporter::KIND_HEAP, rtStats.runtimeTemporary,
                      "Memory held transiently in JSRuntime and used during "
                      "compilation.  It mostly holds parse nodes.",
                      callback, closure);

    ReportMemoryBytes0(pathPrefix + NS_LITERAL_CSTRING("runtime/regexp-code"),
                       nsIMemoryReporter::KIND_NONHEAP, rtStats.runtimeRegexpCode,
                       "Memory used by the regexp JIT to hold generated code.",
                       callback, closure);

    ReportMemoryBytes(pathPrefix + NS_LITERAL_CSTRING("runtime/stack-committed"),
                      nsIMemoryReporter::KIND_NONHEAP, rtStats.runtimeStackCommitted,
                      "Memory used for the JS call stack.  This is the committed portion "
                      "of the stack; the uncommitted portion is not measured because it "
                      "hardly costs anything.",
                      callback, closure);

    ReportMemoryBytes(pathPrefix + NS_LITERAL_CSTRING("runtime/gc-marker"),
                      nsIMemoryReporter::KIND_NONHEAP, rtStats.runtimeGCMarker,
                      "Memory used for the GC mark stack and gray roots.",
                      callback, closure);

    ReportGCHeapBytes(pathPrefix +
                      NS_LITERAL_CSTRING("gc-heap-chunk-dirty-unused"),
                      &gcTotal, rtStats.gcHeapChunkDirtyUnused,
                      "Memory on the garbage-collected JavaScript heap, within chunks with at "
                      "least one allocated GC thing, that could be holding useful data but "
                      "currently isn't.  Memory here is mutually exclusive with memory reported"
                      "under 'explicit/js/gc-heap-decommitted'.",
                      callback, closure);

    ReportGCHeapBytes(pathPrefix +
                      NS_LITERAL_CSTRING("gc-heap-chunk-clean-unused"),
                      &gcTotal, rtStats.gcHeapChunkCleanUnused,
                      "Memory on the garbage-collected JavaScript heap taken by completely empty "
                      "chunks, that soon will be released unless claimed for new allocations.  "
                      "Memory here is mutually exclusive with memory reported under "
                      "'explicit/js/gc-heap-decommitted'.",
                      callback, closure);

    ReportGCHeapBytes(pathPrefix +
                      NS_LITERAL_CSTRING("gc-heap-decommitted"),
                      &gcTotal,
                      rtStats.gcHeapChunkCleanDecommitted + rtStats.gcHeapChunkDirtyDecommitted,
                      "Memory in the address space of the garbage-collected JavaScript heap that "
                      "is currently returned to the OS.",
                      callback, closure);

    ReportGCHeapBytes(pathPrefix +
                      NS_LITERAL_CSTRING("gc-heap-chunk-admin"),
                      &gcTotal, rtStats.gcHeapChunkAdmin,
                      "Memory on the garbage-collected JavaScript heap, within chunks, that is "
                      "used to hold internal book-keeping information.",
                      callback, closure);

    // gcTotal is the sum of everything we've reported for the GC heap.  It
    // should equal rtStats.gcHeapChunkTotal.
    JS_ASSERT(size_t(gcTotal) == rtStats.gcHeapChunkTotal);
}

} // namespace memory
} // namespace xpconnect
} // namespace mozilla

NS_MEMORY_REPORTER_MALLOC_SIZEOF_FUN(JsMallocSizeOf, "js")

class JSCompartmentsMultiReporter : public nsIMemoryMultiReporter
{
  public:
    NS_DECL_ISUPPORTS

    NS_IMETHOD GetName(nsACString &name)
    {
        name.AssignLiteral("compartments");
        return NS_OK;
    }

    typedef js::Vector<nsCString, 0, js::SystemAllocPolicy> Paths; 

    static void CompartmentCallback(JSRuntime *rt, void* data, JSCompartment *c)
    {
        Paths *paths = static_cast<Paths *>(data);
        nsCString *name =
            static_cast<nsCString *>(xpc::GetCompartmentName(rt, c));
        nsCString path;
        if (js::IsSystemCompartment(c))
            path = NS_LITERAL_CSTRING("compartments/system/") + *name;
        else
            path = NS_LITERAL_CSTRING("compartments/user/") + *name;
        if (!paths->append(path))
            return;     // silent failure, but it's very unlikely

        xpc::DestroyCompartmentName(name);
    }

    NS_IMETHOD CollectReports(nsIMemoryMultiReporterCallback *callback,
                              nsISupports *closure)
    {
        // First we collect the compartment paths.  Then we report them.  Doing
        // the two steps interleaved is a bad idea, because calling |callback|
        // from within CompartmentCallback() leads to all manner of assertions.

        // Collect.
 
        Paths paths; 
        JS_IterateCompartments(nsXPConnect::GetRuntimeInstance()->GetJSRuntime(),
                               &paths, CompartmentCallback);
 
        // Report.
        for (size_t i = 0; i < paths.length(); i++)
            // These ones don't need a description, hence the "".
            ReportMemory(paths[i],
                         nsIMemoryReporter::KIND_OTHER,
                         nsIMemoryReporter::UNITS_COUNT,
                         1, "", callback, closure);

        return NS_OK;
    }

    NS_IMETHOD
    GetExplicitNonHeap(PRInt64 *n)
    {
        // This reporter does neither "explicit" nor NONHEAP measurements.
        *n = 0;
        return NS_OK;
    }
};

NS_IMPL_THREADSAFE_ISUPPORTS1(JSCompartmentsMultiReporter
                              , nsIMemoryMultiReporter
                              )

class JSMemoryMultiReporter : public nsIMemoryMultiReporter
{
public:
    NS_DECL_ISUPPORTS

    NS_IMETHOD GetName(nsACString &name)
    {
        name.AssignLiteral("js");
        return NS_OK;
    }

    NS_IMETHOD CollectReports(nsIMemoryMultiReporterCallback *callback,
                              nsISupports *closure)
    {
        XPCJSRuntime *xpcrt = nsXPConnect::GetRuntimeInstance();

        // In the first step we get all the stats and stash them in a local
        // data structure.  In the second step we pass all the stashed stats to
        // the callback.  Separating these steps is important because the
        // callback may be a JS function, and executing JS while getting these
        // stats seems like a bad idea.
        JS::RuntimeStats rtStats(JsMallocSizeOf, GetCompartmentNameAndAddress,
                                 xpc::DestroyCompartmentName);
        if (!JS::CollectRuntimeStats(xpcrt->GetJSRuntime(), &rtStats))
            return NS_ERROR_FAILURE;

        size_t xpconnect =
            xpcrt->SizeOfIncludingThis(JsMallocSizeOf) +
            XPCWrappedNativeScope::SizeOfAllScopesIncludingThis(JsMallocSizeOf);

        NS_NAMED_LITERAL_CSTRING(pathPrefix, "explicit/js/");

        // This is the second step (see above).  First we report stuff in the
        // "explicit" tree, then we report other stuff.

        ReportJSRuntimeExplicitTreeStats(rtStats, pathPrefix, callback, closure);

        ReportMemoryBytes(pathPrefix + NS_LITERAL_CSTRING("xpconnect"),
                          nsIMemoryReporter::KIND_HEAP, xpconnect,
                          "Memory used by XPConnect.",
                          callback, closure);

        ReportMemoryBytes(NS_LITERAL_CSTRING("js-main-runtime-gc-heap-chunk-dirty-unused"),
                          nsIMemoryReporter::KIND_OTHER,
                          rtStats.gcHeapChunkDirtyUnused,
                          "The same as 'explicit/js/gc-heap-chunk-dirty-unused'.  Shown here for "
                          "easy comparison with other 'js-gc' reporters.",
                          callback, closure);

        ReportMemoryBytes(NS_LITERAL_CSTRING("js-main-runtime-gc-heap-chunk-clean-unused"),
                          nsIMemoryReporter::KIND_OTHER,
                          rtStats.gcHeapChunkCleanUnused,
                          "The same as 'explicit/js/gc-heap-chunk-clean-unused'.  Shown here for "
                          "easy comparison with other 'js-gc' reporters.",
                          callback, closure);

        ReportMemoryBytes(NS_LITERAL_CSTRING("js-main-runtime-gc-heap-decommitted"),
                          nsIMemoryReporter::KIND_OTHER,
                          rtStats.gcHeapChunkCleanDecommitted + rtStats.gcHeapChunkDirtyDecommitted,
                          "The same as 'explicit/js/gc-heap-decommitted'.  Shown here for "
                          "easy comparison with other 'js-gc' reporters.",
                          callback, closure);

        ReportMemoryBytes(NS_LITERAL_CSTRING("js-main-runtime-gc-heap-arena-unused"),
                          nsIMemoryReporter::KIND_OTHER,
                          rtStats.gcHeapArenaUnused,
                          "Memory on the main JSRuntime's garbage-collected JavaScript heap, "
                          "within arenas, that could be holding useful data but currently isn't. "
                          "This is the sum of all compartments' 'gc-heap/arena-unused' numbers.",
                          callback, closure);

        ReportMemoryPercentage(NS_LITERAL_CSTRING("js-main-runtime-gc-heap-unused-fraction"),
                               nsIMemoryReporter::KIND_OTHER,
                               rtStats.gcHeapUnusedPercentage,
                               "Fraction of the main JSRuntime's garbage-collected JavaScript "
                               "heap that is unused. Computed as ('js-gc-heap-chunk-clean-unused' "
                               "+ 'js-gc-heap-chunk-dirty-unused' + 'js-gc-heap-decommitted' + "
                               "'js-gc-heap-arena-unused') / 'js-gc-heap'.",
                               callback, closure);

        ReportMemoryBytes(NS_LITERAL_CSTRING("js-main-runtime-objects"),
                          nsIMemoryReporter::KIND_OTHER, rtStats.totalObjects,
                          "Memory used for all object-related data in the main JSRuntime. "
                          "This is the sum of all compartments' 'gc-heap/objects-non-function', "
                          "'gc-heap/objects-function' and 'object-slots' numbers.",
                          callback, closure);

        ReportMemoryBytes(NS_LITERAL_CSTRING("js-main-runtime-shapes"),
                          nsIMemoryReporter::KIND_OTHER, rtStats.totalShapes,
                          "Memory used for all shape-related data in the main JSRuntime. "
                          "This is the sum of all compartments' 'gc-heap/shapes/tree', "
                          "'gc-heap/shapes/dict', 'gc-heap/shapes/base', "
                          "'shapes-extra/tree-tables', 'shapes-extra/dict-tables', "
                          "'shapes-extra/tree-shape-kids' and 'shapes-extra/empty-shape-arrays'.",
                          callback, closure);

        ReportMemoryBytes(NS_LITERAL_CSTRING("js-main-runtime-scripts"),
                          nsIMemoryReporter::KIND_OTHER, rtStats.totalScripts,
                          "Memory used for all script-related data in the main JSRuntime. "
                          "This is the sum of all compartments' 'gc-heap/scripts' and "
                          "'script-data' numbers.",
                          callback, closure);

        ReportMemoryBytes(NS_LITERAL_CSTRING("js-main-runtime-strings"),
                          nsIMemoryReporter::KIND_OTHER, rtStats.totalStrings,
                          "Memory used for all string-related data in the main JSRuntime. "
                          "This is the sum of all compartments' 'gc-heap/strings' and "
                          "'string-chars' numbers.",
                          callback, closure);
#ifdef JS_METHODJIT
        ReportMemoryBytes(NS_LITERAL_CSTRING("js-main-runtime-mjit"),
                          nsIMemoryReporter::KIND_OTHER, rtStats.totalMjit,
                          "Memory used by the method JIT in the main JSRuntime.  "
                          "This is the sum of all compartments' 'mjit/code', and 'mjit/data' "
                          "numbers.",
                          callback, closure);
#endif
        ReportMemoryBytes(NS_LITERAL_CSTRING("js-main-runtime-type-inference"),
                          nsIMemoryReporter::KIND_OTHER, rtStats.totalTypeInference,
                          "Non-transient memory used by type inference in the main JSRuntime. "
                          "This is the sum of all compartments' 'gc-heap/type-objects', "
                          "'type-inference/script-main', 'type-inference/object-main' and "
                          "'type-inference/tables' numbers.",
                          callback, closure);

        ReportMemoryBytes(NS_LITERAL_CSTRING("js-main-runtime-analysis-temporary"),
                          nsIMemoryReporter::KIND_OTHER, rtStats.totalAnalysisTemp,
                          "Memory used transiently during type inference and compilation in the "
                          "main JSRuntime. This is the sum of all compartments' "
                          "'analysis-temporary' numbers.",
                          callback, closure);

        return NS_OK;
    }

    NS_IMETHOD
    GetExplicitNonHeap(PRInt64 *n)
    {
        JSRuntime *rt = nsXPConnect::GetRuntimeInstance()->GetJSRuntime();
        *reinterpret_cast<int64_t*>(n) = JS::GetExplicitNonHeapForRuntime(rt, JsMallocSizeOf);
        return NS_OK;
    }
};

NS_IMPL_THREADSAFE_ISUPPORTS1(JSMemoryMultiReporter
                              , nsIMemoryMultiReporter
                              )

#ifdef MOZ_CRASHREPORTER
static JSBool
DiagnosticMemoryCallback(void *ptr, size_t size)
{
    return CrashReporter::RegisterAppMemory(ptr, size) == NS_OK;
}
#endif

static void
AccumulateTelemetryCallback(int id, uint32_t sample)
{
    switch (id) {
      case JS_TELEMETRY_GC_REASON:
        Telemetry::Accumulate(Telemetry::GC_REASON, sample);
        break;
      case JS_TELEMETRY_GC_IS_COMPARTMENTAL:
        Telemetry::Accumulate(Telemetry::GC_IS_COMPARTMENTAL, sample);
        break;
      case JS_TELEMETRY_GC_MS:
        Telemetry::Accumulate(Telemetry::GC_MS, sample);
        break;
      case JS_TELEMETRY_GC_MARK_MS:
        Telemetry::Accumulate(Telemetry::GC_MARK_MS, sample);
        break;
      case JS_TELEMETRY_GC_SWEEP_MS:
        Telemetry::Accumulate(Telemetry::GC_SWEEP_MS, sample);
        break;
      case JS_TELEMETRY_GC_SLICE_MS:
        Telemetry::Accumulate(Telemetry::GC_SLICE_MS, sample);
        break;
      case JS_TELEMETRY_GC_MMU_50:
        Telemetry::Accumulate(Telemetry::GC_MMU_50, sample);
        break;
      case JS_TELEMETRY_GC_RESET:
        Telemetry::Accumulate(Telemetry::GC_RESET, sample);
        break;
      case JS_TELEMETRY_GC_INCREMENTAL_DISABLED:
        Telemetry::Accumulate(Telemetry::GC_INCREMENTAL_DISABLED, sample);
        break;
      case JS_TELEMETRY_GC_NON_INCREMENTAL:
        Telemetry::Accumulate(Telemetry::GC_NON_INCREMENTAL, sample);
        break;
    }
}

bool XPCJSRuntime::gNewDOMBindingsEnabled;

bool PreserveWrapper(JSContext *cx, JSObject *obj)
{
    JS_ASSERT(IS_WRAPPER_CLASS(js::GetObjectClass(obj)));
    nsISupports *native = nsXPConnect::GetXPConnect()->GetNativeOfWrapper(cx, obj);
    if (!native)
        return false;
    nsresult rv;
    nsCOMPtr<nsINode> node = do_QueryInterface(native, &rv);
    if (NS_FAILED(rv))
        return false;
    nsContentUtils::PreserveWrapper(native, node);
    return true;
}

XPCJSRuntime::XPCJSRuntime(nsXPConnect* aXPConnect)
 : mXPConnect(aXPConnect),
   mJSRuntime(nsnull),
   mJSCycleCollectionContext(nsnull),
   mWrappedJSMap(JSObject2WrappedJSMap::newMap(XPC_JS_MAP_SIZE)),
   mWrappedJSClassMap(IID2WrappedJSClassMap::newMap(XPC_JS_CLASS_MAP_SIZE)),
   mIID2NativeInterfaceMap(IID2NativeInterfaceMap::newMap(XPC_NATIVE_INTERFACE_MAP_SIZE)),
   mClassInfo2NativeSetMap(ClassInfo2NativeSetMap::newMap(XPC_NATIVE_SET_MAP_SIZE)),
   mNativeSetMap(NativeSetMap::newMap(XPC_NATIVE_SET_MAP_SIZE)),
   mThisTranslatorMap(IID2ThisTranslatorMap::newMap(XPC_THIS_TRANSLATOR_MAP_SIZE)),
   mNativeScriptableSharedMap(XPCNativeScriptableSharedMap::newMap(XPC_NATIVE_JSCLASS_MAP_SIZE)),
   mDyingWrappedNativeProtoMap(XPCWrappedNativeProtoMap::newMap(XPC_DYING_NATIVE_PROTO_MAP_SIZE)),
   mDetachedWrappedNativeProtoMap(XPCWrappedNativeProtoMap::newMap(XPC_DETACHED_NATIVE_PROTO_MAP_SIZE)),
   mExplicitNativeWrapperMap(XPCNativeWrapperMap::newMap(XPC_NATIVE_WRAPPER_MAP_SIZE)),
   mMapLock(XPCAutoLock::NewLock("XPCJSRuntime::mMapLock")),
   mThreadRunningGC(nsnull),
   mWrappedJSToReleaseArray(),
   mNativesToReleaseArray(),
   mDoingFinalization(false),
   mVariantRoots(nsnull),
   mWrappedJSRoots(nsnull),
   mObjectHolderRoots(nsnull),
   mWatchdogWakeup(nsnull),
   mWatchdogThread(nsnull),
   mWatchdogHibernating(false),
   mLastActiveTime(-1)
{
#ifdef XPC_CHECK_WRAPPERS_AT_SHUTDOWN
    DEBUG_WrappedNativeHashtable =
        JS_NewDHashTable(JS_DHashGetStubOps(), nsnull,
                         sizeof(JSDHashEntryStub), 128);
#endif
    NS_TIME_FUNCTION;

    DOM_InitInterfaces();
    Preferences::AddBoolVarCache(&gNewDOMBindingsEnabled, "dom.new_bindings",
                                 false);


    // these jsids filled in later when we have a JSContext to work with.
    mStrIDs[0] = JSID_VOID;

    mJSRuntime = JS_NewRuntime(32L * 1024L * 1024L); // pref ?
    if (!mJSRuntime)
        NS_RUNTIMEABORT("JS_NewRuntime failed.");

    {
        // Unconstrain the runtime's threshold on nominal heap size, to avoid
        // triggering GC too often if operating continuously near an arbitrary
        // finite threshold (0xffffffff is infinity for uint32_t parameters).
        // This leaves the maximum-JS_malloc-bytes threshold still in effect
        // to cause period, and we hope hygienic, last-ditch GCs from within
        // the GC's allocator.
        JS_SetGCParameter(mJSRuntime, JSGC_MAX_BYTES, 0xffffffff);
        JS_SetNativeStackQuota(mJSRuntime, 128 * sizeof(size_t) * 1024);
        JS_SetContextCallback(mJSRuntime, ContextCallback);
        JS_SetCompartmentCallback(mJSRuntime, CompartmentCallback);
        JS_SetGCCallbackRT(mJSRuntime, GCCallback);
        JS_SetExtraGCRootsTracer(mJSRuntime, TraceBlackJS, this);
        JS_SetGrayGCRootsTracer(mJSRuntime, TraceGrayJS, this);
        JS_SetWrapObjectCallbacks(mJSRuntime,
                                  xpc::WrapperFactory::Rewrap,
                                  xpc::WrapperFactory::PrepareForWrapping);
        js::SetPreserveWrapperCallback(mJSRuntime, PreserveWrapper);

#ifdef MOZ_CRASHREPORTER
        JS_EnumerateDiagnosticMemoryRegions(DiagnosticMemoryCallback);
#endif
        JS_SetAccumulateTelemetryCallback(mJSRuntime, AccumulateTelemetryCallback);
        mWatchdogWakeup = PR_NewCondVar(js::GetRuntimeGCLock(mJSRuntime));
        if (!mWatchdogWakeup)
            NS_RUNTIMEABORT("PR_NewCondVar failed.");

        js::SetActivityCallback(mJSRuntime, ActivityCallback, this);

        NS_RegisterMemoryReporter(new NS_MEMORY_REPORTER_NAME(XPConnectJSGCHeap));
        NS_RegisterMemoryReporter(new NS_MEMORY_REPORTER_NAME(XPConnectJSSystemCompartmentCount));
        NS_RegisterMemoryReporter(new NS_MEMORY_REPORTER_NAME(XPConnectJSUserCompartmentCount));
        NS_RegisterMemoryMultiReporter(new JSMemoryMultiReporter);
        NS_RegisterMemoryMultiReporter(new JSCompartmentsMultiReporter);
    }

    if (!JS_DHashTableInit(&mJSHolders, JS_DHashGetStubOps(), nsnull,
                           sizeof(ObjectHolder), 512))
        mJSHolders.ops = nsnull;

    mCompartmentMap.Init();

    // Install a JavaScript 'debugger' keyword handler in debug builds only
#ifdef DEBUG
    if (mJSRuntime && !JS_GetGlobalDebugHooks(mJSRuntime)->debuggerHandler)
        xpc_InstallJSDebuggerKeywordHandler(mJSRuntime);
#endif

    if (mWatchdogWakeup) {
        js::AutoLockGC lock(mJSRuntime);

        mWatchdogThread = PR_CreateThread(PR_USER_THREAD, WatchdogMain, this,
                                          PR_PRIORITY_NORMAL, PR_LOCAL_THREAD,
                                          PR_UNJOINABLE_THREAD, 0);
        if (!mWatchdogThread)
            NS_RUNTIMEABORT("PR_CreateThread failed!");
    }
}

// static
XPCJSRuntime*
XPCJSRuntime::newXPCJSRuntime(nsXPConnect* aXPConnect)
{
    NS_PRECONDITION(aXPConnect,"bad param");

    XPCJSRuntime* self = new XPCJSRuntime(aXPConnect);

    if (self                                  &&
        self->GetJSRuntime()                  &&
        self->GetWrappedJSMap()               &&
        self->GetWrappedJSClassMap()          &&
        self->GetIID2NativeInterfaceMap()     &&
        self->GetClassInfo2NativeSetMap()     &&
        self->GetNativeSetMap()               &&
        self->GetThisTranslatorMap()          &&
        self->GetNativeScriptableSharedMap()  &&
        self->GetDyingWrappedNativeProtoMap() &&
        self->GetExplicitNativeWrapperMap()   &&
        self->GetMapLock()                    &&
        self->mWatchdogThread) {
        return self;
    }

    NS_RUNTIMEABORT("new XPCJSRuntime failed to initialize.");

    delete self;
    return nsnull;
}

// DefineStaticDictionaryJSVals is automatically generated.
bool DefineStaticDictionaryJSVals(JSContext* aCx);

JSBool
XPCJSRuntime::OnJSContextNew(JSContext *cx)
{
    NS_TIME_FUNCTION;

    // if it is our first context then we need to generate our string ids
    JSBool ok = true;
    if (JSID_IS_VOID(mStrIDs[0])) {
        JS_SetGCParameterForThread(cx, JSGC_MAX_CODE_CACHE_BYTES, 16 * 1024 * 1024);
        {
            // Scope the JSAutoRequest so it goes out of scope before calling
            // mozilla::dom::binding::DefineStaticJSVals.
            JSAutoRequest ar(cx);
            for (unsigned i = 0; i < IDX_TOTAL_COUNT; i++) {
                JSString* str = JS_InternString(cx, mStrings[i]);
                if (!str || !JS_ValueToId(cx, STRING_TO_JSVAL(str), &mStrIDs[i])) {
                    mStrIDs[0] = JSID_VOID;
                    ok = false;
                    break;
                }
                mStrJSVals[i] = STRING_TO_JSVAL(str);
            }
        }

        ok = mozilla::dom::binding::DefineStaticJSVals(cx);
        if (!ok)
            return false;

        ok = DefineStaticDictionaryJSVals(cx);
    }
    if (!ok)
        return false;

    XPCPerThreadData* tls = XPCPerThreadData::GetData(cx);
    if (!tls)
        return false;

    XPCContext* xpc = new XPCContext(this, cx);
    if (!xpc)
        return false;

    // we want to mark the global object ourselves since we use a different color
    JS_ToggleOptions(cx, JSOPTION_UNROOTED_GLOBAL);

    return true;
}

JSBool
XPCJSRuntime::DeferredRelease(nsISupports* obj)
{
    NS_ASSERTION(obj, "bad param");

    if (mNativesToReleaseArray.IsEmpty()) {
        // This array sometimes has 1000's
        // of entries, and usually has 50-200 entries. Avoid lots
        // of incremental grows.  We compact it down when we're done.
        mNativesToReleaseArray.SetCapacity(256);
    }
    return mNativesToReleaseArray.AppendElement(obj) != nsnull;
}

/***************************************************************************/

#ifdef DEBUG
static JSDHashOperator
WrappedJSClassMapDumpEnumerator(JSDHashTable *table, JSDHashEntryHdr *hdr,
                                uint32_t number, void *arg)
{
    ((IID2WrappedJSClassMap::Entry*)hdr)->value->DebugDump(*(PRInt16*)arg);
    return JS_DHASH_NEXT;
}
static JSDHashOperator
WrappedJSMapDumpEnumerator(JSDHashTable *table, JSDHashEntryHdr *hdr,
                           uint32_t number, void *arg)
{
    ((JSObject2WrappedJSMap::Entry*)hdr)->value->DebugDump(*(PRInt16*)arg);
    return JS_DHASH_NEXT;
}
static JSDHashOperator
NativeSetDumpEnumerator(JSDHashTable *table, JSDHashEntryHdr *hdr,
                        uint32_t number, void *arg)
{
    ((NativeSetMap::Entry*)hdr)->key_value->DebugDump(*(PRInt16*)arg);
    return JS_DHASH_NEXT;
}
#endif

void
XPCJSRuntime::DebugDump(PRInt16 depth)
{
#ifdef DEBUG
    depth--;
    XPC_LOG_ALWAYS(("XPCJSRuntime @ %x", this));
        XPC_LOG_INDENT();
        XPC_LOG_ALWAYS(("mXPConnect @ %x", mXPConnect));
        XPC_LOG_ALWAYS(("mJSRuntime @ %x", mJSRuntime));
        XPC_LOG_ALWAYS(("mMapLock @ %x", mMapLock));

        XPC_LOG_ALWAYS(("mWrappedJSToReleaseArray @ %x with %d wrappers(s)", \
                        &mWrappedJSToReleaseArray,
                        mWrappedJSToReleaseArray.Length()));

        int cxCount = 0;
        JSContext* iter = nsnull;
        while (JS_ContextIterator(mJSRuntime, &iter))
            ++cxCount;
        XPC_LOG_ALWAYS(("%d JS context(s)", cxCount));

        iter = nsnull;
        while (JS_ContextIterator(mJSRuntime, &iter)) {
            XPCContext *xpc = XPCContext::GetXPCContext(iter);
            XPC_LOG_INDENT();
            xpc->DebugDump(depth);
            XPC_LOG_OUTDENT();
        }

        XPC_LOG_ALWAYS(("mWrappedJSClassMap @ %x with %d wrapperclasses(s)",  \
                        mWrappedJSClassMap, mWrappedJSClassMap ?              \
                        mWrappedJSClassMap->Count() : 0));
        // iterate wrappersclasses...
        if (depth && mWrappedJSClassMap && mWrappedJSClassMap->Count()) {
            XPC_LOG_INDENT();
            mWrappedJSClassMap->Enumerate(WrappedJSClassMapDumpEnumerator, &depth);
            XPC_LOG_OUTDENT();
        }
        XPC_LOG_ALWAYS(("mWrappedJSMap @ %x with %d wrappers(s)",             \
                        mWrappedJSMap, mWrappedJSMap ?                        \
                        mWrappedJSMap->Count() : 0));
        // iterate wrappers...
        if (depth && mWrappedJSMap && mWrappedJSMap->Count()) {
            XPC_LOG_INDENT();
            mWrappedJSMap->Enumerate(WrappedJSMapDumpEnumerator, &depth);
            XPC_LOG_OUTDENT();
        }

        XPC_LOG_ALWAYS(("mIID2NativeInterfaceMap @ %x with %d interface(s)",  \
                        mIID2NativeInterfaceMap, mIID2NativeInterfaceMap ?    \
                        mIID2NativeInterfaceMap->Count() : 0));

        XPC_LOG_ALWAYS(("mClassInfo2NativeSetMap @ %x with %d sets(s)",       \
                        mClassInfo2NativeSetMap, mClassInfo2NativeSetMap ?    \
                        mClassInfo2NativeSetMap->Count() : 0));

        XPC_LOG_ALWAYS(("mThisTranslatorMap @ %x with %d translator(s)",      \
                        mThisTranslatorMap, mThisTranslatorMap ?              \
                        mThisTranslatorMap->Count() : 0));

        XPC_LOG_ALWAYS(("mNativeSetMap @ %x with %d sets(s)",                 \
                        mNativeSetMap, mNativeSetMap ?                        \
                        mNativeSetMap->Count() : 0));

        // iterate sets...
        if (depth && mNativeSetMap && mNativeSetMap->Count()) {
            XPC_LOG_INDENT();
            mNativeSetMap->Enumerate(NativeSetDumpEnumerator, &depth);
            XPC_LOG_OUTDENT();
        }

        XPC_LOG_OUTDENT();
#endif
}

/***************************************************************************/

void
XPCRootSetElem::AddToRootSet(XPCLock *lock, XPCRootSetElem **listHead)
{
    NS_ASSERTION(!mSelfp, "Must be not linked");

    XPCAutoLock autoLock(lock);

    mSelfp = listHead;
    mNext = *listHead;
    if (mNext) {
        NS_ASSERTION(mNext->mSelfp == listHead, "Must be list start");
        mNext->mSelfp = &mNext;
    }
    *listHead = this;
}

void
XPCRootSetElem::RemoveFromRootSet(XPCLock *lock)
{
    NS_ASSERTION(mSelfp, "Must be linked");

    XPCAutoLock autoLock(lock);

    NS_ASSERTION(*mSelfp == this, "Link invariant");
    *mSelfp = mNext;
    if (mNext)
        mNext->mSelfp = mSelfp;
#ifdef DEBUG
    mSelfp = nsnull;
    mNext = nsnull;
#endif
}

void
XPCJSRuntime::AddGCCallback(JSGCCallback cb)
{
    NS_ASSERTION(cb, "null callback");
    extraGCCallbacks.AppendElement(cb);
}

void
XPCJSRuntime::RemoveGCCallback(JSGCCallback cb)
{
    NS_ASSERTION(cb, "null callback");
    bool found = extraGCCallbacks.RemoveElement(cb);
    if (!found) {
        NS_ERROR("Removing a callback which was never added.");
    }
}
