#ifndef CUSTOMEVENTHANDLER_H
#define CUSTOMEVENTHANDLER_H

#include <inttypes.h>
#include <thread>
#include <sys/wait.h>
#include <sys/mman.h>

namespace rr {

    struct WaitArgs
    {
        union
        {
            struct
            {
                pid_t pid;
                int* stat_loc;
            };
            struct
            {
                idtype_t idtype;
                id_t id;
                siginfo_t *info;
            };
        };
        int options;
        int result;
        bool wait_pid;
    };

    class RecordTask;
    class ReplayTask;
    class Session;
    class CustomEventHandler
    {
        struct Data
        {
            pid_t  override_pid;
            size_t exclude_start;
            size_t exclude_len;
        };
    public:
        CustomEventHandler();
        ~CustomEventHandler();
        void Init(bool record);
        void HandleMsg();
        void SendEvent(ReplayTask *task, uint64_t custom_index, uint64_t custom_data);
        void Run();

        void Wait(WaitArgs& args);

        void SetNoWait(pid_t sync_pid = -1, size_t es = 0, size_t el = 0)
        {
            if(m_pData != reinterpret_cast<Data*>(MAP_FAILED))
            {
                m_pData->override_pid = sync_pid;
                m_pData->exclude_start = es;
                m_pData->exclude_len = el;
            }
        }

        bool IsOpen()
        {
            return m_iClientFd != -1;
        }
    private:
        int32_t do_connect();

        int32_t m_iClientFd;
        int32_t m_iWait;
        WaitArgs* m_pWait;
        std::thread m_Thread;
        Data* m_pData;
    };
}

#endif // CUSTOMEVENTHANDLER_H
