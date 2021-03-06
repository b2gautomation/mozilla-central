/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
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

#ifndef jsobjinlines_h___
#define jsobjinlines_h___

#include <new>

#include "jsapi.h"
#include "jsarray.h"
#include "jsbool.h"
#include "jscntxt.h"
#include "jsdate.h"
#include "jsfun.h"
#include "jsgcmark.h"
#include "jsiter.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsprobes.h"
#include "jspropertytree.h"
#include "jsproxy.h"
#include "jsscope.h"
#include "jsstr.h"
#include "jstypedarray.h"
#include "jsxml.h"
#include "jswrapper.h"

#include "gc/Barrier.h"
#include "js/TemplateLib.h"

#include "vm/BooleanObject.h"
#include "vm/GlobalObject.h"
#include "vm/NumberObject.h"
#include "vm/RegExpStatics.h"
#include "vm/StringObject.h"

#include "jsatominlines.h"
#include "jsfuninlines.h"
#include "jsgcinlines.h"
#include "jsinferinlines.h"
#include "jsscopeinlines.h"
#include "jsscriptinlines.h"

#include "gc/Barrier-inl.h"

#include "vm/ObjectImpl-inl.h"
#include "vm/RegExpStatics-inl.h"
#include "vm/String-inl.h"

inline bool
JSObject::hasPrivate() const
{
    return getClass()->hasPrivate();
}

inline void *&
JSObject::privateRef(uint32_t nfixed) const
{
    /*
     * The private pointer of an object can hold any word sized value.
     * Private pointers are stored immediately after the last fixed slot of
     * the object.
     */
    JS_ASSERT(nfixed == numFixedSlots());
    JS_ASSERT(hasPrivate());
    js::HeapSlot *end = &fixedSlots()[nfixed];
    return *reinterpret_cast<void**>(end);
}

inline void *
JSObject::getPrivate() const { return privateRef(numFixedSlots()); }

inline void *
JSObject::getPrivate(size_t nfixed) const { return privateRef(nfixed); }

inline void
JSObject::setPrivate(void *data)
{
    void **pprivate = &privateRef(numFixedSlots());

    privateWriteBarrierPre(pprivate);
    *pprivate = data;
    privateWriteBarrierPost(pprivate);
}

inline void
JSObject::initPrivate(void *data)
{
    privateRef(numFixedSlots()) = data;
}

inline bool
JSObject::enumerate(JSContext *cx, JSIterateOp iterop, js::Value *statep, jsid *idp)
{
    JSNewEnumerateOp op = getOps()->enumerate;
    return (op ? op : JS_EnumerateState)(cx, this, iterop, statep, idp);
}

inline bool
JSObject::defaultValue(JSContext *cx, JSType hint, js::Value *vp)
{
    JSConvertOp op = getClass()->convert;
    bool ok = (op == JS_ConvertStub ? js::DefaultValue : op)(cx, this, hint, vp);
    JS_ASSERT_IF(ok, vp->isPrimitive());
    return ok;
}

inline JSType
JSObject::typeOf(JSContext *cx)
{
    js::TypeOfOp op = getOps()->typeOf;
    return (op ? op : js_TypeOf)(cx, this);
}

inline JSObject *
JSObject::thisObject(JSContext *cx)
{
    JSObjectOp op = getOps()->thisObject;
    return op ? op(cx, this) : this;
}

inline JSBool
JSObject::setGeneric(JSContext *cx, jsid id, js::Value *vp, JSBool strict)
{
    if (getOps()->setGeneric)
        return nonNativeSetProperty(cx, id, vp, strict);
    return js_SetPropertyHelper(cx, this, id, 0, vp, strict);
}

inline JSBool
JSObject::setProperty(JSContext *cx, js::PropertyName *name, js::Value *vp, JSBool strict)
{
    return setGeneric(cx, ATOM_TO_JSID(name), vp, strict);
}

inline JSBool
JSObject::setElement(JSContext *cx, uint32_t index, js::Value *vp, JSBool strict)
{
    if (getOps()->setElement)
        return nonNativeSetElement(cx, index, vp, strict);
    return js_SetElementHelper(cx, this, index, 0, vp, strict);
}

inline JSBool
JSObject::setSpecial(JSContext *cx, js::SpecialId sid, js::Value *vp, JSBool strict)
{
    return setGeneric(cx, SPECIALID_TO_JSID(sid), vp, strict);
}

inline JSBool
JSObject::setGenericAttributes(JSContext *cx, jsid id, unsigned *attrsp)
{
    js::types::MarkTypePropertyConfigured(cx, this, id);
    js::GenericAttributesOp op = getOps()->setGenericAttributes;
    return (op ? op : js_SetAttributes)(cx, this, id, attrsp);
}

inline JSBool
JSObject::setPropertyAttributes(JSContext *cx, js::PropertyName *name, unsigned *attrsp)
{
    return setGenericAttributes(cx, ATOM_TO_JSID(name), attrsp);
}

inline JSBool
JSObject::setElementAttributes(JSContext *cx, uint32_t index, unsigned *attrsp)
{
    js::ElementAttributesOp op = getOps()->setElementAttributes;
    return (op ? op : js_SetElementAttributes)(cx, this, index, attrsp);
}

inline JSBool
JSObject::setSpecialAttributes(JSContext *cx, js::SpecialId sid, unsigned *attrsp)
{
    return setGenericAttributes(cx, SPECIALID_TO_JSID(sid), attrsp);
}

inline JSBool
JSObject::getGeneric(JSContext *cx, JSObject *receiver, jsid id, js::Value *vp)
{
    js::GenericIdOp op = getOps()->getGeneric;
    if (op) {
        if (!op(cx, this, receiver, id, vp))
            return false;
    } else {
        if (!js_GetProperty(cx, this, receiver, id, vp))
            return false;
    }
    return true;
}

inline JSBool
JSObject::getProperty(JSContext *cx, JSObject *receiver, js::PropertyName *name, js::Value *vp)
{
    return getGeneric(cx, receiver, ATOM_TO_JSID(name), vp);
}

inline JSBool
JSObject::getGeneric(JSContext *cx, jsid id, js::Value *vp)
{
    return getGeneric(cx, this, id, vp);
}

inline JSBool
JSObject::getProperty(JSContext *cx, js::PropertyName *name, js::Value *vp)
{
    return getGeneric(cx, ATOM_TO_JSID(name), vp);
}

inline bool
JSObject::deleteProperty(JSContext *cx, js::PropertyName *name, js::Value *rval, bool strict)
{
    jsid id = js_CheckForStringIndex(ATOM_TO_JSID(name));
    js::types::AddTypePropertyId(cx, this, id, js::types::Type::UndefinedType());
    js::types::MarkTypePropertyConfigured(cx, this, id);
    js::DeletePropertyOp op = getOps()->deleteProperty;
    return (op ? op : js_DeleteProperty)(cx, this, name, rval, strict);
}

inline bool
JSObject::deleteElement(JSContext *cx, uint32_t index, js::Value *rval, bool strict)
{
    jsid id;
    if (!js::IndexToId(cx, index, &id))
        return false;
    js::types::AddTypePropertyId(cx, this, id, js::types::Type::UndefinedType());
    js::types::MarkTypePropertyConfigured(cx, this, id);
    js::DeleteElementOp op = getOps()->deleteElement;
    return (op ? op : js_DeleteElement)(cx, this, index, rval, strict);
}

inline bool
JSObject::deleteSpecial(JSContext *cx, js::SpecialId sid, js::Value *rval, bool strict)
{
    jsid id = SPECIALID_TO_JSID(sid);
    js::types::AddTypePropertyId(cx, this, id, js::types::Type::UndefinedType());
    js::types::MarkTypePropertyConfigured(cx, this, id);
    js::DeleteSpecialOp op = getOps()->deleteSpecial;
    return (op ? op : js_DeleteSpecial)(cx, this, sid, rval, strict);
}

inline void
JSObject::finalize(JSContext *cx, bool background)
{
    js::Probes::finalizeObject(this);

    if (!background) {
        /*
         * Finalize obj first, in case it needs map and slots. Objects with
         * finalize hooks are not finalized in the background, as the class is
         * stored in the object's shape, which may have already been destroyed.
         */
        js::Class *clasp = getClass();
        if (clasp->finalize)
            clasp->finalize(cx, this);
    }

    finish(cx);
}

