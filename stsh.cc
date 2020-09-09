/**
 * File: stsh.cc
 * -------------
 * Defines the entry point of the stsh executable.
 */

#include "stsh-parser/stsh-parse.h"
#include "stsh-parser/stsh-readline.h"
#include "stsh-parser/stsh-parse-exception.h"
#include "stsh-parse-utils.h"
#include "stsh-signal.h"
#include "stsh-job-list.h"
#include "stsh-job.h"
#include "stsh-process.h"
#include <cstring>
#include <iostream>
#include <string>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>  // for fork
#include <signal.h>  // for kill
#include <sys/wait.h>
using namespace std;

static STSHJobList joblist; // the one piece of global data we need so signal handlers can access it

static void bringJobForeground(const command& command);
static void continueJobBackground(const command& command);
static pid_t findProcess(const command& command, const  string& usage);
static void slayProcess(const command& command);
static void haltProcess(const command& command);
static void continueProcess(const command& command);
static bool handleBuiltin(const pipeline& pipeline);

static void toggleSIGCHLDBlock(int how);
static void blockSIGCHLD();
static void unblockSIGCHLD();

static void waitForForegroundJob(pid_t pid);
static void reapProcess(int sig);

static void printCommand(const command& command);


/* -----------------------------------------------------------------
 * built-in commands
 */

static pid_t findProcess(const command& command, const string& usage) {
    if (command.tokens[0] == NULL || command.tokens[2] != NULL) {
        throw STSHException(usage);
    }

    pid_t pid;
    if (command.tokens[1] == NULL) {
        pid = (pid_t)parseNumber(command.tokens[0], usage);
        if (!joblist.containsProcess(pid)){
            throw STSHException("No process with pid " + to_string(pid) + ".");
        }
        
    } else {
        size_t jobNum = parseNumber(command.tokens[0], usage);
        if (!joblist.containsJob(jobNum)) throw STSHException("No job with id of " + to_string(jobNum) + ".");
        size_t processIndex = parseNumber(command.tokens[1], usage);
        if (processIndex >= joblist.getJob(jobNum).getProcesses().size()) throw STSHException("Job " + to_string(jobNum) + " doesn't have a process at index " + to_string(processIndex) + ".");
        
        pid = joblist.getJob(jobNum).getProcesses()[processIndex].getID();
    }

    cout << pid << endl;
    return pid;
}

void bringJobForeground(const command& command) {
    cout << "bring job foreground " << endl;
    if (command.tokens[1] != NULL) throw STSHException("Usage: fg <jobid>.");
    size_t jobNum = parseNumber(command.tokens[0], "Usage: fg <jobid>.");
    if (!joblist.containsJob(jobNum)) {
        printCommand(command);
        throw STSHException(": No such job.");
    }

    STSHJob& job = joblist.getJob(jobNum);
    cout << job << endl;
    pid_t groupID = job.getGroupID();
    if (groupID == 0) throw STSHException("No process running in this job.");

    if (job.getState() == kBackground) {
        blockSIGCHLD();
        job.setState(kForeground);
        kill(-1 * groupID, SIGCONT);
        
        cout << "start waiting" << endl;
        waitForForegroundJob(groupID);
    }
}

static void continueJobBackground(const command& command) {
    if (command.tokens[1] != NULL) throw STSHException("Usage: bg <jobid>.");
    size_t jobNum = parseNumber(command.tokens[0], "Usage: bg <jobid>.");
    if (!joblist.containsJob(jobNum)) {
        printCommand(command);
        throw STSHException(": No such job.");
    }

    const STSHJob& job = joblist.getJob(jobNum);
    if (job.getState() == kBackground && job.getProcess(job.getGroupID()).getState() == kStopped) {
        kill(-1 * job.getGroupID(), SIGCONT);
    }
}

static void slayProcess(const command& command) {
    pid_t pid = findProcess(command, "Usgae: slay <jobid> <index> | <pid>.");
    kill(pid, SIGINT);
}

static void haltProcess(const command& command) {
    pid_t pid = findProcess(command, "Usgae: halt <jobid> <index> | <pid>.");
    if (joblist.getJobWithProcess(pid).getProcess(pid).getState() != kStopped) {
        kill(pid, SIGTSTP);
    }
}

