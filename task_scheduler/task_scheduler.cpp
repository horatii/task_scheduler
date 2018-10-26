#include "task_scheduler.h"

#include <initguid.h>
#include <mstask.h>
#include <taskschd.h>
// #include <security.h>

#include <atlbase.h>
#include <atlpath.h>

#define SECURITY_WIN32
#include <Security.h>
#pragma comment(lib, "Secur32.lib")
#pragma comment(lib, "Mstask.lib")
#pragma comment(lib, "Taskschd.lib")

#include <vector>

const wchar_t kV2Library[] = L"taskschd.dll";

// Text for times used in the V2 API of the Task Scheduler.
const wchar_t kOneHourText[] = L"PT1H";
const wchar_t kSixHoursText[] = L"PT6H";
const wchar_t kZeroMinuteText[] = L"PT0M";
const wchar_t kFifteenMinutesText[] = L"PT15M";
const wchar_t kTwentyFourHoursText[] = L"PT24H";

const size_t kNumDeleteTaskRetry = 3;
const size_t kDeleteRetryDelayInMs = 100;

const VARIANT kEmptyVariant = { { { VT_EMPTY } } };

static void PinModule(const wchar_t* module_name) {
    // Force the DLL to stay loaded until program termination. We have seen
    // cases where it gets unloaded even though we still have references to
    // the objects we just CoCreated.
    HMODULE module_handle = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, module_name,
        &module_handle)) {
        // LOG(ERROR) << "Failed to pin '" << module_name << "'.";
    }
    if (module_handle) {
        FreeLibrary(module_handle);
    }
}

bool GetCurrentUser(CComBSTR& user_name) {
    ULONG user_name_size = 256;
    CStringW buffer;
//    user_name->
    if (!::GetUserNameExW(
        NameSamCompatible,
        buffer.GetBuffer(user_name_size),
        &user_name_size)) {
        if (::GetLastError() != ERROR_MORE_DATA) {
            return false;
        }
        if (!::GetUserNameExW(
            NameSamCompatible,
            buffer.GetBuffer(user_name_size),
            &user_name_size)) {
            return false;
        }
    }
    buffer.ReleaseBuffer();

    user_name.Empty();
    user_name.Append(buffer);

    return true;
}

//////////////////////////////////////////////////////////////////////////////////
class TaskSchedulerV2 : public TaskScheduler
{
public:
    TaskSchedulerV2() {

    }

    virtual bool Initilize() {
        HRESULT hr = ::CoCreateInstance(CLSID_TaskScheduler, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&task_service_));
        if (FAILED(hr)) {
            // LOG (ERROR) << "CreateInstance failed for CLSID_TaskScheduler."
            //             << std::hex << hr;
            return false;
        }


        hr = task_service_->Connect(kEmptyVariant,
                                    kEmptyVariant,
                                    kEmptyVariant,
                                    kEmptyVariant);
        if (FAILED(hr)) {
            // LOG (ERROR) << "Failed to connect to task service."
            //             << std::hex << hr;
            return false;
        }
        hr = task_service_->GetFolder(CComBSTR(L"\\"), &root_task_folder_);
        if (FAILED(hr)) {
            // LOG(ERROR) << "Can't get task service folder. " << std::hex << hr;
            return false;
        }
        PinModule(kV2Library);

