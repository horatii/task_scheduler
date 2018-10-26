#include <stdio.h>

#include "task_scheduler.h"

#include <initguid.h>
#include <mstask.h>
#include <taskschd.h>
// #include <security.h>

#include <atlbase.h>
#include <atlpath.h>


int main(int argc, char* argv[])
{
    ::CoInitialize(NULL);
    ::CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, 0, NULL);

    
    CComPtr<ITaskScheduler> Scheduler;
    Scheduler.CoCreateInstance(CLSID_CTaskScheduler, NULL, CLSCTX_INPROC_SERVER);

    Scheduler->Delete();
    Scheduler->NewWorkItem();

    Scheduler->Activate();
    //TaskScheduler* task_scheduler = CraateTaskScheduler();
    //task_scheduler->Initilize();

    //TaskScheduler::TaskInfo info;
    //task_scheduler->GetTaskInfo(L"GoogleUpdateTaskMachineCore", &info);

    //task_scheduler->RegisterTask(L"TEST", L"TEST", L"C:\\cmd.exe", L"-c pause", TaskScheduler::TRIGGER_TYPE_EVERY_SIX_HOURS, false);

    //task_scheduler->UnInitilize();

    
    int b = 0;
    return 0;
}