#pragma once

#include <atlbase.h>
#include <atlstr.h>
#include <vector>

class TaskScheduler
{
public:
    struct TaskExecAction {
        CStringW application_path;
        CStringW working_dir;
        CStringW arguments;
    };

    struct TaskInfo {
        CStringW name;
        CStringW description;
        std::vector<TaskExecAction> exec_actions;
        uint32_t logon_type;
    };

    // The type of trigger to register for this task.
    enum TriggerType {
        // Only run once post-reboot.
        TRIGGER_TYPE_POST_REBOOT = 0,
        // Run right now (mainly for tests).
        TRIGGER_TYPE_NOW = 1,
        // Run every hour.
        TRIGGER_TYPE_HOURLY = 2,
        TRIGGER_TYPE_EVERY_SIX_HOURS = 3,
        TRIGGER_TYPE_MAX,
    };

    // The log-on requirements for a task to be scheduled. Note that a task can
    // have both the interactive and service bit set. In that case the
    // interactive token will be used when available, and a stored password
    // otherwise.
    enum LogonType {
        LOGON_UNKNOWN = 0,
        // Run the task with the user's interactive token when logged in.
        LOGON_INTERACTIVE = 1 << 0,
        // The task will run whether the user is logged in or not using either a
        // user/password specified at registration time, a service account or a
        // service for user (S4U).
        LOGON_SERVICE = 1 << 1,
        // Vista and later only: the task is run as a service for user and as such
        // will be on an invisible desktop.
        LOGON_S4U = 1 << 2,
    };


    ~TaskScheduler();

    virtual bool Initilize() = 0;
    virtual bool UnInitilize() = 0;

    // Delete the task if it exists. No-op if the task doesn't exist. Return false
    // on failure to delete an existing task.
    virtual bool DeleteTask(const wchar_t* task_name) = 0;

    virtual bool IsTaskRegistered(const wchar_t* task_name) = 0;

    // Enable or disable task based on the value of |enabled|. Return true if the
    // task exists and the operation succeeded.
    virtual bool SetTaskEnabled(const wchar_t* task_name, bool enabled) = 0;

    // Return true if task exists and is enabled.
    virtual bool IsTaskEnabled(const wchar_t* task_name) = 0;

    // Return detailed information about a task. Return true if no errors were
    // encountered. On error, the struct is left unmodified.
    virtual bool GetTaskInfo(const wchar_t* task_name, TaskInfo* info) = 0;

    // Register the task to run the specified application and using the given
    // |trigger_type|.
    virtual bool RegisterTask(const wchar_t* task_name,
        const wchar_t* task_description,
        const wchar_t* application_path,
        const wchar_t* application_arguments,
        TriggerType trigger_type,
        bool hidden) = 0;

protected:
    TaskScheduler();
};

TaskScheduler* CraateTaskScheduler();


