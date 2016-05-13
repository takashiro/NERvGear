/*
 * VCircularQueueSync.h
 *
 *  Created on: 2016年5月11日
 *      Author: yangkai
 */

#pragma once

#include <VMutex.h>

NV_NAMESPACE_BEGIN

template <class E>
class VCircularQueueSync
{
public:
    VCircularQueueSync(uint capacity = 500)
        : m_capacity(capacity)
        , m_size(0)
        , m_front(0)
        , m_back(0)
        , m_data(new E[m_capacity])
    {
    }

    ~VCircularQueueSync()
    {
        delete[] m_data;
    }

    uint capacity() const { return m_capacity; }
    uint size() const { return m_size; }

    void append(const E &element)
    {
        m_data[m_back] = element;
        forward();
    }

    void append(E &&element)
    {
        m_data[m_back] = element;
        forward();
    }

    void prepend(const E &element)
    {
        backward();
        m_data[m_front] = element;
    }

    void prepend(E &&element)
    {
        backward();
        m_data[m_front] = element;
    }

    const E &at(uint index) const { return m_data[(m_front + index) % m_capacity]; }
    E &operator[](uint index) { return m_data[(m_front + index) % m_capacity]; }
    const E &operator[] (uint index) const { return m_data[(m_front + index) % m_capacity]; }

    void clear()
    {
        m_front = m_back = 0;
        m_size = 0;
    }

private:
    void forward()
    {
        m_back = (m_back + 1) % m_capacity;
        if (m_size >= m_capacity) {
            m_front = (m_front + 1) % m_capacity;
        } else {
            m_size++;
        }
    }

    void backward()
    {
        m_front = m_front ? m_front - 1 : m_capacity - 1;
        if (m_size >= m_capacity) {
            m_back = m_back ? m_back - 1 : m_capacity - 1;
        } else {
            m_size++;
        }
    }

    uint m_capacity;
    uint m_size;
    uint m_front;
    uint m_back;
    E *m_data;
};

NV_NAMESPACE_END
