#include "CustomEventHandler.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstddef>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "ReplayTask.h"
#include "RecordTask.h"
#include "ReplaySession.h"

#define SOCKET			"/var/run/mem_record"
#define msg_hdr_size()  CMSG_LEN(0)
#define max_fds() ((sizeof(struct fds_t) - offsetof(struct fds_t, fd)) / sizeof(int32_t))

namespace rr {

    static const char* THE_SOCKET = NULL;

    __attribute((constructor))
    static void init_socket_path()
    {
        char* path = getenv("MEM_RECORD_SOCKET");
        THE_SOCKET = path ? path : SOCKET;
    }

    enum
    {
        MSG_FAIL        = 0,
        MSG_SYNC 	   		= 2,
        MSG_SYNC_NOWAIT = 3,
        MSG_LOAD 	   		= 4,
        MSG_INIT 	   		= 5,
        MSG_DEBUG       = 99,
    };

    typedef struct fds_t
    {
        char 	cmsg[msg_hdr_size()];
        int32_t fd[8];
    } fds;

    typedef struct empty_fds_t
    {
        char 	cmsg[msg_hdr_size()];
    } empty_fds;


    static ssize_t send_msg(int32_t socketfd, const char* msg, uint32_t msg_size, struct fds_t* fd_buffer, uint32_t num_fds)
    {
        ssize_t n;
        struct iovec iov;
        struct msghdr hdr;

        memset(&hdr, 0, sizeof(hdr));

        uint32_t fdsize = num_fds * sizeof(int32_t);

        iov.iov_base = (char*)msg;
        iov.iov_len = msg_size;

        hdr.msg_name = NULL;
        hdr.msg_namelen = 0;

        hdr.msg_iov = &iov;
        hdr.msg_iovlen = 1;

        hdr.msg_control = fd_buffer->cmsg;
        hdr.msg_controllen = num_fds ? msg_hdr_size() + fdsize : 0;

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr);
        if(num_fds)
        {
            cmsg->cmsg_len = CMSG_LEN(fdsize);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
        }

