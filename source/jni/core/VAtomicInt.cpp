/*
 * VAtomicInt.cpp
 *
 *  Created on: 2016年3月18日
 *      Author: yangkai
 */
#include "VAtomicInt.h"
NV_NAMESPACE_BEGIN

VAtomicInt::Type VAtomicInt::operator *= (VAtomicInt::Type argument)
{
    VAtomicInt::Type newValue, oldValue = load();
    do
    {
        newValue = oldValue * argument;
    }while(compare_exchange_weak(oldValue, newValue));
    return newValue;
}

VAtomicInt::Type VAtomicInt::operator /= (VAtomicInt::Type argument)
{
    VAtomicInt::Type newValue, oldValue = load();
    do
    {
        newValue = oldValue / argument;
    }while(compare_exchange_weak(oldValue, newValue));
    return newValue;
}

VAtomicInt::Type VAtomicInt::operator >>= (unsigned bits)
{
    VAtomicInt::Type newValue, oldValue = load();
    do
    {
        newValue = oldValue >> bits;
    }while(compare_exchange_weak(oldValue, newValue));
    return newValue;
}

VAtomicInt::Type VAtomicInt::operator <<= (unsigned bits)
{
    VAtomicInt::Type newValue, oldValue = load();
    do
    {
        newValue = oldValue << bits;
    }while(compare_exchange_weak(oldValue, newValue));
    return newValue;
}
NV_NAMESPACE_END



