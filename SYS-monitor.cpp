// Requires ncurses. C++17.
// Keys: c (CPU sort), m (MEMORY sort), k (kill PID), r (change refresh timing), q (quit)
// Scroll with arrows/PgUp/PgDn.

#include <sstream>   
#include <thread>    
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <iostream>
#include <map>
#include <numeric>
#include <pwd.h>
#include <set>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <ncurses.h>

using namespace std;
using Clock = chrono::steady_clock;

static long g_clk_tck = sysconf(_SC_CLK_TCK);
static long g_page_size = sysconf(_SC_PAGESIZE);

struct CpuTotals 
{
    uint64_t user=0,nice_=0,system=0,idle=0,iowait=0,irq=0,softirq=0,steal=0,guest=0,guest_nice=0;
    uint64_t sum() const 
    {
        return user+nice_+system+idle+iowait+irq+softirq+steal+guest+guest_nice;
    }
};

struct ProcSample 
{
    int pid=0;
    string name;
    char state='?';
    int ppid=0;
    uid_t uid=0;
    uint64_t utime_ticks=0;
    uint64_t stime_ticks=0;
    uint64_t rss_pages=0;
    uint64_t vsize=0;
    double cpu_pct=0.0;
    double mem_pct=0.0;
};

struct MemInfo 
{
    uint64_t memTotal=0, memFree=0, buffers=0, cached=0, sreclaimable=0, shmem=0, swapTotal=0, swapFree=0;
};

static bool is_digits(const char* s) 
{
    if (s == NULL) 
    {
        return false; 
    }
    while (*s) 
    {
        if (*s < '0' || *s > '9') 
        {
            return false;
        }
        s++;
    }
    return true;
}

static bool read_first_line(const string& path, string& out) {
    int fl = open(path.c_str(), O_RDONLY);
    if (fl<0) 
    	return false;
    char buf[4096];
    ssize_t n = read(fl, buf, sizeof(buf)-1);
    close(fl);
    if (n<=0) 
    	return false;
    buf[n]='\0';
    char* nl = strchr(buf,'\n');
    if (nl) *nl='\0';
    out = buf;
    return true;
}

static bool read_file(const string& path, string& out) 
{
    int fl = open(path.c_str(), O_RDONLY);
    if (fl<0) 
    	return false;
    string s;
    char buf[8192];
    while(true) 
    {
        ssize_t n = read(fl, buf, sizeof(buf));
        if (n<=0) 
        	break;
        s.append(buf, buf+n);
    }
    close(fl);
    out.swap(s);
    return true;
}

static bool parse_cpu_totals(CpuTotals& t) 
{
    string line;
    if (!read_first_line("/proc/stat", line)) 
    	return false;
    string tag;
    istringstream iss(line);
    iss >> tag;
    if (tag != "cpu") 
    	return false;
    iss >> t.user >> t.nice_ >> t.system >> t.idle >> t.iowait >> t.irq >> t.softirq >> t.steal >> t.guest >> t.guest_nice;
    return true;
}

static bool parse_loadavg(double& l1, double& l5, double& l15) 
{
    string s;
    if (!read_first_line("/proc/loadavg", s)) 
    	return false;
    istringstream iss(s);
    iss >> l1 >> l5 >> l15;
    return true;
}

static bool parse_meminfo(MemInfo& m) 
{
    string s;
    if (!read_file("/proc/meminfo", s)) 
    	return false;
    istringstream iss(s);
    string key, unit;
    uint64_t val;
    while (iss >> key >> val >> unit) 
    {
        if (key=="MemTotal:") 
        	m.memTotal = val*1024;
        else if (key=="MemFree:") 
        	m.memFree = val*1024;
        else if (key=="Buffers:") 
        	m.buffers = val*1024;
        else if (key=="Cached:") 
        	m.cached = val*1024;
        else if (key=="SReclaimable:") 
        	m.sreclaimable = val*1024;
        else if (key=="Shmem:") 
        	m.shmem = val*1024;
        else if (key=="SwapTotal:") 
        	m.swapTotal = val*1024;
        else if (key=="SwapFree:") 
        	m.swapFree = val*1024;
    }
    return true;
}