        while((n = sendmsg(socketfd, &hdr, 0)) == -1)
        {
            if(errno != EINTR)
            {
                fprintf(stderr, "failed to send message %i %s\n", errno, strerror(errno));
                return -1;
            }
        }
        return n;
    }

    static ssize_t recv_msg(int32_t socketfd, char* buffer, uint32_t buffer_size, struct fds_t* fd_buffer, uint32_t* num_fd_buffer, bool block = true)
    {
        ssize_t n = 0;

        struct iovec iov;
        struct msghdr hdr;

        memset(&hdr, 0, sizeof(hdr));

        iov.iov_base = buffer;
        iov.iov_len = buffer_size;

        hdr.msg_name = NULL;
        hdr.msg_namelen = 0;

        hdr.msg_iov = &iov;
        hdr.msg_iovlen = 1;

        hdr.msg_control = fd_buffer->cmsg;
        hdr.msg_controllen = *num_fd_buffer ? CMSG_LEN(*num_fd_buffer * sizeof(int32_t)) : 0;

        while(1)
        {
            if((n = recvmsg(socketfd, &hdr, MSG_CMSG_CLOEXEC | (block ? 0 : MSG_DONTWAIT))) >= 0)
            {
                break;
            }
            else
            {
                if(errno != EINTR)
                {
                    if(errno != EAGAIN)
                    {
                        fprintf(stderr, "failed to recv message %i %s\n", errno, strerror(errno));
                    }
                    return -1;
                }

            }
        }

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&hdr);
        *num_fd_buffer = hdr.msg_controllen == 0 ? 0 : ((cmsg->cmsg_len - msg_hdr_size()) / sizeof(int32_t));
        return n;
    }

    CustomEventHandler::CustomEventHandler()
        : m_iClientFd(-1)
        , m_iWait(-1)
        , m_pData(reinterpret_cast<Data*>(MAP_FAILED))
    {

    }

    CustomEventHandler::~CustomEventHandler()
    {
        if(m_Thread.joinable())
        {
            pthread_cancel(m_Thread.native_handle());
            m_Thread.join();
        }
        close(m_iClientFd);
        close(m_iWait);

        if(m_pData != reinterpret_cast<Data*>(MAP_FAILED))
        {
            munmap(m_pData, PAGE_SIZE);
        }
    }

    static void run(CustomEventHandler* handler)
    {
        handler->Run();
    }

    void CustomEventHandler::Init(bool record)
    {
        if(m_iClientFd == -1)
        {
            m_iClientFd = do_connect();
            if(record)
            {
                m_iWait = eventfd(0, EFD_CLOEXEC);
            }
            if(m_iClientFd == -1 || (m_iWait == -1 && record))
            {
                close(m_iWait);
                m_iWait = -1;
                fprintf(stderr, "shmem recorder connect failed\n");
                return;
            }

            if(record)
            {
                fds fd_buffer;
                char buffer[1];
                buffer[0] = MSG_INIT;
                uint32_t num_fds = max_fds();

                if(send_msg(m_iClientFd, buffer, 1, &fd_buffer, 0) == -1 ||
                   recv_msg(m_iClientFd, buffer, sizeof(buffer), &fd_buffer, &num_fds) == -1 ||
                   buffer[0] != MSG_INIT ||
                   num_fds != 1 ||
                   (m_pData = reinterpret_cast<Data*>(mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_buffer.fd[0], 0))) == reinterpret_cast<Data*>(MAP_FAILED))
                {
                    fprintf(stderr, "shmem recorder init failed\n");
                    return;
                }
                m_pData->override_pid = -1;
                m_Thread = std::thread(run, this);
            }
        }
    }

    void CustomEventHandler::Wait(WaitArgs& args)
    {
        Session* session = Session::GetSession(this);
        struct pollfd fds[2];
        fds[0].fd = m_iClientFd;
        fds[0].events = POLLIN | POLLHUP;
        fds[1].fd = m_iWait;
        fds[1].events = POLLOUT | POLLHUP;

        if(m_iWait == -1 || session->is_replaying())
        {
            if(args.wait_pid)
            {
            	args.result = waitpid(args.pid, args.stat_loc, args.options);
            }
            else
            {
                args.result = waitid(args.idtype, args.id, args.info, args.options);
            }
            return;
        }

        m_pWait = &args;
        eventfd_write(m_iWait, 0xfffffffffffffffe);

        while(true)
        {
            int32_t r = poll(fds, 2, -1);
            if(r == -1)
            {
                if(errno != EAGAIN && errno != EINTR)
                {
                    fprintf(stderr, "shmem recorder run failed\n");
                    return;
                }
            }
            else
            {
                if((fds[0].revents & POLLIN))
                {
                    HandleMsg();
                }

                if((fds[1].revents & POLLOUT))
                {
                    break;
                }
            }
        }
    }

    void CustomEventHandler::Run()
    {
        eventfd_t v;
        struct pollfd fds;
        fds.fd = m_iWait;
        fds.events = POLLIN | POLLHUP;
        while(true)
        {
            int32_t r = poll(&fds, 1, -1);
            if(r == -1)
            {
                if(errno != EAGAIN && errno != EINTR)
                {
                    fprintf(stderr, "shmem recorder run failed\n");
                    return;
                }
            }

            if(m_pWait->wait_pid)
            {
                m_pWait->result = waitpid(m_pWait->pid, m_pWait->stat_loc, m_pWait->options);
            }
            else
            {
                m_pWait->result = waitid(m_pWait->idtype, m_pWait->id, m_pWait->info, m_pWait->options);
            }
            eventfd_read(m_iWait, &v);
        }
    }

    void CustomEventHandler::HandleMsg()
    {
        Session* session = Session::GetSession(this);
        if(m_iClientFd != -1)
        {
            while(true)
            {
                ssize_t mlen;
                fds fd_buffer;
                char buffer[128];
                uint32_t num_fds = max_fds();
                if((mlen = recv_msg(m_iClientFd, buffer, sizeof(buffer), &fd_buffer, &num_fds, false)) == -1)
                {
                    break;
                }

                switch(buffer[0])
                {
                case MSG_DEBUG: {
                    fprintf(stderr, "%s\n", buffer + 1);
                    break;
                }
                case MSG_SYNC:
                case MSG_SYNC_NOWAIT: {
                    char* end;
                    bool is_wait = buffer[0] == MSG_SYNC;
                    buffer[0] = MSG_SYNC;
                    RecordTask* task = static_cast<RecordTask*>(session->find_task(strtoll(buffer + 1, &end, 10)));
                    if(task)
                    {
                        if(task->is_running())
                        {
                            if (task->ptrace_if_alive(PTRACE_INTERRUPT, nullptr, nullptr)) {

                            }
                        }
                        else
                        {
                        }
                        task->push_event(Event::custom(strtoll(end + 1, &end, 10), strtoll(end + 1, nullptr, 10)));
                    }
                    if(is_wait && send_msg(m_iClientFd, buffer, 1, &fd_buffer, 0) == -1)
                    {
                        fprintf(stderr, "failed to sync shmem recorder event\n");
                    }
                    break;
                }
                }
            }
        }
        else
        {
            fprintf(stderr, "failed to handle shmem recorder message\n");
        }
    }

    void CustomEventHandler::SendEvent(ReplayTask* task, uint64_t custom_index, uint64_t custom_data)
    {
        if(m_iClientFd != -1)
        {
            fds fd_buffer;
            char buffer[128];
            buffer[0] = MSG_LOAD;
            uint32_t num_fds = max_fds();
            uint32_t outFds = 0;

            int32_t memFd = task->map_memFd(remote_ptr<void>(custom_data));
            if(memFd != -1)
            {
                fd_buffer.fd[0] = memFd;
                outFds++;
            }

            size_t mlen = sprintf(buffer + 1, "%" PRIu64 " %d", custom_index, task->real_tgid()) + 1;
            if(send_msg(m_iClientFd, buffer, mlen + 1, &fd_buffer, outFds) == -1 ||
               recv_msg(m_iClientFd, buffer, sizeof(buffer), &fd_buffer, &num_fds) == -1 ||
               buffer[0] != MSG_LOAD)
            {
                goto FAIL;
            }
        }
        else
        {
            FAIL:
            fprintf(stderr, "failed to send shmem recorder event\n");
        }
    }

    int32_t CustomEventHandler::do_connect()
    {
        struct sockaddr_un address;
        int32_t clientFd;
        if((clientFd = socket(PF_LOCAL, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)) == -1)
        {
            fprintf(stderr, "failed to connect\n");
            return -1;
        }

        if(THE_SOCKET == NULL)
        {
            init_socket_path();
        }

        address.sun_family = AF_LOCAL;
        size_t len = strlen(THE_SOCKET);
        if(len + 2 > sizeof(address.sun_path))
        {
            fprintf(stderr, "failed to connect\n");
            return -1;
        }
        strcpy(address.sun_path, THE_SOCKET);
        address.sun_path[len] = 'm';
        address.sun_path[len + 1] = '\0';
        if(connect(clientFd, (struct sockaddr *)&address, sizeof(address)) == -1)
        {
            close(clientFd);
            fprintf(stderr, "failed to connect\n");
            return -1;
        }

        return clientFd;
    }
}
