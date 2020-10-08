//#include "tbb/tbb.h"
#include <asm/unistd.h>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <linux/perf_event.h>
#include <string.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <cstdio>

extern "C" {
#include "jevents.h"
}

#define GLOBAL 1

bool writeHeader = true;

struct PerfEvents {
   size_t counters;
   struct read_format {
      uint64_t value = 0;        /* The value of the event */
      uint64_t time_enabled = 0; /* if PERF_FORMAT_TOTAL_TIME_ENABLED */
      uint64_t time_running = 0; /* if PERF_FORMAT_TOTAL_TIME_RUNNING */
      uint64_t id = 0;           /* if PERF_FORMAT_ID */
   };
   struct event {
      struct perf_event_attr pe;
      int fd;
      read_format prev;
      read_format data;
      double readCounter() {
         return (data.value - prev.value) *
                (double)(data.time_enabled - prev.time_enabled) /
                (data.time_running - prev.time_running);
      }
   };
   std::unordered_map<std::string, std::vector<event>> events;
   std::vector<std::string> ordered_names;

   PerfEvents() {
      if (GLOBAL)
         counters = 1;
      else {
         counters = std::thread::hardware_concurrency();
      }
      char* cpustr = get_cpu_str();
      std::string cpu(cpustr);

      // see https://download.01.org/perfmon/mapfile.csv for cpu strings
      if (cpu == "GenuineIntel-6-55-core") {
         // Skylake X
         add("cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
         add("instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
         add("LLC-M", PERF_TYPE_HW_CACHE,
            (PERF_COUNT_HW_CACHE_LL << 0) |
            (PERF_COUNT_HW_CACHE_OP_READ << 8) |
            (PERF_COUNT_HW_CACHE_RESULT_MISS << 16));
/*         add("L1-DCA", PERF_TYPE_HW_CACHE,
             PERF_COUNT_HW_CACHE_L1D |
             (PERF_COUNT_HW_CACHE_OP_READ << 8) |
             (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16));*/
         add("L1-DCM", PERF_TYPE_HW_CACHE,
             PERF_COUNT_HW_CACHE_L1D |
             (PERF_COUNT_HW_CACHE_OP_READ << 8) |
             (PERF_COUNT_HW_CACHE_RESULT_MISS << 16));
         add("BR-MSP", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES);

         add("all_rd", "offcore_requests.all_data_rd");
         add("stores", "mem_inst_retired.all_stores");
         add("loads", "mem_inst_retired.all_loads");
      } else if (cpu.rfind("GenuineIntel", 0) == 0) {
         // generic intel
         add("cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
         add("instructions", "cpu/instructions/");
         add("LLC-M", PERF_TYPE_HW_CACHE,
            (PERF_COUNT_HW_CACHE_LL << 0) |
            (PERF_COUNT_HW_CACHE_OP_READ << 8) |
            (PERF_COUNT_HW_CACHE_RESULT_MISS << 16));
         add("L1-DCA", PERF_TYPE_HW_CACHE,
             PERF_COUNT_HW_CACHE_L1D |
             (PERF_COUNT_HW_CACHE_OP_READ << 8) |
             (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16));
         add("L1-DCM", PERF_TYPE_HW_CACHE,
             PERF_COUNT_HW_CACHE_L1D |
             (PERF_COUNT_HW_CACHE_OP_READ << 8) |
             (PERF_COUNT_HW_CACHE_RESULT_MISS << 16));
         add("BR-MSP", "cpu/branch-misses/");
      } else if (cpu == "AuthenticAMD-23-8-core") {
         // ZEN
         add("cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
         add("instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
         add("L1-DCA", PERF_TYPE_HW_CACHE,
             PERF_COUNT_HW_CACHE_L1D |
             (PERF_COUNT_HW_CACHE_OP_READ << 8) |
             (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16));
         add("L1-DCM", PERF_TYPE_HW_CACHE,
             PERF_COUNT_HW_CACHE_L1D |
             (PERF_COUNT_HW_CACHE_OP_READ << 8) |
             (PERF_COUNT_HW_CACHE_RESULT_MISS << 16));
         add("BR-MSP", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES);
      }
      add("task-clk", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK);

      registerAll();
   }

   void add(std::string name, uint64_t type, uint64_t eventID) {
      if (getenv("EXTERNALPROFILE")) return;
      ordered_names.push_back(name);
      auto& eventsPerThread = events[name];
      eventsPerThread.assign(counters, event());
      for (auto& event : eventsPerThread) {
         auto& pe = event.pe;
         memset(&pe, 0, sizeof(struct perf_event_attr));
         pe.type = type;
         pe.size = sizeof(struct perf_event_attr);
         pe.config = eventID;
         pe.disabled = true;
         pe.inherit = 1;
         pe.inherit_stat = 0;
         pe.exclude_kernel = true;
         pe.exclude_hv = true;
         pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
      }
   }

   void add(std::string name, std::string str) {
      if (getenv("EXTERNALPROFILE")) return;
      ordered_names.push_back(name);
      auto& eventsPerThread = events[name];
      eventsPerThread.assign(counters, event());
      for (auto& event : eventsPerThread) {
         auto& pe = event.pe;
         memset(&pe, 0, sizeof(struct perf_event_attr));
         if (resolve_event(const_cast<char*>(str.c_str()), &pe) < 0) {
            // empty $HOME/.cache/pmu-events/ ?
            // -> run pmu-tools/event_download.py
            std::cerr << "Error resolving perf event " << str << std::endl;
         }
         pe.disabled = true;
         pe.inherit = 1;
         pe.inherit_stat = 0;
         pe.exclude_kernel = true;
         pe.exclude_hv = true;
         pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
      }
   }

   void registerAll() {
      for (auto& ev : events) {
         size_t i = 0;
         for (auto& event : ev.second) {
            if (GLOBAL)
               event.fd = syscall(__NR_perf_event_open, &event.pe, 0, -1, -1, 0);
            else
               event.fd = syscall(__NR_perf_event_open, &event.pe, 0, i, -1, 0);
            if (event.fd < 0)
               std::cerr << "Error opening perf event " << ev.first << std::endl;
            ++i;
         }
      }
   }

   void startAll() {
      for (auto& ev : events) {
         for (auto& event : ev.second) {
            if (read(event.fd, &event.prev, sizeof(uint64_t) * 3) != sizeof(uint64_t) * 3)
               std::cerr << "Error reading counter " << ev.first << std::endl;
            ioctl(event.fd, PERF_EVENT_IOC_ENABLE, 0);
         }
      }
   }

   ~PerfEvents() {
      for (auto& ev : events)
         for (auto& event : ev.second) close(event.fd);
   }

   void readAll() {
      for (auto& ev : events)
         for (auto& event : ev.second) {
            if (read(event.fd, &event.data, sizeof(uint64_t) * 3) != sizeof(uint64_t) * 3)
               std::cerr << "Error reading counter " << ev.first << std::endl;
            ioctl(event.fd, PERF_EVENT_IOC_DISABLE, 0);
         }
   }

   void printHeader(std::ostream& out) {
      for (auto& name : ordered_names) {
            out << ",";
            printf("%8.8s", name.c_str());
        }
   }

   void printAll(double n) {
      for (auto& name : ordered_names) {
         double aggr = 0;
         for (auto& event : events[name])
            aggr += event.readCounter();
         printf(",%8.2f", aggr / n);
      }
   }

   double operator[](std::string index) {
      double aggr = 0;
      for (auto& event : events[index])
         aggr += event.readCounter();
      return aggr;
   };

   void timeAndProfile(std::string s, uint64_t count, std::function<void()> fn, uint64_t repetitions = 1, std::vector<std::pair<std::string,std::string>>&& extraStrings = {});
};

inline double gettime() {
   struct timeval now_tv;
   gettimeofday(&now_tv, NULL);
   return ((double)now_tv.tv_sec) + ((double)now_tv.tv_usec) / 1000000.0;
}

size_t getCurrentRSS() {
   long rss = 0L;
   FILE* fp = NULL;
   if ((fp = fopen("/proc/self/statm", "r")) == NULL)
      return (size_t)0L;
   if (fscanf(fp, "%*s%ld", &rss) != 1) {
      fclose(fp);
      return (size_t)0L;
   }
   fclose(fp);
   return (size_t)rss * (size_t)sysconf(_SC_PAGESIZE);
}

void PerfEvents::timeAndProfile(std::string s, uint64_t count, std::function<void()> fn, uint64_t repetitions, std::vector<std::pair<std::string,std::string>>&& extraStrings) {
   // warmup round
   if (repetitions>4) {
      fn();
      repetitions--;
   }

   startAll();
   double start = gettime();
   for (size_t i = 0; i < repetitions; ++i) { fn(); }
   double end = gettime();
   readAll();

   // write header
   if (writeHeader) {
      printf("        name,");
      printf("  timems,    MOPS, CPUtime,   IPC,   GHz");
      printHeader(std::cout);
//        if (augFn) { augFn(std::cout, true); }
      for (auto& p : extraStrings) {
         printf(",%8s", p.first.c_str());
      }
      printf("\n");
    }

   double runtime = end - start;

    // print derived values
   printf("%12s,", s.c_str());
   printf("%8.2f,", (runtime * 1e3) / repetitions); // timems
   printf("%8.2f,", ((count * repetitions) / 1e6) / runtime); // MOPS
   printf("%8.2f,", ((*this)["task-clk"] / (runtime * 1e9))); // CPUtime
   printf("%6.2f,", ((*this)["instructions"] / (*this)["cycles"])); // IPC
   printf("%6.2f", ((*this)["cycles"] / (this->events["cycles"][0].data.time_enabled - this->events["cycles"][0].prev.time_enabled))); // GHz
//   printf("%8.0f,", ((((*this)["all_rd"] * 64.0) / (1024 * 1024)) / (end-start)));
   printAll(count * repetitions);
   for (auto& p : extraStrings)
      printf(",%8s", p.second.c_str());

   std::cout << std::endl;
   writeHeader = false;
}