inline JSObject *
JSObject::getParent() const
{
    return lastProperty()->getObjectParent();
}

inline JSObject *
JSObject::enclosingScope()
{
    return isScope() ? &asScope().enclosingScope() : getParent();
}

/*
 * Property read barrier for deferred cloning of compiler-created function
 * objects optimized as typically non-escaping, ad-hoc methods in obj.
 */
inline js::Shape *
JSObject::methodReadBarrier(JSContext *cx, const js::Shape &shape, js::Value *vp)
{
    JS_ASSERT(nativeContains(cx, shape));
    JS_ASSERT(shape.isMethod());
    JS_ASSERT(shape.hasSlot());
    JS_ASSERT(shape.hasDefaultSetter());
    JS_ASSERT(!isGlobal());  /* i.e. we are not changing the global shape */

    JSFunction *fun = vp->toObject().toFunction();
    JS_ASSERT(!fun->isClonedMethod());
    JS_ASSERT(fun->isNullClosure());

    fun = js::CloneFunctionObject(cx, fun);
    if (!fun)
        return NULL;
    fun->setMethodObj(*this);

    /*
     * Replace the method property with an ordinary data property. This is
     * equivalent to this->setProperty(cx, shape.id, vp) except that any
     * watchpoint on the property is not triggered.
     */
    uint32_t slot = shape.slot();
    js::Shape *newshape = methodShapeChange(cx, shape);
    if (!newshape)
        return NULL;
    JS_ASSERT(!newshape->isMethod());
    JS_ASSERT(newshape->slot() == slot);
    vp->setObject(*fun);
    nativeSetSlot(slot, *vp);
    return newshape;
}

inline bool
JSObject::canHaveMethodBarrier() const
{
    return isObject() || isFunction() || isPrimitive() || isDate();
}

inline bool
JSObject::isFixedSlot(size_t slot)
{
    JS_ASSERT(!isDenseArray());
    return slot < numFixedSlots();
}

inline size_t
JSObject::dynamicSlotIndex(size_t slot)
{
    JS_ASSERT(!isDenseArray() && slot >= numFixedSlots());
    return slot - numFixedSlots();
}

inline size_t
JSObject::numDynamicSlots() const
{
    return dynamicSlotsCount(numFixedSlots(), slotSpan());
}

inline void
JSObject::setLastPropertyInfallible(const js::Shape *shape)
{
    JS_ASSERT(!shape->inDictionary());
    JS_ASSERT(shape->compartment() == compartment());
    JS_ASSERT(!inDictionaryMode());
    JS_ASSERT(slotSpan() == shape->slotSpan());
    JS_ASSERT(numFixedSlots() == shape->numFixedSlots());

    shape_ = const_cast<js::Shape *>(shape);
}

inline void
JSObject::removeLastProperty(JSContext *cx)
{
    JS_ASSERT(canRemoveLastProperty());
    JS_ALWAYS_TRUE(setLastProperty(cx, lastProperty()->previous()));
}

inline bool
JSObject::canRemoveLastProperty()
{
    /*
     * Check that the information about the object stored in the last
     * property's base shape is consistent with that stored in the previous
     * shape. If not consistent, then the last property cannot be removed as it
     * will induce a change in the object itself, and the object must be
     * converted to dictionary mode instead. See BaseShape comment in jsscope.h
     */
    JS_ASSERT(!inDictionaryMode());
    const js::Shape *previous = lastProperty()->previous();
    return previous->getObjectParent() == lastProperty()->getObjectParent()
        && previous->getObjectFlags() == lastProperty()->getObjectFlags();
}

inline const js::HeapSlot *
JSObject::getRawSlots()
{
    JS_ASSERT(isGlobal());
    return slots;
}

inline const js::Value &
JSObject::getReservedSlot(unsigned index) const
{
    JS_ASSERT(index < JSSLOT_FREE(getClass()));
    return getSlot(index);
}

inline js::HeapSlot &
JSObject::getReservedSlotRef(unsigned index)
{
    JS_ASSERT(index < JSSLOT_FREE(getClass()));
    return getSlotRef(index);
}

inline void
JSObject::setReservedSlot(unsigned index, const js::Value &v)
{
    JS_ASSERT(index < JSSLOT_FREE(getClass()));
    setSlot(index, v);
}

inline void
JSObject::initReservedSlot(unsigned index, const js::Value &v)
{
    JS_ASSERT(index < JSSLOT_FREE(getClass()));
    initSlot(index, v);
}

inline bool
JSObject::hasContiguousSlots(size_t start, size_t count) const
{
    /*
     * Check that the range [start, start+count) is either all inline or all
     * out of line.
     */
    JS_ASSERT(slotInRange(start + count, SENTINEL_ALLOWED));
    return (start + count <= numFixedSlots()) || (start >= numFixedSlots());
}

inline void
JSObject::prepareSlotRangeForOverwrite(size_t start, size_t end)
{
    for (size_t i = start; i < end; i++)
        getSlotAddressUnchecked(i)->js::HeapSlot::~HeapSlot();
}

inline void
JSObject::prepareElementRangeForOverwrite(size_t start, size_t end)
{
    JS_ASSERT(isDenseArray());
    JS_ASSERT(end <= getDenseArrayInitializedLength());
    for (size_t i = start; i < end; i++)
        elements[i].js::HeapSlot::~HeapSlot();
}

inline uint32_t
JSObject::getArrayLength() const
{
    JS_ASSERT(isArray());
    return getElementsHeader()->length;
}

inline void
JSObject::setArrayLength(JSContext *cx, uint32_t length)
{
    JS_ASSERT(isArray());

    if (length > INT32_MAX) {
        /*
         * Mark the type of this object as possibly not a dense array, per the
         * requirements of OBJECT_FLAG_NON_DENSE_ARRAY.
         */
        js::types::MarkTypeObjectFlags(cx, this,
                                       js::types::OBJECT_FLAG_NON_PACKED_ARRAY |
                                       js::types::OBJECT_FLAG_NON_DENSE_ARRAY);
        jsid lengthId = ATOM_TO_JSID(cx->runtime->atomState.lengthAtom);
        js::types::AddTypePropertyId(cx, this, lengthId,
                                     js::types::Type::DoubleType());
    }

    getElementsHeader()->length = length;
}

inline void
JSObject::setDenseArrayLength(uint32_t length)
{
    /* Variant of setArrayLength for use on dense arrays where the length cannot overflow int32. */
    JS_ASSERT(isDenseArray());
    JS_ASSERT(length <= INT32_MAX);
    getElementsHeader()->length = length;
}

inline uint32_t
JSObject::getDenseArrayInitializedLength()
{
    JS_ASSERT(isDenseArray());
    return getElementsHeader()->initializedLength;
}

inline void
JSObject::setDenseArrayInitializedLength(uint32_t length)
{
    JS_ASSERT(isDenseArray());
    JS_ASSERT(length <= getDenseArrayCapacity());
    prepareElementRangeForOverwrite(length, getElementsHeader()->initializedLength);
    getElementsHeader()->initializedLength = length;
}

inline uint32_t
JSObject::getDenseArrayCapacity()
{
    JS_ASSERT(isDenseArray());
    return getElementsHeader()->capacity;
}

inline bool
JSObject::ensureElements(JSContext *cx, uint32_t capacity)
{
    if (capacity > getDenseArrayCapacity())
        return growElements(cx, capacity);
    return true;
}

inline js::HeapSlotArray
JSObject::getDenseArrayElements()
{
    JS_ASSERT(isDenseArray());
    return js::HeapSlotArray(elements);
}

inline const js::Value &
JSObject::getDenseArrayElement(unsigned idx)
{
    JS_ASSERT(isDenseArray() && idx < getDenseArrayInitializedLength());
    return elements[idx];
}

inline void
JSObject::setDenseArrayElement(unsigned idx, const js::Value &val)
{
    JS_ASSERT(isDenseArray() && idx < getDenseArrayInitializedLength());
    elements[idx].set(this, idx, val);
}

inline void
JSObject::initDenseArrayElement(unsigned idx, const js::Value &val)
{
    JS_ASSERT(isDenseArray() && idx < getDenseArrayInitializedLength());
    elements[idx].init(this, idx, val);
}

inline void
JSObject::setDenseArrayElementWithType(JSContext *cx, unsigned idx, const js::Value &val)
{
    js::types::AddTypePropertyId(cx, this, JSID_VOID, val);
    setDenseArrayElement(idx, val);
}