static bool parse_proc_stat(int pid, ProcSample& ps) 
{
    char path[64];
    snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    string s;
    if (!read_first_line(path, s)) 
    	return false;
    size_t l = s.find('(');
    size_t r = s.rfind(')');
    if (l==string::npos || r==string::npos || r<=l) 
    	return false;
    string comm = s.substr(l+1, r-l-1);
    string rest = s.substr(r+2);
    istringstream iss(rest);

    ps.pid = pid;
    ps.name = comm;
    iss >> ps.state >> ps.ppid;
    uint64_t dummy;
    for (int i=5; i<=13; ++i) 
    	iss >> dummy;
    uint64_t utime, stime;
    iss >> utime >> stime;
    ps.utime_ticks = utime;
    ps.stime_ticks = stime;
    for (int i=16; i<=22; ++i) 
    	iss >> dummy;
    uint64_t vsize, rss;
    iss >> vsize >> rss; 
    ps.vsize = vsize;
    ps.rss_pages = rss;
    return true;
}

static bool read_status_uid(int pid, uid_t& uid_out) 
{
    char path[64];
    snprintf(path,sizeof(path),"/proc/%d/status",pid);
    string s;
    if (!read_file(path, s)) 
    	return false;
    size_t pos = s.find("\nUid:");
    if (pos == string::npos) 
    	pos = s.find("Uid:");
    if (pos == string::npos) 
    	return false;
    istringstream iss(s.substr(pos));
    string k;
    uint32_t r,e,sav,fs;
    iss >> k >> r >> e >> sav >> fs;
    uid_out = (uid_t)e;
    return true;
}

static string uid_to_name(uid_t u) 
{
    if (passwd* pw = getpwuid(u)) 
    	return pw->pw_name ? pw->pw_name : to_string(u);
    return to_string(u);
}

enum class SortKey 
{ CPU, MEM };

struct CpuSnapshot 
{
    CpuTotals totals;
    map<int, uint64_t> proc_ticks;
};

struct Options 
{
    SortKey sortKey = SortKey::CPU;
    int offset = 0;  
    double refreshSec = 1.0;
};

static void draw_header(const MemInfo& m, double cpu_total_pct, double l1, double l5, double l15, double refreshSec) 
{
    uint64_t used = m.memTotal - m.memFree - m.buffers - m.cached - m.sreclaimable + m.shmem;
    uint64_t swapUsed = m.swapTotal - m.swapFree;

    mvprintw(0, 0, "SYS-monitor  |  Load: %.2f %.2f %.2f  |  CPU: %5.1f%%  |  Mem: %5.1f%%  (%.1f/%.1f GiB)  |  Swap: %.1f/%.1f GiB  |  Refresh: %.1fs",
        l1, l5, l15,
        cpu_total_pct,
        100.0 * (double)used / (double)m.memTotal,
        used / (1024.0*1024*1024), m.memTotal / (1024.0*1024*1024),
        swapUsed / (1024.0*1024*1024), m.swapTotal / (1024.0*1024*1024),
        refreshSec 
    );
    clrtoeol();
    mvhline(1, 0, 0, COLS);
}

static void draw_table_header(SortKey key) 
{
    mvprintw(2, 0, "PID	 USER           CPU%%    MEM%%      RSS(MiB)   VSZ(MiB)  STATE   NAME  		%s",
        key==SortKey::CPU ? "[Sort: CPU]" : "[Sort: MEM]");
    clrtoeol();
    mvhline(3, 0, 0, COLS);
}

static void clamp(int& v, int lo, int hi) 
{ 
	if (v<lo) 
		v=lo; 
	if (v>hi) 
		v=hi; 
}

