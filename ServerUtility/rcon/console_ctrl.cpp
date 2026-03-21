#include "console_ctrl.h"
#include "commands/cmd_stop.h"
#include "plugin_helpers.h"

#include <windows.h>

namespace ConsoleCtrl
{
    static BOOL WINAPI CtrlHandler(DWORD ctrlType)
    {
        switch (ctrlType)
        {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            LOG_INFO("[ConsoleCtrl] Console control event %lu - requesting graceful shutdown", ctrlType);
            Cmd_Stop::TriggerShutdown();
            return TRUE;
        default:
            return FALSE;
        }
    }

    void Install()
    {
        if (SetConsoleCtrlHandler(CtrlHandler, TRUE))
            LOG_INFO("[ConsoleCtrl] Console control handler installed");
        else
            LOG_WARN("[ConsoleCtrl] SetConsoleCtrlHandler failed (error %lu)", GetLastError());
    }

    void Remove()
    {
        SetConsoleCtrlHandler(CtrlHandler, FALSE);
    }
}
