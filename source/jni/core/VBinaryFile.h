#pragma once

#include "vglobal.h"
#include "VString.h"
//#include "VArray.h"
#include "Array.h"

#include <sstream>

/*
        This is a simple helper class to read binary data next to a JSON file.
*/

NV_NAMESPACE_BEGIN

class VBinaryFile
{
public:
    VBinaryFile( const uchar * bData, const int bSize )
        : m_data(bData)
        , m_size(bSize)
        , m_offset(0)
        , m_allocated(false)
    {

    }
    ~VBinaryFile();

    //        VBinaryReader( const char * path, const char ** perror );
    VBinaryFile(const VString path, const char **error);

    uint readUint() const
    {
        const int byteNum = sizeof(uint);
        if (m_data == nullptr || byteNum > m_size - m_offset) {
            return 0;
        }
        m_offset += byteNum;
        return *(uint *)(m_data + m_offset - byteNum);
    }

    template< typename _type_ >
    bool readArray( Array< _type_ > & oArray, const int num ) const
    {
        const int byteNum = num * sizeof(oArray[0]);
        if (m_data == nullptr || byteNum > m_size - m_offset) {
            oArray.resize(0);
            return false;
        }
        oArray.resize(num);
        memcpy(&oArray[0], &m_data[m_offset], byteNum);

        m_offset += byteNum;
        return true;
    }

    bool isEnd() const
    {
        return (m_offset == m_size);
    }

private:
    const uchar* m_data;
    int m_size;
    mutable int	m_offset;
    bool m_allocated;
};

NV_NAMESPACE_END