inline void
JSObject::initDenseArrayElementWithType(JSContext *cx, unsigned idx, const js::Value &val)
{
    js::types::AddTypePropertyId(cx, this, JSID_VOID, val);
    initDenseArrayElement(idx, val);
}

inline void
JSObject::copyDenseArrayElements(unsigned dstStart, const js::Value *src, unsigned count)
{
    JS_ASSERT(dstStart + count <= getDenseArrayCapacity());
    JSCompartment *comp = compartment();
    for (unsigned i = 0; i < count; ++i)
        elements[dstStart + i].set(comp, this, dstStart + i, src[i]);
}

inline void
JSObject::initDenseArrayElements(unsigned dstStart, const js::Value *src, unsigned count)
{
    JS_ASSERT(dstStart + count <= getDenseArrayCapacity());
    JSCompartment *comp = compartment();
    for (unsigned i = 0; i < count; ++i)
        elements[dstStart + i].init(comp, this, dstStart + i, src[i]);
}

inline void
JSObject::moveDenseArrayElements(unsigned dstStart, unsigned srcStart, unsigned count)
{
    JS_ASSERT(dstStart + count <= getDenseArrayCapacity());
    JS_ASSERT(srcStart + count <= getDenseArrayInitializedLength());

    /*
     * Using memmove here would skip write barriers. Also, we need to consider
     * an array containing [A, B, C], in the following situation:
     *
     * 1. Incremental GC marks slot 0 of array (i.e., A), then returns to JS code.
     * 2. JS code moves slots 1..2 into slots 0..1, so it contains [B, C, C].
     * 3. Incremental GC finishes by marking slots 1 and 2 (i.e., C).
     *
     * Since normal marking never happens on B, it is very important that the
     * write barrier is invoked here on B, despite the fact that it exists in
     * the array before and after the move.
    */
    JSCompartment *comp = compartment();
    if (comp->needsBarrier()) {
        if (dstStart < srcStart) {
            js::HeapSlot *dst = elements + dstStart;
            js::HeapSlot *src = elements + srcStart;
            for (unsigned i = 0; i < count; i++, dst++, src++)
                dst->set(comp, this, dst - elements, *src);
        } else {
            js::HeapSlot *dst = elements + dstStart + count - 1;
            js::HeapSlot *src = elements + srcStart + count - 1;
            for (unsigned i = 0; i < count; i++, dst--, src--)
                dst->set(comp, this, dst - elements, *src);
        }
    } else {
        memmove(elements + dstStart, elements + srcStart, count * sizeof(js::HeapSlot));
    }
}

inline void
JSObject::moveDenseArrayElementsUnbarriered(unsigned dstStart, unsigned srcStart, unsigned count)
{
    JS_ASSERT(!compartment()->needsBarrier());

    JS_ASSERT(dstStart + count <= getDenseArrayCapacity());
    JS_ASSERT(srcStart + count <= getDenseArrayCapacity());

    memmove(elements + dstStart, elements + srcStart, count * sizeof(js::Value));
}

inline bool
JSObject::denseArrayHasInlineSlots() const
{
    JS_ASSERT(isDenseArray());
    return elements == fixedElements();
}

namespace js {

inline JSObject *
ValueToObjectOrPrototype(JSContext *cx, const Value &v)
{
    if (v.isObject())
        return &v.toObject();
    GlobalObject *global = &cx->fp()->scopeChain().global();
    if (v.isString())
        return global->getOrCreateStringPrototype(cx);
    if (v.isNumber())
        return global->getOrCreateNumberPrototype(cx);
    if (v.isBoolean())
        return global->getOrCreateBooleanPrototype(cx);
    JS_ASSERT(v.isNull() || v.isUndefined());
    js_ReportIsNullOrUndefined(cx, JSDVG_SEARCH_STACK, v, NULL);
    return NULL;
}

/*
 * Any name atom for a function which will be added as a DeclEnv object to the
 * scope chain above call objects for fun.
 */
static inline JSAtom *
CallObjectLambdaName(JSFunction *fun)
{
    return (fun->flags & JSFUN_LAMBDA) ? fun->atom : NULL;
}

} /* namespace js */

inline const js::Value &
JSObject::getDateUTCTime() const
{
    JS_ASSERT(isDate());
    return getFixedSlot(JSSLOT_DATE_UTC_TIME);
}

inline void 
JSObject::setDateUTCTime(const js::Value &time)
{
    JS_ASSERT(isDate());
    setFixedSlot(JSSLOT_DATE_UTC_TIME, time);
}

inline js::NativeIterator *
JSObject::getNativeIterator() const
{
    return (js::NativeIterator *) getPrivate();
}

inline void
JSObject::setNativeIterator(js::NativeIterator *ni)
{
    setPrivate(ni);
}

inline JSLinearString *
JSObject::getNamePrefix() const
{
    JS_ASSERT(isNamespace() || isQName());
    const js::Value &v = getSlot(JSSLOT_NAME_PREFIX);
    return !v.isUndefined() ? &v.toString()->asLinear() : NULL;
}

inline jsval
JSObject::getNamePrefixVal() const
{
    JS_ASSERT(isNamespace() || isQName());
    return getSlot(JSSLOT_NAME_PREFIX);
}

inline void
JSObject::setNamePrefix(JSLinearString *prefix)
{
    JS_ASSERT(isNamespace() || isQName());
    setSlot(JSSLOT_NAME_PREFIX, prefix ? js::StringValue(prefix) : js::UndefinedValue());
}

inline void
JSObject::clearNamePrefix()
{
    JS_ASSERT(isNamespace() || isQName());
    setSlot(JSSLOT_NAME_PREFIX, js::UndefinedValue());
}

inline JSLinearString *
JSObject::getNameURI() const
{
    JS_ASSERT(isNamespace() || isQName());
    const js::Value &v = getSlot(JSSLOT_NAME_URI);
    return !v.isUndefined() ? &v.toString()->asLinear() : NULL;
}

inline jsval
JSObject::getNameURIVal() const
{
    JS_ASSERT(isNamespace() || isQName());
    return getSlot(JSSLOT_NAME_URI);
}

inline void
JSObject::setNameURI(JSLinearString *uri)
{
    JS_ASSERT(isNamespace() || isQName());
    setSlot(JSSLOT_NAME_URI, uri ? js::StringValue(uri) : js::UndefinedValue());
}

inline jsval
JSObject::getNamespaceDeclared() const
{
    JS_ASSERT(isNamespace());
    return getSlot(JSSLOT_NAMESPACE_DECLARED);
}

inline void
JSObject::setNamespaceDeclared(jsval decl)
{
    JS_ASSERT(isNamespace());
    setSlot(JSSLOT_NAMESPACE_DECLARED, decl);
}

inline JSAtom *
JSObject::getQNameLocalName() const
{
    JS_ASSERT(isQName());
    const js::Value &v = getSlot(JSSLOT_QNAME_LOCAL_NAME);
    return !v.isUndefined() ? &v.toString()->asAtom() : NULL;
}

inline jsval
JSObject::getQNameLocalNameVal() const
{
    JS_ASSERT(isQName());
    return getSlot(JSSLOT_QNAME_LOCAL_NAME);
}

inline void
JSObject::setQNameLocalName(JSAtom *name)
{
    JS_ASSERT(isQName());
    setSlot(JSSLOT_QNAME_LOCAL_NAME, name ? js::StringValue(name) : js::UndefinedValue());
}

inline bool
JSObject::setSingletonType(JSContext *cx)
{
    if (!cx->typeInferenceEnabled())
        return true;

    JS_ASSERT(!lastProperty()->previous());
    JS_ASSERT(!hasLazyType());
    JS_ASSERT_IF(getProto(), type() == getProto()->getNewType(cx, NULL));

    js::types::TypeObject *type = cx->compartment->getLazyType(cx, getProto());
    if (!type)
        return false;

    type_ = type;
    return true;
}

inline js::types::TypeObject *
JSObject::getType(JSContext *cx)
{
    if (hasLazyType())
        makeLazyType(cx);
    return type_;
}

inline bool
JSObject::clearType(JSContext *cx)
{
    JS_ASSERT(!hasSingletonType());

    js::types::TypeObject *type = cx->compartment->getEmptyType(cx);
    if (!type)
        return false;

    type_ = type;
    return true;
}