static void continueProcess(const command& command) {
    pid_t pid = findProcess(command, "Usage: cont <jobid> <index> | <pid>.");
    if (joblist.getJobWithProcess(pid).getProcess(pid).getState() != kRunning) {
        kill(pid, SIGCONT);
    }
}




/**
 * Function: handleBuiltin
 * -----------------------
 * Examines the leading command of the provided pipeline to see if
 * it's a shell builtin, and if so, handles and executes it.  handleBuiltin
 * returns true if the command is a builtin, and false otherwise.
 */
static const string kSupportedBuiltins[] = {"quit", "exit", "fg", "bg", "slay", "halt", "cont", "jobs"};
static const size_t kNumSupportedBuiltins = sizeof(kSupportedBuiltins)/sizeof(kSupportedBuiltins[0]);

/* --------------------------------------------------------------------
 * commands and signal manager
 */

static bool handleBuiltin(const pipeline& pipeline) {
  const string& command = pipeline.commands[0].command;
  auto iter = find(kSupportedBuiltins, kSupportedBuiltins + kNumSupportedBuiltins, command);
  if (iter == kSupportedBuiltins + kNumSupportedBuiltins) return false;
  
  size_t index = iter - kSupportedBuiltins;

  switch (index) {
  case 0:
  case 1: exit(0); break;
  case 2: bringJobForeground(pipeline.commands[0]); break;
  case 3: continueJobBackground(pipeline.commands[0]); break;
  case 4: slayProcess(pipeline.commands[0]); break;
  case 5: haltProcess(pipeline.commands[0]); break;
  case 6: continueProcess(pipeline.commands[0]); break;
  case 7: cout << joblist; break;
  default: throw STSHException("Internal Error: Builtin command not supported.");
  }
  return true;
}

static void stopForegroundJob(int sig) {
    if (!joblist.hasForegroundJob()) return;
    STSHJob& fgJob = joblist.getForegroundJob();
    kill(-1 * fgJob.getGroupID(), SIGTSTP);
}

static void killForegroundJob(int sig) {
    if (!joblist.hasForegroundJob()) return;
    STSHJob& fgJob = joblist.getForegroundJob();
    kill(-1 * fgJob.getGroupID(), SIGINT);
}

/**
 * Function: installSignalHandlers
 * -------------------------------
 * Installs user-defined signals handlers for four signals
 * (once you've implemented signal handlers for SIGCHLD, 
 * SIGINT, and SIGTSTP, you'll add more installSignalHandler calls) and 
 * ignores two others.
 *
 * installSignalHandler is a wrapper around a more robust version of the
 * signal function we've been using all quarter.  Check out stsh-signal.cc
 * to see how it works.
 */
static void installSignalHandlers() {
  installSignalHandler(SIGQUIT, [](int sig) {exit(0);  });
  installSignalHandler(SIGCHLD, reapProcess);
  installSignalHandler(SIGINT, killForegroundJob);
  installSignalHandler(SIGTSTP, stopForegroundJob);
  installSignalHandler(SIGTTIN, SIG_IGN);
  installSignalHandler(SIGTTOU, SIG_IGN);
}

/* -----------------------------------------------------------------
 * unblock and unblock signals coming from child process
 */

static void toggleSIGCHLDBlock(int how) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(how, &mask, NULL);
}

static void blockSIGCHLD() {
    toggleSIGCHLDBlock(SIG_BLOCK);
}

static void unblockSIGCHLD() {
    toggleSIGCHLDBlock(SIG_UNBLOCK);
}

/* -----------------------------------------------------------------
 * process control
 */
static void reapProcess(int sig) {
    while (true) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        
        if (pid <= 0) break;
        
        STSHJob& job = joblist.getJobWithProcess(pid);
        STSHProcess& process = job.getProcess(pid);

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            process.setState(kTerminated);
        } else if (WIFSTOPPED(status)) {
            process.setState(kStopped);
        } else if (WIFCONTINUED(status)) {
            process.setState(kRunning);
        } else {
            throw STSHException("Unexpected signal detected.");
        }
        joblist.synchronize(job);
        
    }
}