        return true;
    }

    virtual bool UnInitilize() {
        task_service_.Release();
        root_task_folder_.Release();
        return true;
    }
    
    virtual bool DeleteTask(const wchar_t* task_name) {
        if (!root_task_folder_)
            return false;
        HRESULT hr = root_task_folder_->DeleteTask(CComBSTR(task_name), 0);

        size_t num_retries_left = kNumDeleteTaskRetry;
        if (FAILED(hr)) {
            while ((hr == HRESULT_FROM_WIN32(ERROR_TRANSACTION_NOT_ACTIVE) ||
                hr == HRESULT_FROM_WIN32(ERROR_TRANSACTION_ALREADY_ABORTED)) &&
                --num_retries_left && IsTaskRegistered(task_name)) {
                hr = root_task_folder_->DeleteTask(CComBSTR(task_name), 0);
                ::Sleep(kDeleteRetryDelayInMs);
            }
            if (!IsTaskRegistered(task_name))
                hr = S_OK;
        }

        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            // LOG(ERROR) << "Can't delete task. " << std::hex << hr;
            return false;
        }

        return true;
    }

    virtual bool IsTaskRegistered(const wchar_t* task_name) {
        if (!root_task_folder_)
            return false;
        return GetTask(task_name, nullptr);
    }

    // Enable or disable task based on the value of |enabled|. Return true if the
    // task exists and the operation succeeded.
    virtual bool SetTaskEnabled(const wchar_t* task_name, bool enabled) {
        if (!root_task_folder_)
            return false;

        CComPtr<IRegisteredTask> registered_task;
        if (!GetTask(task_name, &registered_task)) {
            return false;
        }

        HRESULT hr;
        hr = registered_task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE);
        if (FAILED(hr)) {
            return false;
        }
        return true;
    }

    // Return true if task exists and is enabled.
    virtual bool IsTaskEnabled(const wchar_t* task_name) {
        if (!root_task_folder_)
            return false;

        CComPtr<IRegisteredTask> registered_task;
        if (!GetTask(task_name, &registered_task))
            return false;

        HRESULT hr;
        VARIANT_BOOL is_enabled;
        hr = registered_task->get_Enabled(&is_enabled);
        if (FAILED(hr)) {
            return false;
        }
        return true;
    }

    // Return detailed information about a task. Return true if no errors were
    // encountered. On error, the struct is left unmodified.
    virtual bool GetTaskInfo(const wchar_t* task_name, TaskInfo* info) {
        if (!root_task_folder_)
            return false;

        CComPtr<IRegisteredTask> registered_task;
        if (!GetTask(task_name, &registered_task)) {
            return false;
        }

        // Collect information into internal storage to ensure that we start with
        // a clean slate and don't return partial results on error.
        TaskInfo info_storage;
        HRESULT hr =
            GetTaskDescription(registered_task, &info_storage.description);
        if (FAILED(hr)) {
            return false;
        }

        if (!GetTaskExecActions(registered_task,
            &info_storage.exec_actions)) {
            return false;
        }

        hr = GetTaskLogonType(registered_task, &info_storage.logon_type);
        if (FAILED(hr)) {
            return false;
        }
        info_storage.name = task_name;
        std::swap(*info, info_storage);
        return true;
    }

    // Return the description of the task.
    HRESULT GetTaskDescription(IRegisteredTask* task,
        CStringW* description) {

        CComBSTR task_name_bstr;
        HRESULT hr = task->get_Name(&task_name_bstr);
        if (FAILED(hr)) {
            return false;
        }

        CStringW task_name = CStringW(task_name_bstr ? task_name_bstr : L"");

        CComPtr<ITaskDefinition> task_info;
        hr = task->get_Definition(&task_info);
        if (FAILED(hr)) {
            return hr;
        }

        CComPtr<IRegistrationInfo> reg_info;
        hr = task_info->get_RegistrationInfo(&reg_info);
        if (FAILED(hr)) {
            return hr;
        }

        CComBSTR raw_description;
        hr = reg_info->get_Description(&raw_description);
        if (FAILED(hr)) {
            return hr;
        }
        *description =CStringW(raw_description ? raw_description : L"");

        return ERROR_SUCCESS;
    }

    // Return all executable actions associated with the given task. Non-exec
    // actions are silently ignored.
    bool GetTaskExecActions(IRegisteredTask* task,
        std::vector<TaskExecAction>* actions) {
        CComPtr<ITaskDefinition> task_definition;
        HRESULT hr = task->get_Definition(&task_definition);
        if (FAILED(hr)) {
            return false;
        }

        CComPtr<IActionCollection> action_collection;
        hr = task_definition->get_Actions(&action_collection);
        if (FAILED(hr)) {
            return false;
        }

        long actions_count = 0;  // NOLINT, API requires a long.
        hr = action_collection->get_Count(&actions_count);
        if (FAILED(hr)) {
            return false;
        }

        // Find and return as many exec actions as possible in |actions| and return
        // false if there were any errors on the way. Note that the indexing of
        // actions is 1-based.
        bool success = true;
        for (long action_index = 1;  // NOLINT
            action_index <= actions_count; ++action_index) {
            CComPtr<IAction> action;
            hr = action_collection->get_Item(action_index, &action);
            if (FAILED(hr)) {
                success = false;
                continue;
            }

            ::TASK_ACTION_TYPE action_type;
            hr = action->get_Type(&action_type);
            if (FAILED(hr)) {
                success = false;
                continue;
            }

            // We only care about exec actions for now. The other types are
            // TASK_ACTION_COM_HANDLER, TASK_ACTION_SEND_EMAIL,
            // TASK_ACTION_SHOW_MESSAGE. The latter two are marked as deprecated in
            // the Task Scheduler's GUI.
            if (action_type != ::TASK_ACTION_EXEC)
                continue;

            CComQIPtr<IExecAction> exec_action(action);;
            if (!exec_action) {
                success = false;
                continue;
            }

            CComBSTR application_path;
            hr = exec_action->get_Path(&application_path);
            if (FAILED(hr)) {
                success = false;
                continue;
            }

            CComBSTR working_dir;
            hr = exec_action->get_WorkingDirectory(&working_dir);
            if (FAILED(hr)) {
                success = false;
                continue;
            }

            CComBSTR parameters;
            hr = exec_action->get_Arguments(&parameters);
            if (FAILED(hr)) {
                success = false;
                continue;
            }

            actions->push_back(
            { CStringW(application_path ? application_path : L""),
                CStringW(working_dir ? working_dir : L""),
                CStringW(parameters ? parameters : L"") });
        }
        return success;
    }

    // Return the log-on type required for the task's actions to be run.
    HRESULT GetTaskLogonType(IRegisteredTask* task, uint32_t* logon_type) {
        CComPtr<ITaskDefinition> task_info;
        HRESULT hr = task->get_Definition(&task_info);
        if (FAILED(hr)) {
            return hr;
        }

        CComPtr<IPrincipal> principal;
        hr = task_info->get_Principal(&principal);
        if (FAILED(hr)) {
            return hr;
        }

        TASK_LOGON_TYPE raw_logon_type;
        hr = principal->get_LogonType(&raw_logon_type);
        if (FAILED(hr)) {
            return hr;
        }

        switch (raw_logon_type) {
        case TASK_LOGON_INTERACTIVE_TOKEN:
            *logon_type = LOGON_INTERACTIVE;
            break;
        case TASK_LOGON_GROUP:     // fall-thru
        case TASK_LOGON_PASSWORD:  // fall-thru
        case TASK_LOGON_SERVICE_ACCOUNT:
            *logon_type = LOGON_SERVICE;
            break;
        case TASK_LOGON_S4U:
            *logon_type = LOGON_SERVICE | LOGON_S4U;
            break;
        case TASK_LOGON_INTERACTIVE_TOKEN_OR_PASSWORD:
            *logon_type = LOGON_INTERACTIVE | LOGON_SERVICE;
            break;
        default:
            *logon_type = LOGON_UNKNOWN;
            break;
        }
        return ERROR_SUCCESS;
    }
    // Register the task to run the specified application and using the given
    // |trigger_type|.
    virtual bool RegisterTask(const wchar_t* task_name,
        const wchar_t* task_description,
        const wchar_t* application_path,
        const wchar_t* application_arguments,
        TriggerType trigger_type,
        bool hidden) {
        if (!DeleteTask(task_name))
            return false;

        // Create the task definition object to create the task.
        CComPtr<ITaskDefinition> task;
        HRESULT hr = task_service_->NewTask(0, &task);
        if (FAILED(hr)) {
            return false;
        }

        CComBSTR user_name;
        if (!GetCurrentUser(user_name))
            return false;

        if (trigger_type != TRIGGER_TYPE_NOW) {
            // Allow the task to run elevated on startup.
            CComPtr<IPrincipal> principal;
            hr = task->get_Principal(&principal);
            if (FAILED(hr)) {
                return false;
            }

            hr = principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
            if (FAILED(hr)) {
                return false;
            }

            hr = principal->put_UserId(user_name);
            if (FAILED(hr)) {
                return false;
            }

            hr = principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
            if (FAILED(hr)) {
                return false;
            }
        }

        CComPtr<IRegistrationInfo> registration_info;
        hr = task->get_RegistrationInfo(&registration_info);
        if (FAILED(hr)) {
            return false;
        }

        hr = registration_info->put_Author(user_name);
        if (FAILED(hr)) {
            return false;
        }

        CComBSTR description(task_description);
        hr = registration_info->put_Description(description);
        if (FAILED(hr)) {
            return false;
        }

       CComPtr<ITaskSettings> task_settings;
        hr = task->get_Settings(&task_settings);
        if (FAILED(hr)) {
            return false;
        }

        hr = task_settings->put_StartWhenAvailable(VARIANT_TRUE);
        if (FAILED(hr)) {
            return false;
        }

        // TODO(csharp): Find a way to only set this for log upload retry.
        hr = task_settings->put_DeleteExpiredTaskAfter(
            CComBSTR(kZeroMinuteText));
        if (FAILED(hr)) {
            return false;
        }

        hr = task_settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        if (FAILED(hr)) {
            return false;
        }

        hr = task_settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        if (FAILED(hr)) {
            return false;
        }

        if (hidden) {
            hr = task_settings->put_Hidden(VARIANT_TRUE);
            if (FAILED(hr)) {
                return false;
            }
        }

        CComPtr<ITriggerCollection> trigger_collection;
        hr = task->get_Triggers(&trigger_collection);
        if (FAILED(hr)) {
            return false;
        }

        TASK_TRIGGER_TYPE2 task_trigger_type = TASK_TRIGGER_EVENT;
        switch (trigger_type) {
        case TRIGGER_TYPE_POST_REBOOT:
            task_trigger_type = TASK_TRIGGER_LOGON;
            break;
        case TRIGGER_TYPE_NOW:
            task_trigger_type = TASK_TRIGGER_REGISTRATION;
            break;
        case TRIGGER_TYPE_HOURLY:
        case TRIGGER_TYPE_EVERY_SIX_HOURS:
            task_trigger_type = TASK_TRIGGER_DAILY;
            // repetition_interval.Reset(::SysAllocString(kSixHoursText));
            // repetition_interval.Reset(::SysAllocString(kOneHourText));
            break;
        }

        CComPtr<ITrigger> trigger;
        hr = trigger_collection->Create(task_trigger_type, &trigger);
        if (FAILED(hr)) {
            return false;
        }

        if (trigger_type == TRIGGER_TYPE_HOURLY ||
            trigger_type == TRIGGER_TYPE_EVERY_SIX_HOURS) {
            CComQIPtr<IDailyTrigger> daily_trigger(trigger);
            if (!daily_trigger) {
                return false;
            }

            hr = daily_trigger->put_DaysInterval(1);
            if (FAILED(hr)) {
                return false;
            }

            CComPtr<IRepetitionPattern> repetition_pattern;
            hr = trigger->get_Repetition(&repetition_pattern);
            if (FAILED(hr)) {
                return false;
            }

            // The duration is the time to keep repeating until the next daily
            // trigger.
            hr = repetition_pattern->put_Duration(CComBSTR(kTwentyFourHoursText));
            if (FAILED(hr)) {
                return false;
            }

            CComBSTR repetition_interval = kOneHourText;
            hr = repetition_pattern->put_Interval(repetition_interval);
            if (FAILED(hr)) {
                return false;
            }
        }

        if (trigger_type == TRIGGER_TYPE_POST_REBOOT) {
            CComQIPtr<ILogonTrigger> logon_trigger(trigger);
            if (!logon_trigger) {
                return false;
            }

            hr = logon_trigger->put_Delay(CComBSTR(kFifteenMinutesText));
            if (FAILED(hr)) {
                return false;
            }
        }

        hr = trigger->put_StartBoundary(L"2008-10-11T13:21:17Z");
        hr = trigger->put_EndBoundary(L"2028-10-11T13:21:17Z");
        CComPtr<IActionCollection> actions;
        hr = task->get_Actions(&actions);
        if (FAILED(hr)) {
            return false;
        }

        CComPtr<IAction> action;
        hr = actions->Create(TASK_ACTION_EXEC, &action);
        if (FAILED(hr)) {
            return false;
        }

        CComQIPtr<IExecAction> exec_action(action);
        if (!exec_action) {
            return false;
        }

        
        hr = exec_action->put_Path(CComBSTR(application_path));
        if (FAILED(hr)) {
            return false;
        }

        hr = exec_action->put_Arguments(CComBSTR(application_arguments));
        if (FAILED(hr)) {
            return false;
        }

        CComPtr<IRegisteredTask> registered_task;                    
        hr = root_task_folder_->RegisterTaskDefinition(
            CComBSTR(task_name), 
            task, 
            TASK_CREATE,
            CComVariant(user_name),  // Not really input, but API expect non-const.
            kEmptyVariant, 
            TASK_LOGON_NONE,
            kEmptyVariant,
            &registered_task);
        if (FAILED(hr)) {
            return false;
        }

        return true;
    }