static void compute_cpu_mem(vector<ProcSample>& procs, const CpuSnapshot& prev, const CpuTotals& now, const MemInfo& m) 
{
    double total_delta = (double)(now.sum() - prev.totals.sum());
    if (total_delta < 1.0) 
    	total_delta = 1.0;

    for (auto& p : procs) {
        uint64_t ticks = p.utime_ticks + p.stime_ticks;
        auto it = prev.proc_ticks.find(p.pid);
        uint64_t prev_ticks = (it==prev.proc_ticks.end()) ? ticks : it->second;
        double proc_delta = (double)(ticks - prev_ticks);
        p.cpu_pct = 100.0 * proc_delta / total_delta;
        uint64_t rss_bytes = p.rss_pages * (uint64_t)g_page_size;
        p.mem_pct = m.memTotal ? (100.0 * (double)rss_bytes / (double)m.memTotal) : 0.0;
    }
}

static void collect_processes(vector<ProcSample>& out) 
{
    out.clear();
    DIR* d = opendir("/proc");
    if (!d) 
    	return;
    dirent* e;
    while ((e = readdir(d))) 
    {
        if (!is_digits(e->d_name)) 
        	continue;
        int pid = atoi(e->d_name);
        ProcSample ps;
        if (!parse_proc_stat(pid, ps)) 
        	continue;
        read_status_uid(pid, ps.uid);
        out.push_back(move(ps));
    }
    closedir(d);
}

static void build_snapshot(const vector<ProcSample>& procs, const CpuTotals& totals, CpuSnapshot& snap) 
{
    snap.totals = totals;
    snap.proc_ticks.clear();
    for (auto& p : procs) 
    {
        snap.proc_ticks[p.pid] = p.utime_ticks + p.stime_ticks;
    }
}

static void draw_processes(const vector<ProcSample>& procs, int offset) 
{
    int rows = LINES - 5;
    int total = (int)procs.size();
    clamp(offset, 0, max(0, total - rows));

    for (int i=0; i<rows; ++i) {
        int idx = offset + i;
        move(4+i, 0);
        clrtoeol();
        if (idx >= total) 
        	continue;
        const auto& p = procs[idx];
        double rssMiB = (p.rss_pages * (double)g_page_size) / (1024.0*1024.0);
        double vsizeMiB = p.vsize / (1024.0*1024.0);
        string user = uid_to_name(p.uid);
        if ((int)user.size()>12) 
        	user = user.substr(0,12);
        printw("%-8d %-12s %6.2f  %7.3f  %10.1f  %9.1f   %-5c  %s",
               p.pid, user.c_str(), p.cpu_pct, p.mem_pct, rssMiB, vsizeMiB, p.state, p.name.c_str());
    }

    mvhline(LINES-2, 0, 0, COLS);
    mvprintw(LINES-1, 0, "Key:Working |	c:CPU-Sort  m:MEMORY-sort  k:kill  r:refresh  arrows/PgUp/PgDn:Scroll  ESC/q:quit");
    clrtoeol();
}

static void sort_processes(vector<ProcSample>& procs, SortKey key) 
{
    if (key == SortKey::CPU) {
        stable_sort(procs.begin(), procs.end(), [](const auto& a, const auto& b)
        {
            if (a.cpu_pct == b.cpu_pct) 
            	return a.pid < b.pid;
            return a.cpu_pct > b.cpu_pct;
        });
    } 
    else 
    {
        stable_sort(procs.begin(), procs.end(), [](const auto& a, const auto& b)
        {
            if (a.mem_pct == b.mem_pct) 
            	return a.pid < b.pid;
            return a.mem_pct > b.mem_pct;
        });
    }
}

static int prompt_pid_and_kill() {
    echo();
    nodelay(stdscr, FALSE);
    curs_set(1);
    mvprintw(LINES-1, 0, "Enter PID to kill (Enter=SIGTERM, append '!': SIGKILL). Example: 1234 or 1234!");
    clrtoeol();
    char buf[32];
    getnstr(buf, sizeof(buf)-1);
    bool sigkill = false;
    string s(buf);
    if (!s.empty() && s.back()=='!') 
    {
        sigkill = true;
        s.pop_back();
    }
    int pid = 0;
    try 
    { 
    	pid = stoi(s); 
    }catch (...)
    { 
    	pid = 0; 
    }
    int sig = sigkill ? SIGKILL : SIGTERM;
    int rc = -1;
    if (pid > 1) 
    	rc = kill(pid, sig);
    noecho();
    nodelay(stdscr, TRUE);
    curs_set(0);
    return rc;
}

