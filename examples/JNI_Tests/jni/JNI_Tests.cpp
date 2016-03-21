/*
 * JNI_Tests.cpp
 *
 *  Created on: 2016年3月9日
 *      Author: yangkai
 */
#include <iostream>
#include <string>
#include <list>
#include "com_example_jni_tests_MainActivity.h"
#include "VList.h"
#include "logcat_cpp.h"

#define LOG_TAG "JNI_Tests"
#define LOGD(...) _LOGD(LOG_TAG, __VA_ARGS__)
using namespace std;
using namespace NervGear;

class StrNode:public NodeOfVList<VList<StrNode>>
{
public:
    string* pstr;//需要自己释放
    StrNode(string *p):pstr(p)
    {

    }
    bool operator == (const StrNode& a)
    {
        return *(a.pstr) == *(this->pstr);
    }
};
void testVList()
{
    VList<StrNode> tester;
    int count = 0;
    LOGD("%d.isEmpty():1 == %d\n", count++, tester.isEmpty());
    tester.append(StrNode(new string("tester2")));
    tester.append(StrNode(new string("tester3")));
    tester.prepend(StrNode(new string("tester1")));
    tester.prepend(StrNode(new string("tester0")));
    LOGD("%d.isEmpty():0 == %d\n", count++, tester.isEmpty());

    LOGD("%d.size():4 == %d", count++, tester.size());

    LOGD("%d.first():tester0 == %s\n", count++, tester.first().pstr->c_str());
    LOGD("%d.last():tester3 == %s\n", count++, tester.last().pstr->c_str());

    VList<StrNode>::iterator iter0 = tester.end();
    iter0--;

    LOGD("%d.isFirst():1 == %d\n", count++, tester.isFirst(tester.begin()));
    LOGD("%d.isFirst():0 == %d\n", count++, tester.isFirst(iter0));
    LOGD("%d.isLast():0 == %d\n", count++, tester.isLast(tester.begin()));
    LOGD("%d.isLast():1 == %d\n", count++, tester.isLast(iter0));

    tester.bringToFront(iter0);
    LOGD("%d.bringToFront():tester3 == %s\n", count++, tester.first().pstr->c_str());

    tester.sendToBack(tester.begin());
    LOGD("%d.sendToBack():tester3 == %s\n", count++, tester.last().pstr->c_str());

    LOGD("%d.contains():1 == %d\n", count++, tester.contains(StrNode(new string("tester2"))));
    LOGD("%d.contains():0 == %d\n", count++, tester.contains(StrNode(new string("tester4"))));

    StrNode &p = tester.front();
    p.pointToVList->removeElementByPointer(&p);
    LOGD("%d.removeElementByPointer():tester1 == %s\n", count++, tester.front().pstr->c_str());

    VList<StrNode>::iterator iter = tester.begin();
    iter++;
    StrNode* ptr = &(*iter);
    ptr->pointToVList->removeElementByPointer(ptr);
    LOGD("%d.removeElementByPointer():tester3 == %s\n", count++, tester.back().pstr->c_str());

    LOGD("%d.size():2 == %d\n", count++, tester.size());
}

JNIEXPORT jstring JNICALL Java_com_example_jni_1tests_MainActivity_runTests
  (JNIEnv *env, jobject obj)
{
    testVList();
    return env->NewStringUTF("It works!");
}


