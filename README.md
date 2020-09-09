### A simple linux shell written in C that supports a collection of builtin comands:
* **quit**, which exits the shell and abandons any jobs that were still running. If there are any extra arguments after the **quit** which exits the shell and abandons any jobs that were still running. If there are any extra arguments after , just can just ignore them. 
* **exit**, which does the same thing as **quit**. Extraneous arguments? Just ignore them.
* **fg**, which prompts a stopped job to continue in the foreground or brings a running background job into the foreground. takes a single job number(e.g. **bg 3**).
* **bg**, which prompts a stopped job to continue in the background. takes a single job number(e.g. **bg 3**). 
* **slay**, which is used to terminate a single process (which may have many sibling processes as part of a larger pipeline). **slay** takes either one or two numeric arguments. If only one number is provided, it's assumed to be the pid of the process to be killed. If two numbers are provided, the first number is assumed to be the job number, and the second is assumed to be a process index within the job.  So, **saly 12345** would terminate, the process with pid 12345. **saly 2 0** would terminate the first process in the pipeline making up job 2. **slay 13 8** would terminate the 9th process in the pipeline of processes making up job 13. 
* **halt**, which has nearly the same story as **slay**, except that its one or two arguments identify a single process that should be halted (but not terminated) if it isn't already stopped. If it's already stopped, then don't do anything and just return.
* **cont**, which has the same story as **slay** and **halt**, except that its one or two arguments identify a single process that should continue if it isn't already running. If it's already running, then don't do anything and just return. When you prompt a single process to continue, you're asking that it do so in the background.
* **jobs**, which prints the job list to the console. If there are any additional arguments, then just ignore them.
* **pipelines**, which supports pipelines consisting of multiple processes.
* **input and output redirection**, which supports input and output redirection via **<** and **>**(e.g. **cat < usr/include/stdio.h | wc > output.txt**). If the file its writing to doesn't exist, create it. If the output file you're redirecting to already exists, then truncate it. 
* **terminal control transfer**, which allows foreground jobs whose leading process(e.g. **cat**, **more**, **emacs**, **vi**,and other executables requiring elaborate control of the console) requires control of the terminal. 


<img width="935" alt="Screen Shot 2020-09-08 at 6 57 32 PM" src="https://user-images.githubusercontent.com/55666152/92545625-66b33400-f205-11ea-9d1b-2b18d76b8dac.png">