inline void
JSObject::setType(js::types::TypeObject *newType)
{
#ifdef DEBUG
    JS_ASSERT(newType);
    for (JSObject *obj = newType->proto; obj; obj = obj->getProto())
        JS_ASSERT(obj != this);
#endif
    JS_ASSERT_IF(hasSpecialEquality(),
                 newType->hasAnyFlags(js::types::OBJECT_FLAG_SPECIAL_EQUALITY));
    JS_ASSERT(!hasSingletonType());
    type_ = newType;
}

inline bool JSObject::setIteratedSingleton(JSContext *cx)
{
    return setFlag(cx, js::BaseShape::ITERATED_SINGLETON);
}

inline bool JSObject::isSystem() const
{
    return lastProperty()->hasObjectFlag(js::BaseShape::SYSTEM);
}

inline bool JSObject::setSystem(JSContext *cx)
{
    return setFlag(cx, js::BaseShape::SYSTEM);
}

inline bool JSObject::setDelegate(JSContext *cx)
{
    return setFlag(cx, js::BaseShape::DELEGATE, GENERATE_SHAPE);
}

inline bool JSObject::isVarObj() const
{
    return lastProperty()->hasObjectFlag(js::BaseShape::VAROBJ);
}

inline bool JSObject::setVarObj(JSContext *cx)
{
    return setFlag(cx, js::BaseShape::VAROBJ);
}

inline bool JSObject::setWatched(JSContext *cx)
{
    return setFlag(cx, js::BaseShape::WATCHED, GENERATE_SHAPE);
}

inline bool JSObject::hasUncacheableProto() const
{
    return lastProperty()->hasObjectFlag(js::BaseShape::UNCACHEABLE_PROTO);
}

inline bool JSObject::setUncacheableProto(JSContext *cx)
{
    return setFlag(cx, js::BaseShape::UNCACHEABLE_PROTO, GENERATE_SHAPE);
}

inline bool JSObject::isBoundFunction() const
{
    return lastProperty()->hasObjectFlag(js::BaseShape::BOUND_FUNCTION);
}

inline bool JSObject::isIndexed() const
{
    return lastProperty()->hasObjectFlag(js::BaseShape::INDEXED);
}

inline bool JSObject::watched() const
{
    return lastProperty()->hasObjectFlag(js::BaseShape::WATCHED);
}

inline bool JSObject::hasSpecialEquality() const
{
    return !!getClass()->ext.equality;
}

inline bool JSObject::isArguments() const { return isNormalArguments() || isStrictArguments(); }
inline bool JSObject::isArrayBuffer() const { return hasClass(&js::ArrayBufferClass); }
inline bool JSObject::isBlock() const { return hasClass(&js::BlockClass); }
inline bool JSObject::isBoolean() const { return hasClass(&js::BooleanClass); }
inline bool JSObject::isCall() const { return hasClass(&js::CallClass); }
inline bool JSObject::isClonedBlock() const { return isBlock() && !!getProto(); }
inline bool JSObject::isDate() const { return hasClass(&js::DateClass); }
inline bool JSObject::isDeclEnv() const { return hasClass(&js::DeclEnvClass); }
inline bool JSObject::isElementIterator() const { return hasClass(&js::ElementIteratorClass); }
inline bool JSObject::isError() const { return hasClass(&js::ErrorClass); }
inline bool JSObject::isFunction() const { return hasClass(&js::FunctionClass); }
inline bool JSObject::isFunctionProxy() const { return hasClass(&js::FunctionProxyClass); }
inline bool JSObject::isGenerator() const { return hasClass(&js::GeneratorClass); }
inline bool JSObject::isIterator() const { return hasClass(&js::IteratorClass); }
inline bool JSObject::isNamespace() const { return hasClass(&js::NamespaceClass); }
inline bool JSObject::isNestedScope() const { return isBlock() || isWith(); }
inline bool JSObject::isNormalArguments() const { return hasClass(&js::NormalArgumentsObjectClass); }
inline bool JSObject::isNumber() const { return hasClass(&js::NumberClass); }
inline bool JSObject::isObject() const { return hasClass(&js::ObjectClass); }
inline bool JSObject::isPrimitive() const { return isNumber() || isString() || isBoolean(); }
inline bool JSObject::isRegExp() const { return hasClass(&js::RegExpClass); }
inline bool JSObject::isRegExpStatics() const { return hasClass(&js::RegExpStaticsClass); }
inline bool JSObject::isScope() const { return isCall() || isDeclEnv() || isNestedScope(); }
inline bool JSObject::isStaticBlock() const { return isBlock() && !getProto(); }
inline bool JSObject::isStopIteration() const { return hasClass(&js::StopIterationClass); }
inline bool JSObject::isStrictArguments() const { return hasClass(&js::StrictArgumentsObjectClass); }
inline bool JSObject::isString() const { return hasClass(&js::StringClass); }
inline bool JSObject::isWeakMap() const { return hasClass(&js::WeakMapClass); }
inline bool JSObject::isWith() const { return hasClass(&js::WithClass); }
inline bool JSObject::isXML() const { return hasClass(&js::XMLClass); }

inline bool JSObject::isArray() const
{
    return isSlowArray() || isDenseArray();
}

inline bool JSObject::isDenseArray() const
{
    bool result = hasClass(&js::ArrayClass);
    JS_ASSERT_IF(result, elements != js::emptyObjectElements);
    return result;
}

inline bool JSObject::isSlowArray() const
{
    bool result = hasClass(&js::SlowArrayClass);
    JS_ASSERT_IF(result, elements != js::emptyObjectElements);
    return result;
}

inline bool
JSObject::isXMLId() const
{
    return hasClass(&js::QNameClass)
        || hasClass(&js::AttributeNameClass)
        || hasClass(&js::AnyNameClass);
}

inline bool
JSObject::isQName() const
{
    return hasClass(&js::QNameClass)
        || hasClass(&js::AttributeNameClass)
        || hasClass(&js::AnyNameClass);
}

inline void
JSObject::getSlotRangeUnchecked(size_t start, size_t length,
                                js::HeapSlot **fixedStart, js::HeapSlot **fixedEnd,
                                js::HeapSlot **slotsStart, js::HeapSlot **slotsEnd)
{
    JS_ASSERT(!isDenseArray());

    size_t fixed = numFixedSlots();
    if (start < fixed) {
        if (start + length < fixed) {
            *fixedStart = &fixedSlots()[start];
            *fixedEnd = &fixedSlots()[start + length];
            *slotsStart = *slotsEnd = NULL;
        } else {
            size_t localCopy = fixed - start;
            *fixedStart = &fixedSlots()[start];
            *fixedEnd = &fixedSlots()[start + localCopy];
            *slotsStart = &slots[0];
            *slotsEnd = &slots[length - localCopy];
        }
    } else {
        *fixedStart = *fixedEnd = NULL;
        *slotsStart = &slots[start - fixed];
        *slotsEnd = &slots[start - fixed + length];
    }
}

inline void
JSObject::getSlotRange(size_t start, size_t length,
                       js::HeapSlot **fixedStart, js::HeapSlot **fixedEnd,
                       js::HeapSlot **slotsStart, js::HeapSlot **slotsEnd)
{
    JS_ASSERT(slotInRange(start + length, SENTINEL_ALLOWED));
    getSlotRangeUnchecked(start, length, fixedStart, fixedEnd, slotsStart, slotsEnd);
}

inline void
JSObject::initializeSlotRange(size_t start, size_t length)
{
    /*
     * No bounds check, as this is used when the object's shape does not
     * reflect its allocated slots (updateSlotsForSpan).
     */
    js::HeapSlot *fixedStart, *fixedEnd, *slotsStart, *slotsEnd;
    getSlotRangeUnchecked(start, length, &fixedStart, &fixedEnd, &slotsStart, &slotsEnd);

    JSCompartment *comp = compartment();
    size_t offset = start;
    for (js::HeapSlot *sp = fixedStart; sp != fixedEnd; sp++)
        sp->init(comp, this, offset++, js::UndefinedValue());
    for (js::HeapSlot *sp = slotsStart; sp != slotsEnd; sp++)
        sp->init(comp, this, offset++, js::UndefinedValue());
}

