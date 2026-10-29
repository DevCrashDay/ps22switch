#pragma once

// The emulation and the vector units run on threads of their own. Their translated code calls the game's functions as C++ calls,
// so its stack is as deep as the game's call chains: the 1 MB (Windows) and 8 MB (Linux) a thread gets there are enough, the
// 128 KiB libnx gives a thread on the Switch is not. There the threads are started with the 16 MiB the browser build uses too.

#include <functional>
#include <thread>

#ifdef __SWITCH__
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#endif

class HostThread {
public:
    explicit HostThread(std::function<void()> body) {
#ifdef __SWITCH__
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, kStackSize);
        auto *heap = new std::function<void()>(std::move(body));
        if (pthread_create(&thread_, &attr, [](void *arg) -> void * {
                auto *f = static_cast<std::function<void()> *>(arg);
                (*f)();
                delete f;
                return nullptr;
            }, heap) != 0) {
            std::fprintf(stderr, "[switch] cannot start a thread with a %u KiB stack\n", static_cast<unsigned>(kStackSize >> 10));
            std::abort();
        }
        pthread_attr_destroy(&attr);
#else
        thread_ = std::thread(std::move(body));
#endif
    }
    HostThread(const HostThread &) = delete;
    HostThread &operator=(const HostThread &) = delete;

    void join() {
#ifdef __SWITCH__
        pthread_join(thread_, nullptr);
#else
        thread_.join();
#endif
    }
    void detach() {
#ifdef __SWITCH__
        pthread_detach(thread_);
#else
        thread_.detach();
#endif
    }

private:
#ifdef __SWITCH__
    static constexpr std::size_t kStackSize = 16u << 20;
    pthread_t thread_{};
#else
    std::thread thread_;
#endif
};