private:
    // Return the task with |task_name| and false if not found. |task| can be null
    // when only interested in task's existence.
    bool GetTask(const wchar_t* task_name, IRegisteredTask** task) {
        for (TaskIterator it(root_task_folder_); !it.done(); it.Next()) {
            if (::_wcsicmp(it.name(), task_name) == 0) {
                if (task)
                    *task = it.Detach();
                return true;
            }
        }
        return false;
    }

    class TaskIterator {
    public:
        explicit TaskIterator(ITaskFolder* task_folder) {
            HRESULT hr =
                task_folder->GetTasks(TASK_ENUM_HIDDEN, &tasks_);
            if (FAILED(hr)) {
                done_ = true;
                return;
            }
            hr = tasks_->get_Count(&num_tasks_);
            if (FAILED(hr)) {
                done_ = true;
                return;
            }
            Next();
        }

        // Increment to the next valid item in the task list. Skip entries for
        // which we cannot retrieve a name.
        void Next() {
            task_.Release();
            name_.Empty();
            if (++task_index_ >= num_tasks_) {
                done_ = true;
                return;
            }

            // Note: get_Item uses 1 based indices.
            HRESULT hr = tasks_->get_Item(CComVariant(task_index_ + 1), &task_);
            if (FAILED(hr)) {
                Next();
                return;
            }

            CComBSTR task_name_bstr;
            hr = task_->get_Name(&task_name_bstr);
            if (FAILED(hr)) {
                Next();
                return;
            }
            name_ = CStringW(task_name_bstr ? task_name_bstr : L"");
        }

        // Detach the currently active task and pass ownership to the caller.
        // After this method has been called, the -> operator must no longer be
        // used.
        IRegisteredTask* Detach() { return task_.Detach(); }

        // Provide access to the current task.
        IRegisteredTask* operator->() const {
            IRegisteredTask* result = task_;
            return result;
        }

        const CStringW& name() const { return name_; }
        bool done() const { return done_; }

    private:
        CComPtr<IRegisteredTaskCollection> tasks_;
        CComPtr<IRegisteredTask> task_;
        CStringW name_;
        long task_index_ = -1;  // NOLINT, API requires a long.
        long num_tasks_ = 0;    // NOLINT, API requires a long.
        bool done_ = false;
    };
private:
    ATL::CComPtr<ITaskService> task_service_;
    ATL::CComPtr<ITaskFolder> root_task_folder_;
};


//
TaskScheduler::TaskScheduler()
{

}

TaskScheduler::~TaskScheduler()
{

}


TaskScheduler* CraateTaskScheduler()
{
    return new TaskSchedulerV2();
}
