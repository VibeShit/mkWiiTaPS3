#pragma once
#include <atomic>
namespace mkw::trace {
// Where each thread last was (static strings), so a watchdog can report a
// hang: the main loop counts its iterations in progress.
enum Thread {Main,Render,Job,ThreadCount};
inline std::atomic<const char*> where[ThreadCount]{};
inline std::atomic<unsigned> progress{0};
inline void at(Thread thread,const char* place){where[thread].store(place,std::memory_order_relaxed);}
}
