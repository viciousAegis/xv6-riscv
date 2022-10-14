# Scheduling Report

## FCFS

- We run a loop through all the process in the proc table and select the process with the least creation time.
- Creation time is set as the value of `ticks` when allocproc() is called for the process.
- In the scheduler loop, after acquiring process lock, we check if it is the process with the minimum creation time. If it is, we do not release the lock so that other CPUs do not mess with the process
- We release lock only if we find a better process
- Once we have the best pick, we ensure we have a process to schedule, and we context switch to it.
- Preemption is disabled for this scheduler

## LBS
- We assign the default number of tickets(1), while allocating the process.
- We have a syscall `settickets(int number)` which a process can use to increase the number of tickes it has.
- During a `fork()`, a child inherits the number of tickets its parent has.
- Once we reach the scheduler loop, we first run a for loop to determine the total number of tickets we have.
- Then, we use a random number generating function to select a random number between 0 and total tickets.
- We run another for loop, and this time, using a bottom up method to probabilistically select the process, we select a process as a lucky process if the total tickets at that point in the loop are greater than the randomly generated number (Another way to think of it is that, in this scenario, the total tickets collected lie in the ticket range of the lucky process).
If so, we select that process and break.
- We check if we have a process to schedule and the schedule it in the same way described above.

# PBS
- Again, we follow a strategy pretty similar to FCFS. In `allocproc`, we give all processes a static priority of 60.
- We also add variables to log the number of times a process was scheduled to run.
- Next, we write a function update_time which runs in every timer interrupt. It simply iterates through the entire process list and updates the run_time, sleep_time, etc and other variables counting time the process has spent in various states.
- Once we have all the variables tracked, we just implement the logic for the given formulas in the scheduler and make it pick based on the calculated DP value.
- we implement a new syscall which can modify the value of the static_priority value a process has.
- We implement this syscall and then add a simple user program to test it. Tested via `procdump` and works on `init`, `sh`, etc.

## MLFQ
- We create a new `struct` to handle the MLFQ, which includes size, head and tail.
- we use an array of this struct, consisting of 5 elements to represent the 5 queues.
- We implemented the basic queue functions: pop, push and remove in order to manage our queues.
- Now in the MLFQ scheduler, we begin by checking for any processes in RUNNABLE state. If found and they're not already in the queue, we add them to the priority 0 queue.
- Now, in the scheduler, we go in order from the priority 0 queue to the priority 4 queue and execute processes in it in order. If a process was found when iterating in this order, we immediately execute it.
- Now, in the clock_interrupt function, we increment the ticks occupied by one process and if it exceeds its quota then we yield the cpu. Otherwise we let it continue.
- We also check every time whether a process has entered a higher priority queue. if so, we preempt the process out of the queue and yield immediately. 
- If the process relinquishes control of the CPU for any reason without using its entire timeslice, then we put it back in the same queue.
- After this is done, we implement aging in a similar fashion. Before pushing stuff into the queue in scheduler, check if any process has wait time > `AGETICKS` (64). If yes, move it up one priority. We can keep track of wait time again in `update_time` as implemented before.

## Benchmarking
Tested on a single cpu.

  |         Scheduler         | rtime | wtime |
  | :-----------------------: | :-----: | :-----: |
  |        Round robin        |   13    |   152   |
  |  First come first serve   |   26    |   118   |
  |  Lottery based scheduler  |   13    |   149   |
  | Priority based scheduler  |   13    |   126   |
  | Multilevel Feedback queue |   13    |   141   |