static void waitForForegroundJob(pid_t grouppid) {
    if (tcsetpgrp(STDIN_FILENO, grouppid) < 0 && errno != ENOTTY) {
         throw STSHException("Error while calling tcsetpgrp.");
    }
    
  
    sigset_t empty;
    sigemptyset(&empty);
    while (joblist.hasForegroundJob()) {
        sigsuspend(&empty);
    }

   
    if (tcsetpgrp(STDIN_FILENO, getpid()) < 0 && errno != ENOTTY) {
        throw STSHException("Error while calling tcsetpgrp.");
    }
   
    unblockSIGCHLD();
}

/* -------------------------------------------------------------------
 * new job created
 */
static void runChildProcess(const command& command) {
    char *arguments[2 + kMaxArguments];
    arguments[0] = (char *)command.command;
    for (int i = 0; i <= kMaxArguments; i++) {
        if (command.tokens[i] == NULL) {
            arguments[i + 1] = NULL;
            break;
        }
        
        arguments[i + 1] = command.tokens[i];
    }

    execvp(arguments[0], arguments);
    cout << arguments[0] << flush;
    cout << ": Command not found." << endl;
    exit(0);  
}

/**
 * Function: createJob
 * -------------------
 * Creates a new job on behalf of the provided pipeline.
 */
static void createJob(const pipeline& p) {
    blockSIGCHLD();
    
    STSHJobState state = (p.background) ? kBackground : kForeground;
    STSHJob& job = joblist.addJob(state);
    pid_t grouppid;
    int n = p.commands.size();
    int input = -1;
    int output = -1;
    
    if (!p.input.empty()) input = open(p.input.c_str(), O_RDONLY | O_CLOEXEC);
    if (!p.output.empty()) output = open(p.output.c_str(), O_WRONLY | O_CLOEXEC | O_CREAT | O_TRUNC, 0644);
    vector<vector<int>> pipes;
    for (int i = 0; i < n - 1; i++) {
        int fds[2];
        pipe2(fds, O_CLOEXEC);

        vector<int> curr;
        curr.push_back(fds[0]);
        curr.push_back(fds[1]);
        pipes.push_back(curr);
    }

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            if (i == 0 && input != -1) dup2(input, STDIN_FILENO);
            else if (i >= 1) dup2(pipes[i - 1][0], STDIN_FILENO);
            else if (i < n - 1) dup2(pipes[i][1], STDOUT_FILENO);
            else if (i == n - 1 && output != -1) dup2(output, STDOUT_FILENO);
            
            unblockSIGCHLD();
            runChildProcess(p.commands[i]);
        }
        
        if (i == 0) grouppid = pid;
        setpgid(pid, grouppid);

        STSHProcess process(pid, p.commands[i]);
        job.addProcess(process);
    }

    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    
    if (state == kForeground){
        waitForForegroundJob(grouppid);
    } else {
        cout << "[" << job.getNum() << "]" << flush;
        for (STSHProcess process : job.getProcesses()) {
            cout << " " << process.getID() << flush;
        }
        cout << " " <<endl;
        unblockSIGCHLD();
    }
}

/**
 * Function: main
 * --------------
 * Defines the entry point for a process running stsh.
 * The main function is little more than a read-eval-print
 * loop (i.e. a repl).  
 */
int main(int argc, char *argv[]) {
    pid_t stshpid = getpid();
  installSignalHandlers();
  rlinit(argc, argv);
  
  while (true) {
    string line;
    if (!readline(line)) break;
    if (line.empty()) continue;
    try {
      pipeline p(line);
      bool builtin = handleBuiltin(p);
      if (!builtin) createJob(p);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
      if (getpid() != stshpid) exit(0); // if exception is thrown from child process, kill it
    }
  }

  return 0;
}

/* ------------------------------------------------------------------
 * helper functions
 */

static void printCommand(const command& command) {
    cout << command.command << flush;
    for (int i = 0; i < kMaxArguments; i++) {
        if (command.tokens[i] == NULL) break;
        cout << " " << command.tokens[i] << flush;
    }
}