/* static */ inline JSObject *
JSObject::create(JSContext *cx, js::gc::AllocKind kind,
                 js::HandleShape shape, js::HandleTypeObject type, js::HeapSlot *slots)
{
    /*
     * Callers must use dynamicSlotsCount to size the initial slot array of the
     * object. We can't check the allocated capacity of the dynamic slots, but
     * make sure their presence is consistent with the shape.
     */
    JS_ASSERT(shape && type);
    JS_ASSERT(!!dynamicSlotsCount(shape->numFixedSlots(), shape->slotSpan()) == !!slots);
    JS_ASSERT(js::gc::GetGCKindSlots(kind, shape->getObjectClass()) == shape->numFixedSlots());

    JSObject *obj = js_NewGCObject(cx, kind);
    if (!obj)
        return NULL;

    obj->shape_.init(shape);
    obj->type_.init(type);
    obj->slots = slots;
    obj->elements = js::emptyObjectElements;

    if (shape->getObjectClass()->hasPrivate())
        obj->privateRef(shape->numFixedSlots()) = NULL;

    if (size_t span = shape->slotSpan())
        obj->initializeSlotRange(0, span);

    return obj;
}

/* static */ inline JSObject *
JSObject::createDenseArray(JSContext *cx, js::gc::AllocKind kind,
                           js::HandleShape shape, js::HandleTypeObject type,
                           uint32_t length)
{
    JS_ASSERT(shape && type);
    JS_ASSERT(shape->getObjectClass() == &js::ArrayClass);

    /*
     * Dense arrays are non-native, and never have properties to store.
     * The number of fixed slots in the shape of such objects is zero.
     */
    JS_ASSERT(shape->numFixedSlots() == 0);

    /*
     * The array initially stores its elements inline, there must be enough
     * space for an elements header.
     */
    JS_ASSERT(js::gc::GetGCKindSlots(kind) >= js::ObjectElements::VALUES_PER_HEADER);

    uint32_t capacity = js::gc::GetGCKindSlots(kind) - js::ObjectElements::VALUES_PER_HEADER;

    JSObject *obj = js_NewGCObject(cx, kind);
    if (!obj)
        return NULL;

    obj->shape_.init(shape);
    obj->type_.init(type);
    obj->slots = NULL;
    obj->setFixedElements();
    new (obj->getElementsHeader()) js::ObjectElements(capacity, length);

    return obj;
}

inline void
JSObject::finish(JSContext *cx)
{
    if (hasDynamicSlots())
        cx->free_(slots);
    if (hasDynamicElements())
        cx->free_(getElementsHeader());
}

inline bool
JSObject::hasProperty(JSContext *cx, jsid id, bool *foundp, unsigned flags)
{
    JSObject *pobj;
    JSProperty *prop;
    JSAutoResolveFlags rf(cx, flags);
    if (!lookupGeneric(cx, id, &pobj, &prop))
        return false;
    *foundp = !!prop;
    return true;
}

inline bool
JSObject::isCallable()
{
    return isFunction() || getClass()->call;
}

inline JSPrincipals *
JSObject::principals(JSContext *cx)
{
    JSSecurityCallbacks *cb = JS_GetSecurityCallbacks(cx);
    if (JSObjectPrincipalsFinder finder = cb ? cb->findObjectPrincipals : NULL)
        return finder(cx, this);
    return cx->compartment ? cx->compartment->principals : NULL;
}

inline uint32_t
JSObject::slotSpan() const
{
    if (inDictionaryMode())
        return lastProperty()->base()->slotSpan();
    return lastProperty()->slotSpan();
}

inline js::HeapSlot &
JSObject::nativeGetSlotRef(unsigned slot)
{
    JS_ASSERT(isNative());
    JS_ASSERT(slot < slotSpan());
    return getSlotRef(slot);
}

inline const js::Value &
JSObject::nativeGetSlot(unsigned slot) const
{
    JS_ASSERT(isNative());
    JS_ASSERT(slot < slotSpan());
    return getSlot(slot);
}

inline JSFunction *
JSObject::nativeGetMethod(const js::Shape *shape) const
{
    /*
     * For method shapes, this object must have an uncloned function object in
     * the shape's slot.
     */
    JS_ASSERT(shape->isMethod());
#ifdef DEBUG
    JSObject *obj = &nativeGetSlot(shape->slot()).toObject();
    JS_ASSERT(obj->isFunction() && !obj->toFunction()->isClonedMethod());
#endif

    return static_cast<JSFunction *>(&nativeGetSlot(shape->slot()).toObject());
}

inline void
JSObject::nativeSetSlot(unsigned slot, const js::Value &value)
{
    JS_ASSERT(isNative());
    JS_ASSERT(slot < slotSpan());
    return setSlot(slot, value);
}

inline void
JSObject::nativeSetSlotWithType(JSContext *cx, const js::Shape *shape, const js::Value &value)
{
    nativeSetSlot(shape->slot(), value);
    js::types::AddTypePropertyId(cx, this, shape->propid(), value);
}

inline bool
JSObject::nativeContains(JSContext *cx, jsid id)
{
    return nativeLookup(cx, id) != NULL;
}

inline bool
JSObject::nativeContains(JSContext *cx, const js::Shape &shape)
{
    return nativeLookup(cx, shape.propid()) == &shape;
}

inline bool
JSObject::nativeEmpty() const
{
    return lastProperty()->isEmptyShape();
}

inline uint32_t
JSObject::propertyCount() const
{
    return lastProperty()->entryCount();
}

inline bool
JSObject::hasPropertyTable() const
{
    return lastProperty()->hasTable();
}

inline size_t
JSObject::computedSizeOfThisSlotsElements() const
{
    size_t n = sizeOfThis();

    if (hasDynamicSlots())
        n += numDynamicSlots() * sizeof(js::Value);

    if (hasDynamicElements())
        n += (js::ObjectElements::VALUES_PER_HEADER + getElementsHeader()->capacity) *
             sizeof(js::Value);

    return n;
}

inline void
JSObject::sizeOfExcludingThis(JSMallocSizeOfFun mallocSizeOf,
                              size_t *slotsSize, size_t *elementsSize,
                              size_t *miscSize) const
{
    *slotsSize = 0;
    if (hasDynamicSlots()) {
        *slotsSize += mallocSizeOf(slots);
    }

    *elementsSize = 0;
    if (hasDynamicElements()) {
        *elementsSize += mallocSizeOf(getElementsHeader());
    }

    /* Other things may be measured in the future if DMD indicates it is worthwhile. */
    *miscSize = 0;
    if (isFunction()) {
        *miscSize += toFunction()->sizeOfMisc(mallocSizeOf);
    } else if (isArguments()) {
        *miscSize += asArguments().sizeOfMisc(mallocSizeOf);
    } else if (isRegExpStatics()) {
        *miscSize += js::SizeOfRegExpStaticsData(this, mallocSizeOf);
    }
}

inline JSBool
JSObject::lookupGeneric(JSContext *cx, jsid id, JSObject **objp, JSProperty **propp)
{
    js::LookupGenericOp op = getOps()->lookupGeneric;
    return (op ? op : js_LookupProperty)(cx, this, id, objp, propp);
}

inline JSBool
JSObject::lookupProperty(JSContext *cx, js::PropertyName *name, JSObject **objp, JSProperty **propp)
{
    return lookupGeneric(cx, ATOM_TO_JSID(name), objp, propp);
}

inline JSBool
JSObject::defineGeneric(JSContext *cx, jsid id, const js::Value &value,
                        JSPropertyOp getter /* = JS_PropertyStub */,
                        JSStrictPropertyOp setter /* = JS_StrictPropertyStub */,
                        unsigned attrs /* = JSPROP_ENUMERATE */)
{
    js::DefineGenericOp op = getOps()->defineGeneric;
    return (op ? op : js_DefineProperty)(cx, this, id, &value, getter, setter, attrs);
}

inline JSBool
JSObject::defineProperty(JSContext *cx, js::PropertyName *name, const js::Value &value,
                        JSPropertyOp getter /* = JS_PropertyStub */,
                        JSStrictPropertyOp setter /* = JS_StrictPropertyStub */,
                        unsigned attrs /* = JSPROP_ENUMERATE */)
{
    return defineGeneric(cx, ATOM_TO_JSID(name), value, getter, setter, attrs);
}