static void prompt_refresh(double& refreshSec) 
{
    echo();
    nodelay(stdscr, FALSE);
    curs_set(1);
    mvprintw(LINES-1, 0, "Enter refresh seconds (0.3 .. 5.0): ");
    clrtoeol();
    char buf[32];
    getnstr(buf, sizeof(buf)-1);
    try 
    {
        double v = stod(string(buf));
        if (v >= 0.3 && v <= 5.0) refreshSec = v;
    } catch (...) {}
    noecho();
    nodelay(stdscr, TRUE);
    curs_set(0);
}

int main() {
    initscr();	// ncurses init
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    Options opt;
    CpuSnapshot prevSnap;
    vector<ProcSample> procs;
    collect_processes(procs);
    CpuTotals t0;
    parse_cpu_totals(t0);
    build_snapshot(procs, t0, prevSnap);

    auto nextTick = Clock::now();

    bool running = true;
    while (running) 
    {
        MemInfo mi{};
        parse_meminfo(mi);

        CpuTotals nowTotals{};
        parse_cpu_totals(nowTotals);

        double l1=0,l5=0,l15=0;
        parse_loadavg(l1,l5,l15);

        collect_processes(procs);
        compute_cpu_mem(procs, prevSnap, nowTotals, mi);
        sort_processes(procs, opt.sortKey);

        double cpu_total_pct = 0.0;
        {
            double total_delta = (double)(nowTotals.sum() - prevSnap.totals.sum());
            double idle_delta  = (double)(nowTotals.idle - prevSnap.totals.idle);
            if (total_delta < 1.0) total_delta = 1.0;
            cpu_total_pct = 100.0 * (1.0 - idle_delta/total_delta);
        }

        erase();
        draw_header(mi, cpu_total_pct, l1, l5, l15, opt.refreshSec);
        draw_table_header(opt.sortKey);
        draw_processes(procs, opt.offset);
        refresh();

        build_snapshot(procs, nowTotals, prevSnap);

        int ch;
        auto until = nextTick + chrono::duration<double>(opt.refreshSec);
        while (Clock::now() < until) 
        {
            ch = getch();
            if (ch == ERR) 
            {
                this_thread::sleep_for(chrono::milliseconds(25));
                continue;
            }
            switch (ch) 
            {
            	case 27 :
                case 'q': 
                	running=false; 
                	break;
                case 'C':
                case 'c': 
                	opt.sortKey = SortKey::CPU; 
                	break;
                case 'M':
                case 'm': 
                	opt.sortKey = SortKey::MEM; 
                	break;
               	case 'K':
                case 'k': 
                	{
                    	int rc = prompt_pid_and_kill();
                    	mvprintw(LINES-1, 0, (rc==0) ? "Signal sent." : "Failed to signal (perm/invalid PID?).");
                    	clrtoeol(); refresh();
                    	this_thread::sleep_for(chrono::milliseconds(600));
                	} break;
                case 'R':
                case 'r': 
                	prompt_refresh(opt.refreshSec); 
                	break;
                case KEY_UP:    
                	opt.offset = std::max(0, opt.offset - 1); 
                	break;
                case KEY_DOWN:  
                	opt.offset = opt.offset + 1;
                	break;
                case KEY_PPAGE: 
                	opt.offset = std::max(0, opt.offset - (LINES-6)); 
                	break;
                case KEY_NPAGE: 
                	opt.offset = opt.offset + (LINES-6); 
                	break;
                default: 
                	break;
            }
        }
        nextTick = Clock::now();
    }
    endwin();
    return 0;
}

