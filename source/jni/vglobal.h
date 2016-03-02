#pragma once

#ifdef NV_NAMESPACE
# define NV_NAMESPACE_BEGIN namespace NV_NAMESPACE {
# define NV_NAMESPACE_END }
# define NV_USING_NAMESPACE using namespace NV_NAMESPACE;
#else
# define NV_NAMESPACE_BEGIN
# define NV_NAMESPACE_END
# define NV_USING_NAMESPACE
#endif

typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned long long ulonglong;

#define forever for(;;)