inline JSBool
JSObject::defineElement(JSContext *cx, uint32_t index, const js::Value &value,
                        JSPropertyOp getter /* = JS_PropertyStub */,
                        JSStrictPropertyOp setter /* = JS_StrictPropertyStub */,
                        unsigned attrs /* = JSPROP_ENUMERATE */)
{
    js::DefineElementOp op = getOps()->defineElement;
    return (op ? op : js_DefineElement)(cx, this, index, &value, getter, setter, attrs);
}

inline JSBool
JSObject::defineSpecial(JSContext *cx, js::SpecialId sid, const js::Value &value,
                        JSPropertyOp getter /* = JS_PropertyStub */,
                        JSStrictPropertyOp setter /* = JS_StrictPropertyStub */,
                        unsigned attrs /* = JSPROP_ENUMERATE */)
{
    return defineGeneric(cx, SPECIALID_TO_JSID(sid), value, getter, setter, attrs);
}

inline JSBool
JSObject::lookupElement(JSContext *cx, uint32_t index, JSObject **objp, JSProperty **propp)
{
    js::LookupElementOp op = getOps()->lookupElement;
    return (op ? op : js_LookupElement)(cx, this, index, objp, propp);
}

inline JSBool
JSObject::lookupSpecial(JSContext *cx, js::SpecialId sid, JSObject **objp, JSProperty **propp)
{
    return lookupGeneric(cx, SPECIALID_TO_JSID(sid), objp, propp);
}

inline JSBool
JSObject::getElement(JSContext *cx, JSObject *receiver, uint32_t index, js::Value *vp)
{
    js::ElementIdOp op = getOps()->getElement;
    if (op)
        return op(cx, this, receiver, index, vp);

    jsid id;
    if (!js::IndexToId(cx, index, &id))
        return false;
    return getGeneric(cx, receiver, id, vp);
}

inline JSBool
JSObject::getElement(JSContext *cx, uint32_t index, js::Value *vp)
{
    return getElement(cx, this, index, vp);
}

inline JSBool
JSObject::getElementIfPresent(JSContext *cx, JSObject *receiver, uint32_t index, js::Value *vp,
                              bool *present)
{
    js::ElementIfPresentOp op = getOps()->getElementIfPresent;
    if (op)
        return op(cx, this, receiver, index, vp, present);

    /* For now, do the index-to-id conversion just once, then use
     * lookupGeneric/getGeneric.  Once lookupElement and getElement stop both
     * doing index-to-id conversions, we can use those here.
     */
    jsid id;
    if (!js::IndexToId(cx, index, &id))
        return false;

    JSObject *obj2;
    JSProperty *prop;
    if (!lookupGeneric(cx, id, &obj2, &prop))
        return false;

    if (!prop) {
        *present = false;
        js::Debug_SetValueRangeToCrashOnTouch(vp, 1);
        return true;
    }

    *present = true;
    return getGeneric(cx, receiver, id, vp);
}

inline JSBool
JSObject::getSpecial(JSContext *cx, JSObject *receiver, js::SpecialId sid, js::Value *vp)
{
    return getGeneric(cx, receiver, SPECIALID_TO_JSID(sid), vp);
}

inline JSBool
JSObject::getGenericAttributes(JSContext *cx, jsid id, unsigned *attrsp)
{
    js::GenericAttributesOp op = getOps()->getGenericAttributes;
    return (op ? op : js_GetAttributes)(cx, this, id, attrsp);
}

inline JSBool
JSObject::getPropertyAttributes(JSContext *cx, js::PropertyName *name, unsigned *attrsp)
{
    return getGenericAttributes(cx, ATOM_TO_JSID(name), attrsp);
}

inline JSBool
JSObject::getElementAttributes(JSContext *cx, uint32_t index, unsigned *attrsp)
{
    jsid id;
    if (!js::IndexToId(cx, index, &id))
        return false;
    return getGenericAttributes(cx, id, attrsp);
}

inline JSBool
JSObject::getSpecialAttributes(JSContext *cx, js::SpecialId sid, unsigned *attrsp)
{
    return getGenericAttributes(cx, SPECIALID_TO_JSID(sid), attrsp);
}

inline bool
JSObject::isProxy() const
{
    return js::IsProxy(this);
}

inline bool
JSObject::isCrossCompartmentWrapper() const
{
    return js::IsCrossCompartmentWrapper(this);
}

inline bool
JSObject::isWrapper() const
{
    return js::IsWrapper(this);
}

inline js::GlobalObject &
JSObject::global() const
{
    JSObject *obj = const_cast<JSObject *>(this);
    while (JSObject *parent = obj->getParent())
        obj = parent;
    return obj->asGlobal();
}

static inline bool
js_IsCallable(const js::Value &v)
{
    return v.isObject() && v.toObject().isCallable();
}

namespace js {

inline void
OBJ_TO_INNER_OBJECT(JSContext *cx, JSObject *&obj)
{
    if (JSObjectOp op = obj->getClass()->ext.innerObject)
        obj = op(cx, obj);
}

inline void
OBJ_TO_OUTER_OBJECT(JSContext *cx, JSObject *&obj)
{
    if (JSObjectOp op = obj->getClass()->ext.outerObject)
        obj = op(cx, obj);
}

/*
 * Methods to test whether an object or a value is of type "xml" (per typeof).
 */

#define VALUE_IS_XML(v)      (!JSVAL_IS_PRIMITIVE(v) && JSVAL_TO_OBJECT(v)->isXML())

static inline bool
IsXML(const js::Value &v)
{
    return v.isObject() && v.toObject().isXML();
}

static inline bool
IsStopIteration(const js::Value &v)
{
    return v.isObject() && v.toObject().isStopIteration();
}

/* ES5 9.1 ToPrimitive(input). */
static JS_ALWAYS_INLINE bool
ToPrimitive(JSContext *cx, Value *vp)
{
    if (vp->isPrimitive())
        return true;
    return vp->toObject().defaultValue(cx, JSTYPE_VOID, vp);
}

/* ES5 9.1 ToPrimitive(input, PreferredType). */
static JS_ALWAYS_INLINE bool
ToPrimitive(JSContext *cx, JSType preferredType, Value *vp)
{
    JS_ASSERT(preferredType != JSTYPE_VOID); /* Use the other ToPrimitive! */
    if (vp->isPrimitive())
        return true;
    return vp->toObject().defaultValue(cx, preferredType, vp);
}

/*
 * Return true if this is a compiler-created internal function accessed by
 * its own object. Such a function object must not be accessible to script
 * or embedding code.
 */
inline bool
IsInternalFunctionObject(JSObject *funobj)
{
    JSFunction *fun = funobj->toFunction();
    return (fun->flags & JSFUN_LAMBDA) && !funobj->getParent();
}

class AutoPropDescArrayRooter : private AutoGCRooter
{
  public:
    AutoPropDescArrayRooter(JSContext *cx)
      : AutoGCRooter(cx, DESCRIPTORS), descriptors(cx)
    { }

    PropDesc *append() {
        if (!descriptors.append(PropDesc()))
            return NULL;
        return &descriptors.back();
    }

    PropDesc& operator[](size_t i) {
        JS_ASSERT(i < descriptors.length());
        return descriptors[i];
    }

    friend void AutoGCRooter::trace(JSTracer *trc);

  private:
    PropDescArray descriptors;
};

class AutoPropertyDescriptorRooter : private AutoGCRooter, public PropertyDescriptor
{
  public:
    AutoPropertyDescriptorRooter(JSContext *cx) : AutoGCRooter(cx, DESCRIPTOR) {
        obj = NULL;
        attrs = 0;
        getter = (PropertyOp) NULL;
        setter = (StrictPropertyOp) NULL;
        value.setUndefined();
    }

    AutoPropertyDescriptorRooter(JSContext *cx, PropertyDescriptor *desc)
      : AutoGCRooter(cx, DESCRIPTOR)
    {
        obj = desc->obj;
        attrs = desc->attrs;
        getter = desc->getter;
        setter = desc->setter;
        value = desc->value;
    }

    friend void AutoGCRooter::trace(JSTracer *trc);
};

inline bool
NewObjectCache::lookup(Class *clasp, gc::Cell *key, gc::AllocKind kind, EntryIndex *pentry)
{
    uintptr_t hash = (uintptr_t(clasp) ^ uintptr_t(key)) + kind;
    *pentry = hash % js::ArrayLength(entries);

    Entry *entry = &entries[*pentry];

    /* N.B. Lookups with the same clasp/key but different kinds map to different entries. */
    return (entry->clasp == clasp && entry->key == key);
}

inline bool
NewObjectCache::lookupProto(Class *clasp, JSObject *proto, gc::AllocKind kind, EntryIndex *pentry)
{
    JS_ASSERT(!proto->isGlobal());
    return lookup(clasp, proto, kind, pentry);
}

inline bool
NewObjectCache::lookupGlobal(Class *clasp, js::GlobalObject *global, gc::AllocKind kind, EntryIndex *pentry)
{
    return lookup(clasp, global, kind, pentry);
}

inline bool
NewObjectCache::lookupType(Class *clasp, js::types::TypeObject *type, gc::AllocKind kind, EntryIndex *pentry)
{
    return lookup(clasp, type, kind, pentry);
}

inline void
NewObjectCache::fill(EntryIndex entry_, Class *clasp, gc::Cell *key, gc::AllocKind kind, JSObject *obj)
{
    JS_ASSERT(unsigned(entry_) < ArrayLength(entries));
    Entry *entry = &entries[entry_];

    JS_ASSERT(!obj->hasDynamicSlots() && !obj->hasDynamicElements());

    entry->clasp = clasp;
    entry->key = key;
    entry->kind = kind;

    entry->nbytes = obj->sizeOfThis();
    js_memcpy(&entry->templateObject, obj, entry->nbytes);
}

inline void
NewObjectCache::fillProto(EntryIndex entry, Class *clasp, JSObject *proto, gc::AllocKind kind, JSObject *obj)
{
    JS_ASSERT(!proto->isGlobal());
    JS_ASSERT(obj->getProto() == proto);
    return fill(entry, clasp, proto, kind, obj);
}

inline void
NewObjectCache::fillGlobal(EntryIndex entry, Class *clasp, js::GlobalObject *global, gc::AllocKind kind, JSObject *obj)
{
    //JS_ASSERT(global == obj->getGlobal());
    return fill(entry, clasp, global, kind, obj);
}

inline void
NewObjectCache::fillType(EntryIndex entry, Class *clasp, js::types::TypeObject *type, gc::AllocKind kind, JSObject *obj)
{
    JS_ASSERT(obj->type() == type);
    return fill(entry, clasp, type, kind, obj);
}

inline void
NewObjectCache::copyCachedToObject(JSObject *dst, JSObject *src)
{
    js_memcpy(dst, src, dst->sizeOfThis());
#ifdef JSGC_GENERATIONAL
    Shape::writeBarrierPost(dst->shape_, &dst->shape_);
    types::TypeObject::writeBarrierPost(dst->type_, &dst->type_);
#endif
}

inline JSObject *
NewObjectCache::newObjectFromHit(JSContext *cx, EntryIndex entry_)
{
    JS_ASSERT(unsigned(entry_) < ArrayLength(entries));
    Entry *entry = &entries[entry_];

    JSObject *obj = js_TryNewGCObject(cx, entry->kind);
    if (obj) {
        copyCachedToObject(obj, &entry->templateObject);
        Probes::createObject(cx, obj);
        return obj;
    }

    /* Copy the entry to the stack first in case it is purged by a GC. */
    size_t nbytes = entry->nbytes;
    char stackObject[sizeof(JSObject_Slots16)];
    JS_ASSERT(nbytes <= sizeof(stackObject));
    js_memcpy(&stackObject, &entry->templateObject, nbytes);

    JSObject *baseobj = (JSObject *) stackObject;
    RootShape shapeRoot(cx, (Shape **) baseobj->addressOfShape());
    RootTypeObject typeRoot(cx, (types::TypeObject **) baseobj->addressOfType());

    obj = js_NewGCObject(cx, entry->kind);
    if (obj) {
        copyCachedToObject(obj, baseobj);
        Probes::createObject(cx, obj);
        return obj;
    }

    return NULL;
}

static inline bool
CanBeFinalizedInBackground(gc::AllocKind kind, Class *clasp)
{
#ifdef JS_THREADSAFE
    JS_ASSERT(kind <= gc::FINALIZE_OBJECT_LAST);
    /* If the class has no finalizer or a finalizer that is safe to call on
     * a different thread, we change the finalize kind. For example,
     * FINALIZE_OBJECT0 calls the finalizer on the main thread,
     * FINALIZE_OBJECT0_BACKGROUND calls the finalizer on the gcHelperThread.
     * IsBackgroundAllocKind is called to prevent recursively incrementing
     * the finalize kind; kind may already be a background finalize kind.
     */
    if (!gc::IsBackgroundAllocKind(kind) && !clasp->finalize)
        return true;
#endif
    return false;
}

/*
 * Make an object with the specified prototype. If parent is null, it will
 * default to the prototype's global if the prototype is non-null.
 */
JSObject *
NewObjectWithGivenProto(JSContext *cx, js::Class *clasp, JSObject *proto, JSObject *parent,
                        gc::AllocKind kind);

inline JSObject *
NewObjectWithGivenProto(JSContext *cx, js::Class *clasp, JSObject *proto, JSObject *parent)
{
    gc::AllocKind kind = gc::GetGCObjectKind(clasp);
    return NewObjectWithGivenProto(cx, clasp, proto, parent, kind);
}

inline JSProtoKey
GetClassProtoKey(js::Class *clasp)
{
    JSProtoKey key = JSCLASS_CACHED_PROTO_KEY(clasp);
    if (key != JSProto_Null)
        return key;
    if (clasp->flags & JSCLASS_IS_ANONYMOUS)
        return JSProto_Object;
    return JSProto_Null;
}

inline bool
FindProto(JSContext *cx, js::Class *clasp, HandleObject parent, JSObject **proto)
{
    JSProtoKey protoKey = GetClassProtoKey(clasp);
    if (!js_GetClassPrototype(cx, parent, protoKey, proto, clasp))
        return false;
    if (!(*proto) && !js_GetClassPrototype(cx, parent, JSProto_Object, proto))
        return false;
    return true;
}

/*
 * Make an object with the prototype set according to the specified prototype or class:
 *
 * if proto is non-null:
 *   use the specified proto
 * for a built-in class:
 *   use the memoized original value of the class constructor .prototype
 *   property object
 * else if available
 *   the current value of .prototype
 * else
 *   Object.prototype.
 *
 * The class prototype will be fetched from the parent's global. If global is
 * null, the context's active global will be used, and the resulting object's
 * parent will be that global.
 */
JSObject *
NewObjectWithClassProto(JSContext *cx, js::Class *clasp, JSObject *proto, JSObject *parent,
                        gc::AllocKind kind);

inline JSObject *
NewObjectWithClassProto(JSContext *cx, js::Class *clasp, JSObject *proto, JSObject *parent)
{
    gc::AllocKind kind = gc::GetGCObjectKind(clasp);
    return NewObjectWithClassProto(cx, clasp, proto, parent, kind);
}

/*
 * Create a native instance of the given class with parent and proto set
 * according to the context's active global.
 */
inline JSObject *
NewBuiltinClassInstance(JSContext *cx, Class *clasp, gc::AllocKind kind)
{
    return NewObjectWithClassProto(cx, clasp, NULL, NULL, kind);
}

inline JSObject *
NewBuiltinClassInstance(JSContext *cx, Class *clasp)
{
    gc::AllocKind kind = gc::GetGCObjectKind(clasp);
    return NewBuiltinClassInstance(cx, clasp, kind);
}

inline GlobalObject *
GetCurrentGlobal(JSContext *cx)
{
    JSObject *scopeChain = (cx->hasfp()) ? &cx->fp()->scopeChain() : cx->globalObject;
    return scopeChain ? &scopeChain->global() : NULL;
}

bool
FindClassPrototype(JSContext *cx, JSObject *scope, JSProtoKey protoKey, JSObject **protop,
                   Class *clasp);

/*
 * Create a plain object with the specified type. This bypasses getNewType to
 * avoid losing creation site information for objects made by scripted 'new'.
 */
JSObject *
NewObjectWithType(JSContext *cx, types::TypeObject *type, JSObject *parent, gc::AllocKind kind);

/* Make an object with pregenerated shape from a NEWOBJECT bytecode. */
static inline JSObject *
CopyInitializerObject(JSContext *cx, JSObject *baseobj, types::TypeObject *type)
{
    JS_ASSERT(baseobj->getClass() == &ObjectClass);
    JS_ASSERT(!baseobj->inDictionaryMode());

    gc::AllocKind kind = gc::GetGCObjectFixedSlotsKind(baseobj->numFixedSlots());
#ifdef JS_THREADSAFE
    kind = gc::GetBackgroundAllocKind(kind);
#endif
    JS_ASSERT(kind == baseobj->getAllocKind());
    JSObject *obj = NewBuiltinClassInstance(cx, &ObjectClass, kind);

    if (!obj)
        return NULL;

    obj->setType(type);

    if (!obj->setLastProperty(cx, baseobj->lastProperty()))
        return NULL;

    return obj;
}

JSObject *
NewReshapedObject(JSContext *cx, js::types::TypeObject *type, JSObject *parent,
                  gc::AllocKind kind, const Shape *shape);

/*
 * As for gc::GetGCObjectKind, where numSlots is a guess at the final size of
 * the object, zero if the final size is unknown. This should only be used for
 * objects that do not require any fixed slots.
 */
static inline gc::AllocKind
GuessObjectGCKind(size_t numSlots)
{
    if (numSlots)
        return gc::GetGCObjectKind(numSlots);
    return gc::FINALIZE_OBJECT4;
}

static inline gc::AllocKind
GuessArrayGCKind(size_t numSlots)
{
    if (numSlots)
        return gc::GetGCArrayKind(numSlots);
    return gc::FINALIZE_OBJECT8;
}

/*
 * Get the GC kind to use for scripted 'new' on the given class.
 * FIXME bug 547327: estimate the size from the allocation site.
 */
static inline gc::AllocKind
NewObjectGCKind(JSContext *cx, js::Class *clasp)
{
    if (clasp == &ArrayClass || clasp == &SlowArrayClass)
        return gc::FINALIZE_OBJECT8;
    if (clasp == &FunctionClass)
        return gc::FINALIZE_OBJECT2;
    return gc::FINALIZE_OBJECT4;
}

/*
 * Fill slots with the initial slot array to use for a newborn object which
 * may or may not need dynamic slots.
 */
inline bool
PreallocateObjectDynamicSlots(JSContext *cx, Shape *shape, HeapSlot **slots)
{
    if (size_t count = JSObject::dynamicSlotsCount(shape->numFixedSlots(), shape->slotSpan())) {
        *slots = (HeapSlot *) cx->malloc_(count * sizeof(HeapSlot));
        if (!*slots)
            return false;
        Debug_SetSlotRangeToCrashOnTouch(*slots, count);
        return true;
    }
    *slots = NULL;
    return true;
}

inline bool
DefineConstructorAndPrototype(JSContext *cx, GlobalObject *global,
                              JSProtoKey key, JSObject *ctor, JSObject *proto)
{
    JS_ASSERT(!global->nativeEmpty()); /* reserved slots already allocated */
    JS_ASSERT(ctor);
    JS_ASSERT(proto);

    jsid id = ATOM_TO_JSID(cx->runtime->atomState.classAtoms[key]);
    JS_ASSERT(!global->nativeLookup(cx, id));

    /* Set these first in case AddTypePropertyId looks for this class. */
    global->setSlot(key, ObjectValue(*ctor));
    global->setSlot(key + JSProto_LIMIT, ObjectValue(*proto));
    global->setSlot(key + JSProto_LIMIT * 2, ObjectValue(*ctor));

    types::AddTypePropertyId(cx, global, id, ObjectValue(*ctor));
    if (!global->addDataProperty(cx, id, key + JSProto_LIMIT * 2, 0)) {
        global->setSlot(key, UndefinedValue());
        global->setSlot(key + JSProto_LIMIT, UndefinedValue());
        global->setSlot(key + JSProto_LIMIT * 2, UndefinedValue());
        return false;
    }

    return true;
}

bool
PropDesc::checkGetter(JSContext *cx)
{
    if (hasGet && !js_IsCallable(get) && !get.isUndefined()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_GET_SET_FIELD,
                             js_getter_str);
        return false;
    }
    return true;
}

bool
PropDesc::checkSetter(JSContext *cx)
{
    if (hasSet && !js_IsCallable(set) && !set.isUndefined()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_GET_SET_FIELD,
                             js_setter_str);
        return false;
    }
    return true;
}

inline bool
ObjectClassIs(JSObject &obj, ESClassValue classValue, JSContext *cx)
{
    if (JS_UNLIKELY(obj.isProxy()))
        return Proxy::objectClassIs(&obj, classValue, cx);

    switch (classValue) {
      case ESClass_Array: return obj.isArray();
      case ESClass_Number: return obj.isNumber();
      case ESClass_String: return obj.isString();
      case ESClass_Boolean: return obj.isBoolean();
      case ESClass_RegExp: return obj.isRegExp();
    }
    JS_NOT_REACHED("bad classValue");
    return false;
}

inline bool
IsObjectWithClass(const Value &v, ESClassValue classValue, JSContext *cx)
{
    if (!v.isObject())
        return false;
    return ObjectClassIs(v.toObject(), classValue, cx);
}

static JS_ALWAYS_INLINE bool
ValueIsSpecial(JSObject *obj, Value *propval, SpecialId *sidp, JSContext *cx)
{
    if (!propval->isObject())
        return false;

#if JS_HAS_XML_SUPPORT
    if (obj->isXML()) {
        *sidp = SpecialId(propval->toObject());
        return true;
    }

    JSObject &propobj = propval->toObject();
    JSAtom *name;
    if (propobj.isQName() && GetLocalNameFromFunctionQName(&propobj, &name, cx)) {
        propval->setString(name);
        return false;
    }
#endif

    return false;
}

JSObject *
DefineConstructorAndPrototype(JSContext *cx, HandleObject obj, JSProtoKey key, HandleAtom atom,
                              JSObject *protoProto, Class *clasp,
                              Native constructor, unsigned nargs,
                              JSPropertySpec *ps, JSFunctionSpec *fs,
                              JSPropertySpec *static_ps, JSFunctionSpec *static_fs,
                              JSObject **ctorp = NULL,
                              gc::AllocKind ctorKind = JSFunction::FinalizeKind);

} /* namespace js */

extern JSObject *
js_InitClass(JSContext *cx, js::HandleObject obj, JSObject *parent_proto,
             js::Class *clasp, JSNative constructor, unsigned nargs,
             JSPropertySpec *ps, JSFunctionSpec *fs,
             JSPropertySpec *static_ps, JSFunctionSpec *static_fs,
             JSObject **ctorp = NULL,
             js::gc::AllocKind ctorKind = JSFunction::FinalizeKind);

inline JSObject *
js_GetProtoIfDenseArray(JSObject *obj)
{
    return obj->isDenseArray() ? obj->getProto() : obj;
}

/*
 * js_PurgeScopeChain does nothing if obj is not itself a prototype or parent
 * scope, else it reshapes the scope and prototype chains it links. It calls
 * js_PurgeScopeChainHelper, which asserts that obj is flagged as a delegate
 * (i.e., obj has ever been on a prototype or parent chain).
 */
extern bool
js_PurgeScopeChainHelper(JSContext *cx, JSObject *obj, jsid id);

inline bool
js_PurgeScopeChain(JSContext *cx, JSObject *obj, jsid id)
{
    if (obj->isDelegate())
        return js_PurgeScopeChainHelper(cx, obj, id);
    return true;
}

inline void
JSObject::setSlot(unsigned slot, const js::Value &value)
{
    JS_ASSERT(slotInRange(slot));
    getSlotRef(slot).set(this, slot, value);
}

inline void
JSObject::initSlot(unsigned slot, const js::Value &value)
{
    JS_ASSERT(getSlot(slot).isUndefined() || getSlot(slot).isMagic(JS_ARRAY_HOLE));
    JS_ASSERT(slotInRange(slot));
    initSlotUnchecked(slot, value);
}

inline void
JSObject::initSlotUnchecked(unsigned slot, const js::Value &value)
{
    getSlotAddressUnchecked(slot)->init(this, slot, value);
}

inline void
JSObject::setFixedSlot(unsigned slot, const js::Value &value)
{
    JS_ASSERT(slot < numFixedSlots());
    fixedSlots()[slot].set(this, slot, value);
}

inline void
JSObject::initFixedSlot(unsigned slot, const js::Value &value)
{
    JS_ASSERT(slot < numFixedSlots());
    fixedSlots()[slot].init(this, slot, value);
}

#endif /* jsobjinlines_h___ */
